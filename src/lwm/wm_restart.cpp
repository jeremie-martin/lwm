/**
 * @file wm_restart.cpp
 * @brief Hot-reload (exec-based binary replacement) for WindowManager
 *
 * Implements state serialization to X properties before exec and state
 * restoration from X properties after exec. This enables seamless WM
 * binary replacement without losing window state.
 *
 * Private atoms used:
 *   _LWM_RESTART_CLIENT  — per-window state (kind, layer, geometry, etc.)
 *   _LWM_RESTART_STATE   — global state (monitor workspaces, focus, etc.)
 *   _LWM_RESTART_TILED_ORDER   — tiled window ordering across workspaces
 *   _LWM_RESTART_FLOATING_ORDER — floating window MRU ordering
 */

#include "lwm/core/log.hpp"
#include "wm.hpp"
#include <algorithm>
#include <cstring>
#include <xcb/xcb.h>

namespace lwm {

namespace {

constexpr uint32_t RESTART_STATE_VERSION = 3;
constexpr uint32_t RESTART_RATIO_STATE_VERSION = 2;
constexpr size_t CLIENT_PROP_BASE_COUNT = 24; // v3: +kind
constexpr size_t CLIENT_PROP_COUNT = 25; // v4: +wm_initiated_urgency

// Double-cast is intentional: int16_t → uint16_t reinterprets the bit pattern (e.g. -100 → 65436),
// then uint16_t → uint32_t zero-extends, preserving the 16-bit pattern without sign-extension.
// On unpack, uint32_t → uint16_t truncates back, then uint16_t → int16_t restores the sign.
void pack_geometry(uint32_t* out, Geometry const& g)
{
    out[0] = static_cast<uint32_t>(static_cast<uint16_t>(g.x));
    out[1] = static_cast<uint32_t>(static_cast<uint16_t>(g.y));
    out[2] = static_cast<uint32_t>(g.width);
    out[3] = static_cast<uint32_t>(g.height);
}

Geometry unpack_geometry(uint32_t const* data)
{
    return {
        static_cast<int16_t>(static_cast<uint16_t>(data[0])),
        static_cast<int16_t>(static_cast<uint16_t>(data[1])),
        static_cast<uint16_t>(data[2]),
        static_cast<uint16_t>(data[3])
    };
}

void pack_optional_geometry(uint32_t* out, std::optional<Geometry> const& g)
{
    if (g.has_value())
    {
        out[0] = 1;
        pack_geometry(out + 1, *g);
    }
    else
    {
        out[0] = 0;
        out[1] = out[2] = out[3] = out[4] = 0;
    }
}

std::optional<Geometry> unpack_optional_geometry(uint32_t const* data)
{
    if (data[0] == 0)
        return std::nullopt;
    return unpack_geometry(data + 1);
}

} // namespace

void WindowManager::initiate_restart(std::string binary)
{
    restarting_ = true;
    restart_binary_ = std::move(binary);
    running_ = false;
}

void WindowManager::serialize_restart_state()
{
    LOG_INFO("Serializing restart state");

    // Per-window: write _LWM_RESTART_CLIENT on each tiled/floating client
    for (auto const& [window, client] : clients_)
    {
        if (client.kind != Client::Kind::Tiled && client.kind != Client::Kind::Floating)
            continue;

        uint32_t data[CLIENT_PROP_COUNT];
        data[0] = (client.layer == WindowLayer::Overlay) ? 1 : 0;
        data[1] = client.borderless ? 1 : 0;
        pack_geometry(data + 2, client.floating_geometry);
        pack_optional_geometry(data + 6, client.fullscreen_restore);
        pack_optional_geometry(data + 11, client.maximize_restore);
        pack_optional_geometry(data + 16, client.float_restore);
        data[21] = client.in_scratchpad ? 1 : 0;
        // 0=none, 1=Tiled, 2=Floating
        data[22] = client.scratchpad_restore_kind.has_value()
            ? (*client.scratchpad_restore_kind == Client::Kind::Tiled ? 1 : 2)
            : 0;
        // v3: current kind (1=Tiled, 2=Floating)
        data[23] = (client.kind == Client::Kind::Tiled) ? 1 : 2;
        // v4: urgency ownership (0=app-owned/none, 1=WM-owned)
        data[24] = client.wm_initiated_urgency ? 1 : 0;

        xcb_change_property(
            conn_.get(),
            XCB_PROP_MODE_REPLACE,
            window,
            lwm_restart_client_,
            XCB_ATOM_CARDINAL,
            32,
            CLIENT_PROP_COUNT,
            data
        );

        // Store scratchpad name as UTF8 string property
        if (client.scratchpad_name.has_value())
        {
            xcb_change_property(
                conn_.get(),
                XCB_PROP_MODE_REPLACE,
                window,
                lwm_restart_scratchpad_name_,
                utf8_string_,
                8,
                static_cast<uint32_t>(client.scratchpad_name->size()),
                client.scratchpad_name->c_str()
            );
        }
    }

    // Scratchpad pool ordering (generic pool window IDs in MRU order)
    if (!scratchpad_pool_.empty())
    {
        std::vector<uint32_t> pool_data;
        pool_data.reserve(scratchpad_pool_.size());
        for (xcb_window_t w : scratchpad_pool_)
            pool_data.push_back(static_cast<uint32_t>(w));

        xcb_change_property(
            conn_.get(),
            XCB_PROP_MODE_REPLACE,
            conn_.screen()->root,
            lwm_restart_scratchpad_pool_,
            XCB_ATOM_WINDOW,
            32,
            static_cast<uint32_t>(pool_data.size()),
            pool_data.data()
        );
    }

    // Global state on root
    size_t global_count = 5 + monitors_.size() * 2;
    std::vector<uint32_t> global(global_count);
    global[0] = RESTART_STATE_VERSION;
    global[1] = static_cast<uint32_t>(focused_monitor_);
    global[2] = static_cast<uint32_t>(active_window_);
    global[3] = showing_desktop_ ? 1 : 0;
    global[4] = static_cast<uint32_t>(monitors_.size());
    for (size_t i = 0; i < monitors_.size(); ++i)
    {
        global[5 + i * 2] = static_cast<uint32_t>(monitors_[i].current_workspace);
        global[6 + i * 2] = static_cast<uint32_t>(monitors_[i].previous_workspace);
    }

    xcb_change_property(
        conn_.get(),
        XCB_PROP_MODE_REPLACE,
        conn_.screen()->root,
        lwm_restart_state_,
        XCB_ATOM_CARDINAL,
        32,
        static_cast<uint32_t>(global_count),
        global.data()
    );

    // Tiled window ordering: iterate monitors, workspaces, append window IDs in order
    std::vector<uint32_t> tiled_order;
    for (auto const& monitor : monitors_)
    {
        for (auto const& workspace : monitor.workspaces)
        {
            for (xcb_window_t w : workspace.windows)
                tiled_order.push_back(static_cast<uint32_t>(w));
        }
    }

    xcb_change_property(
        conn_.get(),
        XCB_PROP_MODE_REPLACE,
        conn_.screen()->root,
        lwm_restart_tiled_order_,
        XCB_ATOM_WINDOW,
        32,
        static_cast<uint32_t>(tiled_order.size()),
        tiled_order.data()
    );

    // Floating window MRU ordering: sort by mru_order to produce ordered list
    std::vector<std::pair<uint64_t, xcb_window_t>> floating_sorted;
    for (auto const& [window, client] : clients_)
    {
        if (client.kind == Client::Kind::Floating)
            floating_sorted.push_back({ client.mru_order, window });
    }
    std::sort(floating_sorted.begin(), floating_sorted.end());

    std::vector<uint32_t> floating_order;
    floating_order.reserve(floating_sorted.size());
    for (auto const& [order, w] : floating_sorted)
        floating_order.push_back(static_cast<uint32_t>(w));

    xcb_change_property(
        conn_.get(),
        XCB_PROP_MODE_REPLACE,
        conn_.screen()->root,
        lwm_restart_floating_order_,
        XCB_ATOM_WINDOW,
        32,
        static_cast<uint32_t>(floating_order.size()),
        floating_order.data()
    );

    // Split ratios per workspace: [version, num_monitors, for each monitor: [num_workspaces,
    //   for each workspace: [num_entries, for each entry: [depth, path, ratio_lo, ratio_hi]]]]
    // Ratio stored as uint64_t bit pattern split into two uint32_t values.
    {
        std::vector<uint32_t> ratio_data;
        ratio_data.push_back(RESTART_RATIO_STATE_VERSION);
        ratio_data.push_back(static_cast<uint32_t>(monitors_.size()));
        for (auto const& monitor : monitors_)
        {
            ratio_data.push_back(static_cast<uint32_t>(monitor.workspaces.size()));
            for (auto const& workspace : monitor.workspaces)
            {
                ratio_data.push_back(static_cast<uint32_t>(workspace.split_ratios.size()));
                for (auto const& [addr, ratio] : workspace.split_ratios)
                {
                    auto serialized_addr = serialize_split_address(addr);
                    ratio_data.push_back(serialized_addr.depth);
                    ratio_data.push_back(serialized_addr.path);
                    // Store double as two uint32_t via bit reinterpretation
                    uint64_t ratio_bits;
                    std::memcpy(&ratio_bits, &ratio, sizeof(double));
                    ratio_data.push_back(static_cast<uint32_t>(ratio_bits & 0xFFFFFFFF));
                    ratio_data.push_back(static_cast<uint32_t>(ratio_bits >> 32));
                }
            }
        }

        xcb_change_property(
            conn_.get(),
            XCB_PROP_MODE_REPLACE,
            conn_.screen()->root,
            lwm_restart_ratios_,
            XCB_ATOM_CARDINAL,
            32,
            static_cast<uint32_t>(ratio_data.size()),
            ratio_data.data()
        );
    }

    conn_.flush();
    LOG_INFO("Restart state serialized ({} clients, {} tiled, {} floating)",
        clients_.size(), tiled_order.size(), floating_order.size());
}

bool WindowManager::restore_global_restart_state()
{
    auto cookie = xcb_get_property(
        conn_.get(), false, conn_.screen()->root,
        lwm_restart_state_, XCB_ATOM_CARDINAL, 0, 1024
    );
    auto* reply = xcb_get_property_reply(conn_.get(), cookie, nullptr);
    if (!reply)
        return false;

    size_t len = xcb_get_property_value_length(reply) / 4;
    if (reply->type != XCB_ATOM_CARDINAL || len < 5)
    {
        free(reply);
        return false;
    }

    LOG_INFO("Restoring global restart state");

    auto* data = static_cast<uint32_t const*>(xcb_get_property_value(reply));
    uint32_t version = data[0];
    if (version != RESTART_STATE_VERSION)
    {
        LOG_WARN("Restart state version mismatch ({} != {}), ignoring", version, RESTART_STATE_VERSION);
        free(reply);
        return false;
    }

    focused_monitor_ = static_cast<size_t>(data[1]);
    active_window_ = static_cast<xcb_window_t>(data[2]);
    showing_desktop_ = data[3] != 0;
    uint32_t num_monitors = data[4];

    // Restore per-monitor workspace selections
    for (uint32_t i = 0; i < num_monitors && i < monitors_.size(); ++i)
    {
        size_t idx = 5 + i * 2;
        if (idx + 1 >= len)
            break;
        size_t ws_count = monitors_[i].workspaces.size();
        monitors_[i].current_workspace = std::min(static_cast<size_t>(data[idx]), ws_count - 1);
        monitors_[i].previous_workspace = std::min(static_cast<size_t>(data[idx + 1]), ws_count - 1);
    }

    // Clamp focused_monitor to valid range
    if (focused_monitor_ >= monitors_.size())
        focused_monitor_ = 0;

    free(reply);

    // Restore split ratios
    {
        auto ratio_cookie = xcb_get_property(
            conn_.get(), false, conn_.screen()->root,
            lwm_restart_ratios_, XCB_ATOM_CARDINAL, 0, 65536
        );
        auto* ratio_reply = xcb_get_property_reply(conn_.get(), ratio_cookie, nullptr);
        if (ratio_reply && ratio_reply->type == XCB_ATOM_CARDINAL)
        {
            size_t rlen = xcb_get_property_value_length(ratio_reply) / 4;
            auto* rdata = static_cast<uint32_t const*>(xcb_get_property_value(ratio_reply));
            size_t pos = 0;

            if (rlen >= 2 && (rdata[0] == 1 || rdata[0] == RESTART_RATIO_STATE_VERSION))
            {
                uint32_t ratio_version = rdata[0];
                pos = 1;
                uint32_t num_monitors_r = rdata[pos++];
                for (uint32_t mi = 0; mi < num_monitors_r && pos < rlen; ++mi)
                {
                    uint32_t num_ws = rdata[pos++];
                    for (uint32_t wi = 0; wi < num_ws && pos < rlen; ++wi)
                    {
                        uint32_t num_entries = rdata[pos++];
                        bool can_apply = mi < monitors_.size() && wi < monitors_[mi].workspaces.size();
                        for (uint32_t ei = 0; ei < num_entries; ++ei)
                        {
                            std::optional<SplitAddress> addr;
                            uint32_t ratio_lo;
                            uint32_t ratio_hi;
                            if (ratio_version == 1)
                            {
                                if (pos + 2 >= rlen)
                                {
                                    pos = rlen;
                                    break;
                                }
                                uint32_t packed_addr = rdata[pos++];
                                ratio_lo = rdata[pos++];
                                ratio_hi = rdata[pos++];
                                addr = SplitAddress {
                                    static_cast<uint8_t>(packed_addr & 0xFF),
                                    (packed_addr >> 8)
                                };
                            }
                            else
                            {
                                if (pos + 3 >= rlen)
                                {
                                    pos = rlen;
                                    break;
                                }
                                uint32_t depth = rdata[pos++];
                                uint32_t path = rdata[pos++];
                                ratio_lo = rdata[pos++];
                                ratio_hi = rdata[pos++];
                                addr = deserialize_split_address(depth, path);
                            }

                            if (!can_apply || !addr.has_value())
                                continue;

                            uint64_t ratio_bits = static_cast<uint64_t>(ratio_lo) | (static_cast<uint64_t>(ratio_hi) << 32);
                            double ratio;
                            std::memcpy(&ratio, &ratio_bits, sizeof(double));

                            if (ratio > 0.0 && ratio < 1.0)
                                monitors_[mi].workspaces[wi].split_ratios[*addr] = ratio;
                        }
                    }
                }
            }
        }
        free(ratio_reply);
    }

    LOG_INFO("Global state restored: focused_monitor={} active_window={:#x} showing_desktop={}",
        focused_monitor_, active_window_, showing_desktop_);
    return true;
}

void WindowManager::apply_restart_client_state(xcb_window_t window)
{
    auto cookie = xcb_get_property(
        conn_.get(), false, window,
        lwm_restart_client_, XCB_ATOM_CARDINAL, 0, CLIENT_PROP_COUNT
    );
    auto* reply = xcb_get_property_reply(conn_.get(), cookie, nullptr);
    if (!reply)
        return;

    size_t len = xcb_get_property_value_length(reply) / 4;
    if (reply->type != XCB_ATOM_CARDINAL || len < CLIENT_PROP_BASE_COUNT)
    {
        free(reply);
        return;
    }

    auto* data = static_cast<uint32_t const*>(xcb_get_property_value(reply));
    auto* client = get_client(window);
    if (!client)
    {
        free(reply);
        return;
    }

    client->layer = (data[0] == 1) ? WindowLayer::Overlay : WindowLayer::Normal;
    client->borderless = data[1] != 0;
    Geometry saved_floating_geometry = unpack_geometry(data + 2);
    client->floating_geometry = saved_floating_geometry;
    client->fullscreen_restore = unpack_optional_geometry(data + 6);
    client->maximize_restore = unpack_optional_geometry(data + 11);
    client->float_restore = unpack_optional_geometry(data + 16);

    // v2 scratchpad fields
    if (len >= 23)
    {
        client->in_scratchpad = data[21] != 0;
        if (data[22] == 1)
            client->scratchpad_restore_kind = Client::Kind::Tiled;
        else if (data[22] == 2)
            client->scratchpad_restore_kind = Client::Kind::Floating;
    }

    // v3: restore window kind — scan_existing_windows reclassifies from scratch,
    // which can turn floating scratchpads into tiled windows.
    std::optional<Client::Kind> saved_kind;
    if (len >= 24)
        saved_kind = (data[23] == 1) ? Client::Kind::Tiled : Client::Kind::Floating;
    if (len >= 25)
        client->wm_initiated_urgency = data[24] != 0;

    free(reply);

    if (saved_kind && *saved_kind == Client::Kind::Floating && client->kind == Client::Kind::Tiled)
    {
        convert_window_to_floating(window);
        // convert_window_to_floating computes fresh geometry from the tiled position,
        // overwriting floating_geometry. Restore the saved value.
        if (auto* c = get_client(window))
            c->floating_geometry = saved_floating_geometry;
    }
    else if (saved_kind && *saved_kind == Client::Kind::Tiled && client->kind == Client::Kind::Floating)
        convert_window_to_tiled(window);

    // Restore scratchpad name
    auto name_cookie = xcb_get_property(
        conn_.get(), false, window,
        lwm_restart_scratchpad_name_, utf8_string_, 0, 256
    );
    auto* name_reply = xcb_get_property_reply(conn_.get(), name_cookie, nullptr);
    if (name_reply && name_reply->type == utf8_string_ && xcb_get_property_value_length(name_reply) > 0)
    {
        client->scratchpad_name = std::string(
            static_cast<char const*>(xcb_get_property_value(name_reply)),
            static_cast<size_t>(xcb_get_property_value_length(name_reply))
        );

        // Re-claim named scratchpad slot
        if (client->scratchpad_name.has_value())
        {
            for (auto& sp : named_scratchpads_)
            {
                if (sp.name == *client->scratchpad_name && sp.window == XCB_NONE)
                {
                    sp.window = window;
                    break;
                }
            }
        }
    }
    free(name_reply);
}

void WindowManager::restore_window_ordering()
{
    LOG_INFO("Restoring window ordering");

    // Pipeline both property reads before collecting replies
    auto tiled_cookie = xcb_get_property(
        conn_.get(), false, conn_.screen()->root,
        lwm_restart_tiled_order_, XCB_ATOM_WINDOW, 0, 65536
    );
    auto float_cookie = xcb_get_property(
        conn_.get(), false, conn_.screen()->root,
        lwm_restart_floating_order_, XCB_ATOM_WINDOW, 0, 65536
    );

    // Restore tiled ordering
    auto* tiled_reply = xcb_get_property_reply(conn_.get(), tiled_cookie, nullptr);
    if (tiled_reply && tiled_reply->type == XCB_ATOM_WINDOW)
    {
        size_t tiled_len = xcb_get_property_value_length(tiled_reply) / 4;
        auto* tiled_data = static_cast<uint32_t const*>(xcb_get_property_value(tiled_reply));

        // Build a priority map: window → position in saved order
        std::unordered_map<xcb_window_t, size_t> tiled_priority;
        for (size_t i = 0; i < tiled_len; ++i)
            tiled_priority[static_cast<xcb_window_t>(tiled_data[i])] = i;

        // Sort each workspace's window vector by saved priority
        for (auto& monitor : monitors_)
        {
            for (auto& workspace : monitor.workspaces)
            {
                std::ranges::sort(workspace.windows, [&](xcb_window_t a, xcb_window_t b)
                {
                    auto ia = tiled_priority.find(a);
                    auto ib = tiled_priority.find(b);
                    size_t pa = (ia != tiled_priority.end()) ? ia->second : SIZE_MAX;
                    size_t pb = (ib != tiled_priority.end()) ? ib->second : SIZE_MAX;
                    return pa < pb;
                });
            }
        }
    }
    free(tiled_reply);

    // Restore floating ordering by assigning mru_order from saved priority
    auto* float_reply = xcb_get_property_reply(conn_.get(), float_cookie, nullptr);

    if (float_reply && float_reply->type == XCB_ATOM_WINDOW)
    {
        size_t float_len = xcb_get_property_value_length(float_reply) / 4;
        auto* float_data = static_cast<uint32_t const*>(xcb_get_property_value(float_reply));

        // Assign mru_order based on saved ordering position
        for (size_t i = 0; i < float_len; ++i)
        {
            auto* client = get_client(static_cast<xcb_window_t>(float_data[i]));
            if (client && client->kind == Client::Kind::Floating)
                client->mru_order = next_mru_order_++;
        }
    }
    free(float_reply);

    // Restore scratchpad pool ordering
    auto pool_cookie = xcb_get_property(
        conn_.get(), false, conn_.screen()->root,
        lwm_restart_scratchpad_pool_, XCB_ATOM_WINDOW, 0, 65536
    );
    auto* pool_reply = xcb_get_property_reply(conn_.get(), pool_cookie, nullptr);
    if (pool_reply && pool_reply->type == XCB_ATOM_WINDOW)
    {
        size_t pool_len = xcb_get_property_value_length(pool_reply) / 4;
        auto* pool_data = static_cast<uint32_t const*>(xcb_get_property_value(pool_reply));

        scratchpad_pool_.clear();
        for (size_t i = 0; i < pool_len; ++i)
        {
            xcb_window_t w = static_cast<xcb_window_t>(pool_data[i]);
            if (get_client(w))
                scratchpad_pool_.push_back(w);
        }
    }
    free(pool_reply);

    LOG_INFO("Window ordering restored");
}

void WindowManager::clean_restart_properties()
{
    LOG_INFO("Cleaning restart properties");

    // Delete global properties from root
    xcb_delete_property(conn_.get(), conn_.screen()->root, lwm_restart_state_);
    xcb_delete_property(conn_.get(), conn_.screen()->root, lwm_restart_tiled_order_);
    xcb_delete_property(conn_.get(), conn_.screen()->root, lwm_restart_floating_order_);
    xcb_delete_property(conn_.get(), conn_.screen()->root, lwm_restart_ratios_);
    xcb_delete_property(conn_.get(), conn_.screen()->root, lwm_restart_scratchpad_pool_);

    // Delete per-window properties
    for (auto const& [window, client] : clients_)
    {
        if (client.kind == Client::Kind::Tiled || client.kind == Client::Kind::Floating)
        {
            xcb_delete_property(conn_.get(), window, lwm_restart_client_);
            xcb_delete_property(conn_.get(), window, lwm_restart_scratchpad_name_);
        }
    }

    conn_.flush();
}

void WindowManager::prepare_restart()
{
    LOG_INFO("Preparing for restart");

    end_drag();
    serialize_restart_state();

    // Move all hidden windows back on-screen so they're recoverable if restart fails.
    // The new WM instance re-tiles everything via scan_existing_windows() → rearrange_all_monitors().
    for (auto& [window, client] : clients_)
    {
        if (!client.hidden)
            continue;
        if (client.kind != Client::Kind::Tiled && client.kind != Client::Kind::Floating)
            continue;

        int16_t restore_x = client.floating_geometry.x;
        if (restore_x <= OFF_SCREEN_X / 2)
            restore_x = 0;

        uint32_t values[] = { static_cast<uint32_t>(static_cast<uint16_t>(restore_x)) };
        xcb_configure_window(conn_.get(), window, XCB_CONFIG_WINDOW_X, values);
    }

    // Ungrab all keys from root and all managed windows
    xcb_ungrab_key(conn_.get(), XCB_GRAB_ANY, conn_.screen()->root, XCB_MOD_MASK_ANY);
    for (auto const& [window, client] : clients_)
        xcb_ungrab_key(conn_.get(), XCB_GRAB_ANY, window, XCB_MOD_MASK_ANY);

    // Ungrab buttons on root and all managed windows
    xcb_ungrab_button(conn_.get(), XCB_BUTTON_INDEX_ANY, conn_.screen()->root, XCB_MOD_MASK_ANY);
    for (auto const& [window, client] : clients_)
        xcb_ungrab_button(conn_.get(), XCB_BUTTON_INDEX_ANY, window, XCB_MOD_MASK_ANY);

    // Clear root event mask (releases SubstructureRedirect so the new WM can claim it)
    uint32_t no_events = XCB_EVENT_MASK_NO_EVENT;
    xcb_change_window_attributes(conn_.get(), conn_.screen()->root, XCB_CW_EVENT_MASK, &no_events);

    // Release WM_S0 selection
    xcb_set_selection_owner(conn_.get(), XCB_NONE, wm_s0_, XCB_CURRENT_TIME);

    // Set RetainPermanent so the X server keeps any lingering resources alive
    // when the connection closes (via CLOEXEC on exec). Standard practice in i3/dwm.
    xcb_set_close_down_mode(conn_.get(), XCB_CLOSE_DOWN_RETAIN_PERMANENT);

    // Destroy our WM windows (both the internal WM window and the EWMH supporting window)
    if (wm_window_ != XCB_NONE)
    {
        xcb_destroy_window(conn_.get(), wm_window_);
        wm_window_ = XCB_NONE;
    }
    ewmh_.destroy_for_restart();

    // Clean up IPC
    cleanup_ipc();

    // Flush all requests and do a round-trip sync to guarantee the X server
    // has fully processed them (especially the SubstructureRedirect release)
    // before we exec into the new binary.
    conn_.flush();
    auto cookie = xcb_get_input_focus(conn_.get());
    free(xcb_get_input_focus_reply(conn_.get(), cookie, nullptr));

    LOG_INFO("Restart preparation complete");
}

} // namespace lwm
