#include "lwm/core/floating.hpp"
#include "lwm/core/focus.hpp"
#include "wm.hpp"
#include <algorithm>
#include <xcb/xcb_icccm.h>

namespace lwm {

void WindowManager::manage_floating_window(xcb_window_t window, bool start_iconic)
{
    auto transient = transient_for_window(window);
    std::optional<size_t> monitor_idx;
    std::optional<size_t> workspace_idx;
    std::optional<Geometry> parent_geom;

    if (transient)
    {
        if (auto const* parent = get_client(*transient);
            parent && parent->monitor < monitors_.size()
            && parent->workspace < monitors_[parent->monitor].workspaces.size())
        {
            monitor_idx = parent->monitor;
            workspace_idx = parent->workspace;
        }

        auto geom_cookie = xcb_get_geometry(conn_.get(), *transient);
        auto* geom_reply = xcb_get_geometry_reply(conn_.get(), geom_cookie, nullptr);
        if (geom_reply)
        {
            parent_geom = Geometry{ geom_reply->x, geom_reply->y, geom_reply->width, geom_reply->height };
            free(geom_reply);
        }
    }

    if (!monitor_idx || !workspace_idx)
    {
        if (auto target = resolve_window_desktop(window))
        {
            monitor_idx = target->first;
            workspace_idx = target->second;
        }
    }

    if (!monitor_idx)
        monitor_idx = focused_monitor_;
    if (!workspace_idx)
        workspace_idx = monitors_[*monitor_idx].current_workspace;

    xcb_size_hints_t size_hints;
    bool has_hints = xcb_icccm_get_wm_normal_hints_reply(
        conn_.get(),
        xcb_icccm_get_wm_normal_hints(conn_.get(), window),
        &size_hints,
        nullptr
    );
    bool has_position_hint = false;
    bool has_size_hint = false;
    int16_t hinted_x = 0;
    int16_t hinted_y = 0;
    uint32_t hinted_width = 0;
    uint32_t hinted_height = 0;
    if (has_hints)
    {
        // Respect user-specified position (US_POSITION) always.
        // For program-specified position (P_POSITION), only use it for non-transient
        // windows — transient windows (dialogs, file pickers) should center on their
        // parent rather than land at whatever fixed coordinate the app requested.
        bool dominated_by_parent = transient.has_value();
        bool user_placed = (size_hints.flags & XCB_ICCCM_SIZE_HINT_US_POSITION) != 0;
        bool program_placed = (size_hints.flags & XCB_ICCCM_SIZE_HINT_P_POSITION) != 0;
        if (user_placed || (program_placed && !dominated_by_parent))
        {
            has_position_hint = true;
            hinted_x = static_cast<int16_t>(size_hints.x);
            hinted_y = static_cast<int16_t>(size_hints.y);
        }
        if (size_hints.flags & (XCB_ICCCM_SIZE_HINT_US_SIZE | XCB_ICCCM_SIZE_HINT_P_SIZE))
        {
            has_size_hint = true;
            hinted_width = static_cast<uint32_t>(size_hints.width);
            hinted_height = static_cast<uint32_t>(size_hints.height);
        }
    }

    uint32_t width = 300;
    uint32_t height = 200;
    auto geom_cookie = xcb_get_geometry(conn_.get(), window);
    auto* geom_reply = xcb_get_geometry_reply(conn_.get(), geom_cookie, nullptr);
    if (geom_reply)
    {
        width = geom_reply->width;
        height = geom_reply->height;
        free(geom_reply);
    }
    if (has_size_hint)
    {
        if (hinted_width > 0)
            width = hinted_width;
        if (hinted_height > 0)
            height = hinted_height;
    }
    if (width == 0)
        width = 300;
    if (height == 0)
        height = 200;
    layout_.apply_size_hints(window, width, height);

    Geometry placement;
    if (has_position_hint)
    {
        placement.x = hinted_x;
        placement.y = hinted_y;
        placement.width = static_cast<uint16_t>(width);
        placement.height = static_cast<uint16_t>(height);
    }
    else
    {
        placement = floating::place_floating(
            monitors_[*monitor_idx].working_area(),
            static_cast<uint16_t>(width),
            static_cast<uint16_t>(height),
            parent_geom
        );
    }

    {
        auto [instance_name, class_name] = get_wm_class(window);
        Client client;
        client.id = window;
        set_floating_state(client, placement);
        client.monitor = *monitor_idx;
        client.workspace = *workspace_idx;
        client.name = get_window_name(window);
        client.wm_class = class_name;
        client.wm_class_name = instance_name;
        client.transient_for = transient.value_or(XCB_NONE);
        client.order = next_client_order_++;
        client.mru_order = next_mru_order_++;
        client.iconic = start_iconic;
        client.ewmh_type = ewmh_.get_window_type_enum(window);
        parse_initial_ewmh_state(client);

        clients_[window] = std::move(client);
    }
    cache_focus_hints(window);

    refresh_user_time_tracking(window);

    uint32_t values[] = { kManagedWindowEventMask };
    xcb_change_window_attributes(conn_.get(), window, XCB_CW_EVENT_MASK, values);

    // Passive button grab for click-to-focus (same as manage_window for tiled).
    xcb_grab_button(
        conn_.get(), 0, window, XCB_EVENT_MASK_BUTTON_PRESS,
        XCB_GRAB_MODE_SYNC, XCB_GRAB_MODE_ASYNC,
        XCB_NONE, XCB_NONE, XCB_BUTTON_INDEX_ANY, XCB_MOD_MASK_ANY
    );

    auto* client = get_client(window);
    if (!client)
        return;

    uint32_t border_width = border_width_for_client(*client);
    xcb_configure_window(conn_.get(), window, XCB_CONFIG_WINDOW_BORDER_WIDTH, &border_width);

    if (wm_state_ != XCB_NONE)
    {
        uint32_t data[] = { start_iconic ? WM_STATE_ICONIC : WM_STATE_NORMAL, 0 };
        xcb_change_property(conn_.get(), XCB_PROP_MODE_REPLACE, window, wm_state_, wm_state_, 32, 2, data);
    }

    if (start_iconic)
    {

        ewmh_.set_window_state(window, ewmh_.get()->_NET_WM_STATE_HIDDEN, true);
    }

    update_sync_state(*client);
    update_fullscreen_monitor_state(*client);

    ewmh_.set_frame_extents(window, 0, 0, 0, 0); // LWM doesn't add frames
    uint32_t desktop = get_ewmh_desktop_index(*monitor_idx, *workspace_idx);
    ewmh_.set_window_desktop(window, desktop);

    update_allowed_actions(*client);

    update_ewmh_client_list();

    keybinds_.grab_keys(window);

    // Before mapping: Apply geometry-affecting states so window appears at correct position
    // (See COMPLIANCE.md "Window State Application Ordering")
    // Fetch all EWMH state flags in a single round-trip
    auto manage_state_flags = ewmh_.get_window_state_flags(window);
    if (manage_state_flags.fullscreen)
    {
        set_fullscreen(*client, true);
    }

    if (manage_state_flags.maximized_horz || manage_state_flags.maximized_vert)
    {
        set_window_maximized(*client, manage_state_flags.maximized_horz, manage_state_flags.maximized_vert);
    }

    // With off-screen visibility: map window once when managing
    xcb_map_window(conn_.get(), window);

    // Sync visibility decides whether to hide or show (and applies floating geometry)
    sync_visibility_for_monitor(*monitor_idx);
    restack_monitor_layers(*monitor_idx);

    if (!start_iconic && !suppress_focus_ && *monitor_idx == focused_monitor_ && should_be_visible(*client))
        focus_any_window(window);

    // AFTER mapping: Apply non-geometry states
    apply_post_manage_states(window, transient.has_value());
}

void WindowManager::unmanage_floating_window(xcb_window_t window)
{
    // Set WM_STATE to Withdrawn before unmanaging (ICCCM)
    if (wm_state_ != XCB_NONE)
    {
        uint32_t data[] = { WM_STATE_WITHDRAWN, 0 };
        xcb_change_property(conn_.get(), XCB_PROP_MODE_REPLACE, window, wm_state_, wm_state_, 32, 2, data);
    }

    // Tracking state that remains outside Client
    pending_kills_.erase(window);

    size_t monitor_idx = 0;
    auto const* client = get_client(window);
    if (!client)
        return; // Not managed
    monitor_idx = client->monitor;

    bool was_active = (active_window_ == window);
    release_fullscreen_owner(*client);

    // Remove from unified Client registry (handles all client state including order)
    clients_.erase(window);

    update_ewmh_client_list();
    if (monitor_idx < monitors_.size())
        update_fullscreen_owner_after_visibility_change(monitor_idx);

    if (was_active)
    {
        if (monitor_idx == focused_monitor_ && monitor_idx < monitors_.size())
        {
            focus_or_fallback(monitors_[monitor_idx]);
        }
        else
        {
            clear_focus();
        }
    }

    flush_and_drain_crossing();
}

bool WindowManager::is_floating_window(xcb_window_t window) const
{
    auto const* client = get_client(window);
    return client && client->kind == Client::Kind::Floating;
}

void WindowManager::update_floating_monitor_for_geometry(Client& client)
{
    if (client.layer == WindowLayer::Overlay)
        return;

    auto const& geom = floating_geometry(client);
    int32_t center_x = static_cast<int32_t>(geom.x) + static_cast<int32_t>(geom.width) / 2;
    int32_t center_y = static_cast<int32_t>(geom.y) + static_cast<int32_t>(geom.height) / 2;
    auto new_monitor =
        focus::monitor_index_at_point(monitors_, static_cast<int16_t>(center_x), static_cast<int16_t>(center_y));
    if (!new_monitor || *new_monitor == client.monitor || *new_monitor >= monitors_.size())
        return;

    if (!move_floating_client_to_workspace(client, *new_monitor, monitors_[*new_monitor].current_workspace, false))
        return;

    if (active_window_ == client.id && is_suppressed_by_fullscreen(client) && client.monitor < monitors_.size())
        focus_or_fallback(monitors_[client.monitor], false);

    flush_and_drain_crossing();
}

void WindowManager::apply_floating_geometry(Client& client)
{
    xcb_window_t window = client.id;

    Geometry geom = floating_geometry(client);
    if (client.layer == WindowLayer::Overlay)
    {
        geom = overlay_geometry_for_client(client);
        floating_geometry(client) = geom;
    }

    uint32_t width = geom.width;
    uint32_t height = geom.height;
    if (client.layer != WindowLayer::Overlay)
        layout_.apply_size_hints(window, width, height);

    send_sync_request(client, last_event_time_);

    int32_t x = static_cast<int32_t>(geom.x);
    int32_t y = static_cast<int32_t>(geom.y);

    uint32_t border_width = border_width_for_client(client);
    uint32_t values[] = { static_cast<uint32_t>(x), static_cast<uint32_t>(y), width, height, border_width };
    uint16_t mask = XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y | XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT
        | XCB_CONFIG_WINDOW_BORDER_WIDTH;
    xcb_configure_window(conn_.get(), window, mask, values);

    xcb_configure_notify_event_t ev = {};
    ev.response_type = XCB_CONFIGURE_NOTIFY;
    ev.event = window;
    ev.window = window;
    ev.x = static_cast<int16_t>(x);
    ev.y = static_cast<int16_t>(y);
    ev.width = static_cast<uint16_t>(width);
    ev.height = static_cast<uint16_t>(height);
    ev.border_width = static_cast<uint16_t>(border_width);
    ev.above_sibling = XCB_NONE;
    ev.override_redirect = 0;

    xcb_send_event(conn_.get(), 0, window, XCB_EVENT_MASK_STRUCTURE_NOTIFY, reinterpret_cast<char*>(&ev));
}

} // namespace lwm
