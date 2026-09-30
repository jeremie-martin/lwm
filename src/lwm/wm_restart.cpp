// Exec handoff through private X properties. Serialization versions and field
// counts below must agree with restore; the properties are consumed after scan.

#include "lwm/core/log.hpp"
#include "lwm/core/restart.hpp"
#include "lwm/core/xproperty.hpp"
#include "wm.hpp"
#include <algorithm>
#include <cstring>
#include <xcb/xcb.h>

namespace lwm {

namespace {

using xproperty::read_words;

} // namespace

void WindowManager::initiate_restart(std::string binary)
{
    restarting_ = true;
    restart_binary_ = std::move(binary);
    running_ = false;
}

void WindowManager::serialize_restart_state()
{
    uint32_t preference_version = 1;
    xcb_change_property(
        conn_.get(),
        XCB_PROP_MODE_REPLACE,
        wm_window_,
        lwm_restart_preferences_,
        XCB_ATOM_CARDINAL,
        32,
        1,
        &preference_version
    );

    // Per-window: write _LWM_RESTART_CLIENT on each tiled/floating client
    for (auto const& [window, client] : clients_)
    {
        if (client.kind() != Client::Kind::Tiled && client.kind() != Client::Kind::Floating)
            continue;

        auto data = restart::encode_client(client);

        xcb_change_property(
            conn_.get(),
            XCB_PROP_MODE_REPLACE,
            window,
            lwm_restart_client_,
            XCB_ATOM_CARDINAL,
            32,
            restart::legacy_client_words,
            data.data()
        );

        // Older readers require exactly the legacy property length. Associate
        // the extension with its writer so an intervening old binary cannot
        // accidentally forward stale preferences that it never maintained.
        std::array<uint32_t, 1 + restart::preference_words> preferences{ wm_window_ };
        std::copy(data.begin() + restart::legacy_client_words, data.end(), preferences.begin() + 1);
        xcb_change_property(
            conn_.get(),
            XCB_PROP_MODE_REPLACE,
            window,
            lwm_restart_preferences_,
            XCB_ATOM_CARDINAL,
            32,
            preferences.size(),
            preferences.data()
        );

        // Store scratchpad name as UTF8 string property
        if (auto const* named = scratchpad_named(client))
        {
            xcb_change_property(
                conn_.get(),
                XCB_PROP_MODE_REPLACE,
                window,
                lwm_restart_scratchpad_name_,
                utf8_string_,
                8,
                static_cast<uint32_t>(named->name.size()),
                named->name.c_str()
            );
        }
    }

    // Scratchpad pool ordering (generic pool window IDs in MRU order)
    if (!scratchpad_pool_.empty())
    {
        std::vector<uint32_t> pool_data;
        pool_data.reserve(scratchpad_pool_.size());
        for (xcb_window_t w : scratchpad_pool_) pool_data.push_back(static_cast<uint32_t>(w));

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
    global[0] = restart::state_version;
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
            for (xcb_window_t w : workspace.windows) tiled_order.push_back(static_cast<uint32_t>(w));
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
        if (client.kind() == Client::Kind::Floating)
            floating_sorted.push_back({ client.mru_order, window });
    }
    std::sort(floating_sorted.begin(), floating_sorted.end());

    std::vector<uint32_t> floating_order;
    floating_order.reserve(floating_sorted.size());
    for (auto const& [order, w] : floating_sorted) floating_order.push_back(static_cast<uint32_t>(w));

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

    // Layout strategy and split ratios per workspace: [version, num_monitors,
    //   for each monitor: [num_workspaces, for each workspace:
    //   [layout_strategy, num_entries, for each entry: [depth, path, ratio_lo, ratio_hi]]]]
    // Ratio stored as uint64_t bit pattern split into two uint32_t values.
    {
        std::vector<uint32_t> ratio_data;
        ratio_data.push_back(restart::ratio_version);
        ratio_data.push_back(static_cast<uint32_t>(monitors_.size()));
        for (auto const& monitor : monitors_)
        {
            ratio_data.push_back(static_cast<uint32_t>(monitor.workspaces.size()));
            for (auto const& workspace : monitor.workspaces)
            {
                ratio_data.push_back(static_cast<uint32_t>(workspace.layout_strategy));
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
    LWM_LOG_INFO(
        "Restart state serialized ({} clients, {} tiled, {} floating)",
        clients_.size(),
        tiled_order.size(),
        floating_order.size()
    );
}

bool WindowManager::restore_global_restart_state()
{
    auto words = read_words(conn_.get(), conn_.screen()->root, lwm_restart_state_, XCB_ATOM_CARDINAL);
    auto record = restart::decode_global(words);
    if (!record)
        return false;
    state_.restore_focus(record->focused_monitor, record->active_window, record->showing_desktop);
    for (size_t i = 0; i < std::min(monitors_.size(), record->workspaces.size()); ++i)
    {
        auto& monitor = monitors_[i];
        state_.restore_workspaces(
            i,
            std::min(record->workspaces[i].first, monitor.workspaces.size() - 1),
            std::min(record->workspaces[i].second, monitor.workspaces.size() - 1)
        );
    }
    auto ratios = read_words(conn_.get(), conn_.screen()->root, lwm_restart_ratios_, XCB_ATOM_CARDINAL);
    for (auto& layout : restart::decode_layouts(ratios))
    {
        if (layout.monitor >= monitors_.size() || layout.workspace >= monitors_[layout.monitor].workspaces.size())
            continue;
        auto& workspace = monitors_[layout.monitor].workspaces[layout.workspace];
        state_.restore_layout(
            layout.monitor,
            layout.workspace,
            layout.strategy.value_or(workspace.layout_strategy),
            std::move(layout.ratios)
        );
    }

    LWM_LOG_INFO(
        "Global state restored: focused_monitor={} active_window={:#x} showing_desktop={}",
        focused_monitor_,
        active_window_,
        showing_desktop_
    );
    return true;
}

void WindowManager::apply_restart_client_state(xcb_window_t window)
{
    auto words = read_words(conn_.get(), window, lwm_restart_client_, XCB_ATOM_CARDINAL, restart::client_words);
    auto preferences =
        read_words(conn_.get(), window, lwm_restart_preferences_, XCB_ATOM_CARDINAL, 1 + restart::preference_words);
    if (words.size() == restart::legacy_client_words && preferences.size() == 1 + restart::preference_words
        && restart_source_ != XCB_NONE && preferences[0] == restart_source_)
        words.insert(words.end(), preferences.begin() + 1, preferences.end());
    auto record = restart::decode_client(words);
    if (!record || !get_client(window))
        return;
    state_.restore_client(window, *record);

    // Restore scratchpad name
    auto name_cookie = xcb_get_property(conn_.get(), false, window, lwm_restart_scratchpad_name_, utf8_string_, 0, 256);
    auto name_reply = xproperty::receive(conn_.get(), name_cookie);
    if (xproperty::complete(name_reply, utf8_string_, 8) && xcb_get_property_value_length(name_reply.get()) > 0)
    {
        std::string scratchpad_name(
            static_cast<char const*>(xcb_get_property_value(name_reply.get())),
            static_cast<size_t>(xcb_get_property_value_length(name_reply.get()))
        );
        state_.scratchpad(window, NamedScratchpadMembership{ scratchpad_name });
    }
}

void WindowManager::restore_window_ordering()
{

    // Pipeline both property reads before collecting replies
    auto tiled_cookie =
        xcb_get_property(conn_.get(), false, conn_.screen()->root, lwm_restart_tiled_order_, XCB_ATOM_WINDOW, 0, 65536);
    auto float_cookie = xcb_get_property(
        conn_.get(),
        false,
        conn_.screen()->root,
        lwm_restart_floating_order_,
        XCB_ATOM_WINDOW,
        0,
        65536
    );

    // Restore tiled ordering
    auto tiled_reply = xproperty::receive(conn_.get(), tiled_cookie);
    auto tiled_data = xproperty::words(tiled_reply, XCB_ATOM_WINDOW);
    if (xproperty::complete(tiled_reply, XCB_ATOM_WINDOW, 32))
    {

        state_.restore_tile_order(tiled_data);
    }

    // Restore floating ordering by assigning mru_order from saved priority
    auto float_reply = xproperty::receive(conn_.get(), float_cookie);
    auto float_data = xproperty::words(float_reply, XCB_ATOM_WINDOW);

    if (xproperty::complete(float_reply, XCB_ATOM_WINDOW, 32))
    {
        size_t float_len = float_data.size();

        // Assign mru_order based on saved ordering position
        for (size_t i = 0; i < float_len; ++i)
        {
            auto* client = get_client(static_cast<xcb_window_t>(float_data[i]));
            if (client && client->kind() == Client::Kind::Floating)
                state_.touch(client->id);
        }
    }

    // Restore scratchpad pool ordering
    auto pool_cookie = xcb_get_property(
        conn_.get(),
        false,
        conn_.screen()->root,
        lwm_restart_scratchpad_pool_,
        XCB_ATOM_WINDOW,
        0,
        65536
    );
    auto pool_reply = xproperty::receive(conn_.get(), pool_cookie);
    auto pool_data = xproperty::words(pool_reply, XCB_ATOM_WINDOW);
    if (xproperty::complete(pool_reply, XCB_ATOM_WINDOW, 32))
    {

        std::vector<xcb_window_t> ids;
        for (auto id : pool_data) ids.push_back(static_cast<xcb_window_t>(id));
        state_.restore_pool(ids);
    }
}

void WindowManager::clean_restart_properties()
{

    // Delete global properties from root
    xcb_delete_property(conn_.get(), conn_.screen()->root, lwm_restart_state_);
    xcb_delete_property(conn_.get(), conn_.screen()->root, lwm_restart_tiled_order_);
    xcb_delete_property(conn_.get(), conn_.screen()->root, lwm_restart_floating_order_);
    xcb_delete_property(conn_.get(), conn_.screen()->root, lwm_restart_ratios_);
    xcb_delete_property(conn_.get(), conn_.screen()->root, lwm_restart_scratchpad_pool_);

    // Delete per-window properties
    for (auto const& [window, client] : clients_)
    {
        if (client.kind() == Client::Kind::Tiled || client.kind() == Client::Kind::Floating)
        {
            xcb_delete_property(conn_.get(), window, lwm_restart_client_);
            xcb_delete_property(conn_.get(), window, lwm_restart_preferences_);
            xcb_delete_property(conn_.get(), window, lwm_restart_scratchpad_name_);
        }
    }

    conn_.flush();
}

void WindowManager::prepare_restart()
{

    end_drag(false);
    serialize_restart_state();

    // Move all hidden windows back on-screen so they're recoverable if restart fails.
    // Startup adoption restores geometry at its completion boundary.
    for (auto& [window, client] : clients_)
    {
        if (!client.presentation.hidden)
            continue;
        if (client.kind() != Client::Kind::Tiled && client.kind() != Client::Kind::Floating)
            continue;

        Geometry restore_geometry = client.kind() == Client::Kind::Floating
            ? floating_geometry(client)
            : prior_floating_geometry(client).value_or(Geometry{});
        int16_t restore_x = restore_geometry.x;
        if (restore_x <= OFF_SCREEN_X / 2)
            restore_x = 0;

        uint32_t values[] = { static_cast<uint32_t>(static_cast<uint16_t>(restore_x)) };
        xcb_configure_window(conn_.get(), window, XCB_CONFIG_WINDOW_X, values);
    }

    // Ungrab all keys from root and all managed windows
    xcb_ungrab_key(conn_.get(), XCB_GRAB_ANY, conn_.screen()->root, XCB_MOD_MASK_ANY);
    for (auto const& [window, client] : clients_) xcb_ungrab_key(conn_.get(), XCB_GRAB_ANY, window, XCB_MOD_MASK_ANY);

    // Ungrab buttons on root and all managed windows
    xcb_ungrab_button(conn_.get(), XCB_BUTTON_INDEX_ANY, conn_.screen()->root, XCB_MOD_MASK_ANY);
    for (auto const& [window, client] : clients_)
        xcb_ungrab_button(conn_.get(), XCB_BUTTON_INDEX_ANY, window, XCB_MOD_MASK_ANY);

    // Clear root event mask (releases SubstructureRedirect so the new WM can claim it)
    uint32_t no_events = XCB_EVENT_MASK_NO_EVENT;
    xcb_change_window_attributes(conn_.get(), conn_.screen()->root, XCB_CW_EVENT_MASK, &no_events);

    // Release WM_S0 selection
    xcb_set_selection_owner(conn_.get(), XCB_NONE, wm_s0_, XCB_CURRENT_TIME);

    // Keep one identifiable predecessor until a replacement connection exists.
    // DestroyAll here would reset an X server whose only client is the WM.
    xcb_change_property(
        conn_.get(),
        XCB_PROP_MODE_REPLACE,
        wm_window_,
        lwm_restart_owner_,
        XCB_ATOM_WINDOW,
        32,
        1,
        &wm_window_
    );
    xcb_change_property(
        conn_.get(),
        XCB_PROP_MODE_REPLACE,
        conn_.screen()->root,
        lwm_restart_owner_,
        XCB_ATOM_WINDOW,
        32,
        1,
        &wm_window_
    );
    xcb_set_close_down_mode(conn_.get(), XCB_CLOSE_DOWN_RETAIN_PERMANENT);

    ewmh_.destroy_for_restart();

    // Clean up IPC
    cleanup_ipc();

    // Flush all requests and do a round-trip sync to guarantee the X server
    // has fully processed them (especially the SubstructureRedirect release)
    // before we exec into the new binary.
    conn_.flush();
    auto cookie = xcb_get_input_focus(conn_.get());
    free(xcb_get_input_focus_reply(conn_.get(), cookie, nullptr));
}

} // namespace lwm
