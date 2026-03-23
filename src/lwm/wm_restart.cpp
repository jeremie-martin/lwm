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
#include <xcb/xcb.h>

namespace lwm {

namespace {

constexpr uint32_t RESTART_STATE_VERSION = 1;
constexpr size_t CLIENT_PROP_COUNT = 21;

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

    // Floating window MRU ordering
    std::vector<uint32_t> floating_order;
    floating_order.reserve(floating_windows_.size());
    for (xcb_window_t w : floating_windows_)
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
    if (reply->type != XCB_ATOM_CARDINAL || len < CLIENT_PROP_COUNT)
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
    client->floating_geometry = unpack_geometry(data + 2);
    client->fullscreen_restore = unpack_optional_geometry(data + 6);
    client->maximize_restore = unpack_optional_geometry(data + 11);
    client->float_restore = unpack_optional_geometry(data + 16);

    free(reply);
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

    // Restore floating ordering
    auto* float_reply = xcb_get_property_reply(conn_.get(), float_cookie, nullptr);

    if (float_reply && float_reply->type == XCB_ATOM_WINDOW)
    {
        size_t float_len = xcb_get_property_value_length(float_reply) / 4;
        auto* float_data = static_cast<uint32_t const*>(xcb_get_property_value(float_reply));

        // Build priority map
        std::unordered_map<xcb_window_t, size_t> float_priority;
        for (size_t i = 0; i < float_len; ++i)
            float_priority[static_cast<xcb_window_t>(float_data[i])] = i;

        // Sort floating_windows_ by saved priority
        std::ranges::sort(floating_windows_, [&](xcb_window_t a, xcb_window_t b)
        {
            auto ia = float_priority.find(a);
            auto ib = float_priority.find(b);
            size_t pa = (ia != float_priority.end()) ? ia->second : SIZE_MAX;
            size_t pb = (ib != float_priority.end()) ? ib->second : SIZE_MAX;
            return pa < pb;
        });
    }
    free(float_reply);

    LOG_INFO("Window ordering restored");
}

void WindowManager::clean_restart_properties()
{
    LOG_INFO("Cleaning restart properties");

    // Delete global properties from root
    xcb_delete_property(conn_.get(), conn_.screen()->root, lwm_restart_state_);
    xcb_delete_property(conn_.get(), conn_.screen()->root, lwm_restart_tiled_order_);
    xcb_delete_property(conn_.get(), conn_.screen()->root, lwm_restart_floating_order_);

    // Delete per-window properties
    for (auto const& [window, client] : clients_)
    {
        if (client.kind == Client::Kind::Tiled || client.kind == Client::Kind::Floating)
            xcb_delete_property(conn_.get(), window, lwm_restart_client_);
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

    // Ungrab buttons on root
    xcb_ungrab_button(conn_.get(), XCB_BUTTON_INDEX_ANY, conn_.screen()->root, XCB_MOD_MASK_ANY);

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
