/**
 * @file wm_events.cpp
 * @brief Event handling implementation for WindowManager
 *
 * This file contains all X11 event handlers. Each handler answers the question
 * "what happens when this event occurs?" and updates window manager state accordingly.
 *
 * Extracted from wm.cpp to improve navigability and local reasoning about event-driven
 * behavior. All functions are methods of WindowManager.
 */

#include "lwm/core/floating.hpp"
#include "lwm/core/log.hpp"
#include "lwm/core/window_rules.hpp"
#include "wm.hpp"
#include <xcb/xcb_icccm.h>

namespace lwm {

namespace {

constexpr uint32_t WM_STATE_ICONIC = 3;

uint32_t extract_event_time(uint8_t response_type, xcb_generic_event_t const& event)
{
    switch (response_type)
    {
        case XCB_KEY_PRESS:
        case XCB_KEY_RELEASE:
            return reinterpret_cast<xcb_key_press_event_t const&>(event).time;
        case XCB_BUTTON_PRESS:
        case XCB_BUTTON_RELEASE:
            return reinterpret_cast<xcb_button_press_event_t const&>(event).time;
        case XCB_MOTION_NOTIFY:
            return reinterpret_cast<xcb_motion_notify_event_t const&>(event).time;
        case XCB_ENTER_NOTIFY:
        case XCB_LEAVE_NOTIFY:
            return reinterpret_cast<xcb_enter_notify_event_t const&>(event).time;
        case XCB_PROPERTY_NOTIFY:
            return reinterpret_cast<xcb_property_notify_event_t const&>(event).time;
        default:
            return 0;
    }
}

}

void WindowManager::handle_event(xcb_generic_event_t const& event)
{
    uint8_t response_type = event.response_type & ~0x80;
    uint32_t event_time = extract_event_time(response_type, event);
    if (event_time != 0)
        last_event_time_ = event_time;

    if (conn_.has_randr() && response_type == conn_.randr_event_base() + XCB_RANDR_SCREEN_CHANGE_NOTIFY)
    {
        handle_randr_screen_change();
        return;
    }

    switch (response_type)
    {
        case XCB_MAP_REQUEST:
            handle_map_request(reinterpret_cast<xcb_map_request_event_t const&>(event));
            break;
        case XCB_UNMAP_NOTIFY:
        {
            auto const& e = reinterpret_cast<xcb_unmap_notify_event_t const&>(event);
            // With off-screen visibility, WM never unmaps windows.
            // Any UnmapNotify is a client-initiated withdraw request - unmanage the window.
            handle_window_removal(e.window);
            break;
        }
        case XCB_DESTROY_NOTIFY:
        {
            auto const& e = reinterpret_cast<xcb_destroy_notify_event_t const&>(event);
            handle_window_removal(e.window);
            break;
        }
        case XCB_ENTER_NOTIFY:
            handle_enter_notify(reinterpret_cast<xcb_enter_notify_event_t const&>(event));
            break;
        case XCB_MOTION_NOTIFY:
            handle_motion_notify(reinterpret_cast<xcb_motion_notify_event_t const&>(event));
            break;
        case XCB_BUTTON_PRESS:
            handle_button_press(reinterpret_cast<xcb_button_press_event_t const&>(event));
            break;
        case XCB_BUTTON_RELEASE:
            handle_button_release(reinterpret_cast<xcb_button_release_event_t const&>(event));
            break;
        case XCB_KEY_PRESS:
        {
            auto const& e = reinterpret_cast<xcb_key_press_event_t const&>(event);
            LOG_TRACE(
                "EVENT: XCB_KEY_PRESS keycode={} time={} state={:#x}",
                static_cast<int>(e.detail),
                e.time,
                e.state
            );
            handle_key_press(e);
            break;
        }
        case XCB_KEY_RELEASE:
        {
            auto const& e = reinterpret_cast<xcb_key_release_event_t const&>(event);
            LOG_TRACE(
                "EVENT: XCB_KEY_RELEASE keycode={} time={} state={:#x}",
                static_cast<int>(e.detail),
                e.time,
                e.state
            );
            handle_key_release(e);
            break;
        }
        case XCB_CLIENT_MESSAGE:
            handle_client_message(reinterpret_cast<xcb_client_message_event_t const&>(event));
            break;
        case XCB_CONFIGURE_REQUEST:
            handle_configure_request(reinterpret_cast<xcb_configure_request_event_t const&>(event));
            break;
        case XCB_PROPERTY_NOTIFY:
            handle_property_notify(reinterpret_cast<xcb_property_notify_event_t const&>(event));
            break;
        case XCB_EXPOSE:
            handle_expose(reinterpret_cast<xcb_expose_event_t const&>(event));
            break;
        case XCB_SELECTION_CLEAR:
        {
            auto const& e = reinterpret_cast<xcb_selection_clear_event_t const&>(event);
            // Another WM is taking over - exit gracefully (ICCCM)
            if (e.selection == wm_s0_)
            {
                running_ = false;
            }
            break;
        }
    }
}

void WindowManager::handle_map_request(xcb_map_request_event_t const& e)
{
    if (auto const* client = get_client(e.window))
    {
        bool focus = client->monitor < monitors_.size() && client->monitor == focused_monitor_
            && client->workspace == monitors_[client->monitor].current_workspace;
        deiconify_window(e.window, focus);
        return;
    }

    if (is_override_redirect_window(e.window))
    {
        return;
    }

    bool has_transient = transient_for_window(e.window).has_value();
    auto classification = ewmh_.classify_window(e.window, has_transient);

    auto [instance_name, class_name] = get_wm_class(e.window);
    std::string title = get_window_name(e.window);
    WindowMatchInfo match_info{ .wm_class = class_name,
                                .wm_class_name = instance_name,
                                .title = title,
                                .ewmh_type = ewmh_.get_window_type_enum(e.window),
                                .is_transient = has_transient };
    auto rule_result = window_rules_.match(match_info, monitors_, config_.workspaces.names);

    if (rule_result.matched && classification.kind != WindowClassification::Kind::Dock
        && classification.kind != WindowClassification::Kind::Desktop
        && classification.kind != WindowClassification::Kind::Popup)
    {
        if (rule_result.floating.has_value())
        {
            classification.kind =
                *rule_result.floating ? WindowClassification::Kind::Floating : WindowClassification::Kind::Tiled;
        }
        if (rule_result.skip_taskbar.has_value())
            classification.skip_taskbar = *rule_result.skip_taskbar;
        if (rule_result.skip_pager.has_value())
            classification.skip_pager = *rule_result.skip_pager;
    }

    bool start_iconic = false;
    bool urgent = false;
    constexpr uint32_t XUrgencyHint = 256; // 1L << 8
    xcb_icccm_wm_hints_t hints;
    if (xcb_icccm_get_wm_hints_reply(conn_.get(), xcb_icccm_get_wm_hints(conn_.get(), e.window), &hints, nullptr))
    {
        if ((hints.flags & XCB_ICCCM_WM_HINT_STATE) && hints.initial_state == XCB_ICCCM_WM_STATE_ICONIC)
        {
            start_iconic = true;
        }
        if (hints.flags & XUrgencyHint)
        {
            urgent = true;
        }
    }
    if (ewmh_.has_window_state(e.window, ewmh_.get()->_NET_WM_STATE_HIDDEN))
    {
        start_iconic = true;
    }

    switch (classification.kind)
    {
        case WindowClassification::Kind::Desktop:
        {
            uint32_t values[] = { XCB_EVENT_MASK_PROPERTY_CHANGE };
            xcb_change_window_attributes(conn_.get(), e.window, XCB_CW_EVENT_MASK, values);
            xcb_map_window(conn_.get(), e.window);
            uint32_t stack_mode = XCB_STACK_MODE_BELOW;
            xcb_configure_window(conn_.get(), e.window, XCB_CONFIG_WINDOW_STACK_MODE, &stack_mode);
            if (std::ranges::find(desktop_windows_, e.window) == desktop_windows_.end())
            {
                desktop_windows_.push_back(e.window);
                Client client;
                client.id = e.window;
                client.kind = Client::Kind::Desktop;
                client.skip_taskbar = true;
                client.skip_pager = true;
                client.order = next_client_order_++;
                clients_[e.window] = std::move(client);
            }
            update_ewmh_client_list();
            conn_.flush();
            return;
        }

        case WindowClassification::Kind::Dock:
        {
            uint32_t values[] = { XCB_EVENT_MASK_ENTER_WINDOW | XCB_EVENT_MASK_POINTER_MOTION
                                  | XCB_EVENT_MASK_PROPERTY_CHANGE };
            xcb_change_window_attributes(conn_.get(), e.window, XCB_CW_EVENT_MASK, values);
            xcb_map_window(conn_.get(), e.window);
            if (std::ranges::find(dock_windows_, e.window) == dock_windows_.end())
            {
                dock_windows_.push_back(e.window);
                Client client;
                client.id = e.window;
                client.kind = Client::Kind::Dock;
                client.skip_taskbar = true;
                client.skip_pager = true;
                client.order = next_client_order_++;
                clients_[e.window] = std::move(client);
            }
            update_struts();
            rearrange_all_monitors();
            update_ewmh_client_list();
            conn_.flush();
            return;
        }

        case WindowClassification::Kind::Popup:
        {
            xcb_map_window(conn_.get(), e.window);
            conn_.flush();
            return;
        }

        case WindowClassification::Kind::Floating:
        {
            manage_floating_window(e.window, start_iconic);

            if (classification.skip_taskbar)
                set_client_skip_taskbar(e.window, true);
            if (classification.skip_pager)
                set_client_skip_pager(e.window, true);
            if (classification.above)
                set_window_above(e.window, true);
            if (urgent)
                set_client_demands_attention(e.window, true);
            if (is_sticky_desktop(e.window) && !is_client_sticky(e.window))
                set_window_sticky(e.window, true);

            if (rule_result.matched)
            {
                auto* client = get_client(e.window);

                if (client && (rule_result.target_monitor.has_value() || rule_result.target_workspace.has_value()))
                {
                    size_t target_mon = rule_result.target_monitor.value_or(client->monitor);
                    size_t target_ws = rule_result.target_workspace.value_or(client->workspace);

                    if (target_mon < monitors_.size())
                    {
                        target_ws = std::min(target_ws, monitors_[target_mon].workspaces.size() - 1);
                        client->monitor = target_mon;
                        client->workspace = target_ws;
                        uint32_t desktop = get_ewmh_desktop_index(target_mon, target_ws);
                        ewmh_.set_window_desktop(e.window, desktop);

                        client->floating_geometry = floating::place_floating(
                            monitors_[target_mon].working_area(),
                            client->floating_geometry.width,
                            client->floating_geometry.height,
                            std::nullopt
                        );
                    }
                }

                if (client && rule_result.geometry.has_value())
                {
                    client->floating_geometry = *rule_result.geometry;
                }

                if (client && rule_result.center)
                {
                    size_t mon = client->monitor;
                    if (mon < monitors_.size())
                    {
                        auto area = monitors_[mon].working_area();
                        client->floating_geometry.x =
                            area.x + static_cast<int16_t>((area.width - client->floating_geometry.width) / 2);
                        client->floating_geometry.y =
                            area.y + static_cast<int16_t>((area.height - client->floating_geometry.height) / 2);
                    }
                }

                if (rule_result.above.has_value() && *rule_result.above)
                    set_window_above(e.window, true);
                if (rule_result.below.has_value() && *rule_result.below)
                    set_window_below(e.window, true);
                if (rule_result.sticky.has_value() && *rule_result.sticky)
                    set_window_sticky(e.window, true);
                if (rule_result.fullscreen.has_value() && *rule_result.fullscreen)
                    set_fullscreen(e.window, true);

                // Update visibility after placement changes
                if (client)
                    update_floating_visibility(client->monitor);
            }
            return;
        }

        case WindowClassification::Kind::Tiled:
        {
            manage_window(e.window, start_iconic);

            if (urgent)
                set_client_demands_attention(e.window, true);
            if (is_sticky_desktop(e.window) && !is_client_sticky(e.window))
                set_window_sticky(e.window, true);

            if (rule_result.matched)
            {
                auto* client = get_client(e.window);

                if (client && (rule_result.target_monitor.has_value() || rule_result.target_workspace.has_value()))
                    if (client && (rule_result.target_monitor.has_value() || rule_result.target_workspace.has_value()))
                    {
                        size_t source_mon = client->monitor;
                        size_t source_ws = client->workspace;
                        size_t target_mon = rule_result.target_monitor.value_or(source_mon);
                        size_t target_ws = rule_result.target_workspace.value_or(source_ws);

                        if (target_mon < monitors_.size())
                        {
                            target_ws = std::min(target_ws, monitors_[target_mon].workspaces.size() - 1);

                            if (target_mon != source_mon || target_ws != source_ws)
                            {
                                auto& source_workspace = monitors_[source_mon].workspaces[source_ws];
                                auto it = source_workspace.find_window(e.window);
                                if (it != source_workspace.windows.end())
                                {
                                    source_workspace.windows.erase(it);
                                    if (source_workspace.focused_window == e.window)
                                        source_workspace.focused_window = XCB_NONE;
                                }

                                auto& target_workspace = monitors_[target_mon].workspaces[target_ws];
                                target_workspace.windows.push_back(e.window);

                                client->monitor = target_mon;
                                client->workspace = target_ws;
                                uint32_t desktop = get_ewmh_desktop_index(target_mon, target_ws);
                                ewmh_.set_window_desktop(e.window, desktop);

                                rearrange_monitor(monitors_[source_mon]);
                                if (target_mon != source_mon)
                                    rearrange_monitor(monitors_[target_mon]);

                                if (target_ws != monitors_[target_mon].current_workspace)
                                    hide_window(e.window);
                            }
                        }
                    }

                if (rule_result.above.has_value() && *rule_result.above)
                    set_window_above(e.window, true);
                if (rule_result.below.has_value() && *rule_result.below)
                    set_window_below(e.window, true);
                if (rule_result.sticky.has_value() && *rule_result.sticky)
                    set_window_sticky(e.window, true);
                if (rule_result.fullscreen.has_value() && *rule_result.fullscreen)
                    set_fullscreen(e.window, true);
            }

            if (!start_iconic)
            {
                if (auto const* client = get_client(e.window))
                {
                    if (client->monitor < monitors_.size() && client->monitor == focused_monitor_
                        && client->workspace == monitors_[client->monitor].current_workspace)
                    {
                        focus_any_window(e.window);
                    }
                }
            }
            return;
        }
    }
}

void WindowManager::handle_window_removal(xcb_window_t window)
{
    unmanage_dock_window(window);
    unmanage_desktop_window(window);
    unmanage_floating_window(window);
    unmanage_window(window);
}

void WindowManager::handle_enter_notify(xcb_enter_notify_event_t const& e)
{
    LOG_TRACE(
        "EVENT: EnterNotify event={:#x} root_x={} root_y={} mode={} detail={} time={}",
        e.event,
        e.root_x,
        e.root_y,
        static_cast<int>(e.mode),
        static_cast<int>(e.detail),
        e.time
    );

    if (drag_state_.active)
    {
        LOG_TRACE("EnterNotify: ignored (drag active)");
        return;
    }

    if (e.event != conn_.screen()->root)
    {
        if (e.mode != XCB_NOTIFY_MODE_NORMAL || e.detail == XCB_NOTIFY_DETAIL_INFERIOR)
        {
            LOG_TRACE(
                "EnterNotify: filtered (mode={} detail={})",
                static_cast<int>(e.mode),
                static_cast<int>(e.detail)
            );
            return;
        }

        if (e.mode != XCB_NOTIFY_MODE_NORMAL)
        {
            LOG_TRACE("EnterNotify: filtered root (mode={})", static_cast<int>(e.mode));
            return;
        }
    }

    if (e.event != conn_.screen()->root)
    {
        if (auto const* client = get_client(e.event); client && client->hidden)
        {
            LOG_TRACE("EnterNotify: ignored (window is hidden)");
            return;
        }

        if (is_floating_window(e.event) || monitor_containing_window(e.event))
        {
            LOG_DEBUG("EnterNotify: focusing window {:#x}", e.event);
            focus_any_window(e.event);
            return;
        }
    }

    // Case 2: Entering root or unmanaged window area (gaps/empty space)
    LOG_TRACE("EnterNotify: updating focused monitor at ({}, {})", e.root_x, e.root_y);
    update_focused_monitor_at_point(e.root_x, e.root_y);
}

void WindowManager::handle_motion_notify(xcb_motion_notify_event_t const& e)
{
    if (drag_state_.active)
    {
        update_drag(e.root_x, e.root_y);
        return;
    }

    // Determine which window the pointer is over.
    // Motion events come to root (which selects POINTER_MOTION), with e.child
    // indicating any managed window under the cursor.
    xcb_window_t window_under_cursor = (e.event == conn_.screen()->root && e.child != XCB_NONE) ? e.child : e.event;

    // Focus-follows-mouse on motion: if motion occurs within a managed window
    // that is not currently focused, focus it. This handles the case where a
    // new window took focus (per EWMH compliance) but the cursor remained in
    // another window. Moving the mouse within that window re-establishes focus.
    if (window_under_cursor != conn_.screen()->root)
    {
        if (auto const* client = get_client(window_under_cursor); client && client->hidden)
            return;

        if (is_floating_window(window_under_cursor) || monitor_containing_window(window_under_cursor))
        {
            if (window_under_cursor != active_window_)
            {
                LOG_DEBUG("MotionNotify: focusing window {:#x} (was {:#x})", window_under_cursor, active_window_);
                focus_any_window(window_under_cursor);
            }
            return;
        }
    }

    update_focused_monitor_at_point(e.root_x, e.root_y);
}

WindowManager::MouseBinding const* WindowManager::resolve_mouse_binding(uint16_t state, uint8_t button) const
{
    uint16_t clean_mod = state & ~(XCB_MOD_MASK_LOCK | XCB_MOD_MASK_2);
    for (auto const& binding : mousebinds_)
    {
        if (binding.button == button && binding.modifier == clean_mod)
            return &binding;
    }
    return nullptr;
}

void WindowManager::handle_button_press(xcb_button_press_event_t const& e)
{
    xcb_window_t target = e.event;
    if (target == conn_.screen()->root && e.child != XCB_NONE)
        target = e.child;

    // Ignore button press on hidden (off-screen) windows
    if (auto const* client = get_client(target); client && client->hidden)
        return;

    if (auto const* binding = resolve_mouse_binding(e.state, e.detail))
    {
        if (binding->action == "drag_window")
        {
            if (is_floating_window(target))
            {
                focus_any_window(target);
                begin_drag(target, false, e.root_x, e.root_y);
                return;
            }
            if (monitor_containing_window(target))
            {
                focus_any_window(target);
                begin_tiled_drag(target, e.root_x, e.root_y);
                return;
            }
        }
        else if (binding->action == "resize_floating")
        {
            if (is_floating_window(target))
            {
                focus_any_window(target);
                begin_drag(target, true, e.root_x, e.root_y);
                return;
            }
        }
    }

    if (target != conn_.screen()->root)
    {
        if (is_floating_window(target) || monitor_containing_window(target))
        {
            if (target != active_window_)
                focus_any_window(target);
            return;
        }
    }

    // Update focused monitor based on click position (for clicks on empty space)
    update_focused_monitor_at_point(e.root_x, e.root_y);
}

void WindowManager::handle_button_release(xcb_button_release_event_t const& e)
{
    if (!drag_state_.active)
        return;

    drag_state_.last_root_x = e.root_x;
    drag_state_.last_root_y = e.root_y;
    end_drag();
}

void WindowManager::handle_key_press(xcb_key_press_event_t const& e)
{
    xcb_keysym_t keysym = xcb_key_press_lookup_keysym(conn_.keysyms(), const_cast<xcb_key_press_event_t*>(&e), 0);

    LOG_KEY(e.state, keysym);

    auto action = keybinds_.resolve(e.state, keysym);
    if (!action)
    {
        LOG_TRACE("No action for keysym");
        return;
    }

    LOG_DEBUG("Action: {}", action->type);

    if (action->type == "kill" && active_window_ != XCB_NONE)
    {
        kill_window(active_window_);
    }
    else if (action->type == "switch_workspace" && action->workspace >= 0)
    {
        switch_workspace(action->workspace);
    }
    else if (action->type == "toggle_workspace")
    {
        LOG_TRACE(
            "KeyPress: keysym={:#x} time={} last_keysym={:#x} last_release_time={}",
            keysym,
            e.time,
            last_toggle_keysym_,
            last_toggle_release_time_
        );
        bool same_key = (keysym == last_toggle_keysym_);
        bool same_time = (e.time == last_toggle_release_time_);
        LOG_TRACE("check: same_key={} same_time={} would_block={}", same_key, same_time, (same_key && same_time));
        if (same_key && same_time)
        {
            LOG_TRACE("BLOCKED (auto-repeat detected)");
            return;
        }
        last_toggle_keysym_ = keysym;
        last_toggle_release_time_ = 0;
        toggle_workspace();
        LOG_TRACE("toggle_workspace() returned");
    }
    else if (action->type == "move_to_workspace" && action->workspace >= 0)
    {
        move_window_to_workspace(action->workspace);
    }
    else if (action->type == "focus_monitor_left")
    {
        focus_monitor(-1);
    }
    else if (action->type == "focus_monitor_right")
    {
        focus_monitor(1);
    }
    else if (action->type == "move_to_monitor_left")
    {
        move_window_to_monitor(-1);
    }
    else if (action->type == "move_to_monitor_right")
    {
        move_window_to_monitor(1);
    }
    else if (action->type == "spawn")
    {
        launch_program(keybinds_.resolve_command(action->command, config_));
    }
    else if (action->type == "toggle_fullscreen")
    {
        if (active_window_ != XCB_NONE)
        {
            set_fullscreen(active_window_, !is_client_fullscreen(active_window_));
        }
    }
    else if (action->type == "focus_next")
    {
        focus_next();
    }
    else if (action->type == "focus_prev")
    {
        focus_prev();
    }
}

void WindowManager::handle_key_release(xcb_key_release_event_t const& e)
{
    xcb_keysym_t keysym = xcb_key_press_lookup_keysym(conn_.keysyms(), const_cast<xcb_key_release_event_t*>(&e), 0);

    LOG_TRACE("KeyRelease: keysym={:#x} time={} last_toggle_keysym={:#x}", keysym, e.time, last_toggle_keysym_);

    // Record timestamp for auto-repeat detection.
    // X11 auto-repeat sends KeyRelease-KeyPress pairs with identical timestamps.
    if (keysym == last_toggle_keysym_)
    {
        LOG_TRACE("KeyRelease matches toggle key, recording time={}", e.time);
        last_toggle_release_time_ = e.time;
    }
}

void WindowManager::handle_client_message(xcb_client_message_event_t const& e)
{
    xcb_ewmh_connection_t* ewmh = ewmh_.get();

    if (e.type == wm_protocols_ && e.data.data32[0] == net_wm_ping_)
    {
        xcb_window_t window = static_cast<xcb_window_t>(e.data.data32[2]);
        if (window == XCB_NONE)
            window = e.window;
        pending_pings_.erase(window);
        pending_kills_.erase(window);
        return;
    }

    if (e.type == net_close_window_)
    {
        kill_window(e.window);
        return;
    }

    if (e.type == net_wm_fullscreen_monitors_)
    {
        FullscreenMonitors monitors;
        monitors.top = e.data.data32[0];
        monitors.bottom = e.data.data32[1];
        monitors.left = e.data.data32[2];
        monitors.right = e.data.data32[3];
        set_fullscreen_monitors(e.window, monitors);
        return;
    }

    if (e.type == wm_change_state_)
    {
        if (e.data.data32[0] == WM_STATE_ICONIC)
            iconify_window(e.window);
        return;
    }

    if (e.type == ewmh->_NET_WM_STATE)
    {
        handle_wm_state_change(e);
        return;
    }

    if (e.type == ewmh->_NET_CURRENT_DESKTOP)
    {
        uint32_t desktop = e.data.data32[0];
        LOG_DEBUG("_NET_CURRENT_DESKTOP request: desktop={}", desktop);
        switch_to_ewmh_desktop(desktop);
        return;
    }

    if (e.type == ewmh->_NET_ACTIVE_WINDOW)
    {
        handle_active_window_request(e);
        return;
    }

    if (e.type == ewmh->_NET_WM_DESKTOP)
    {
        handle_desktop_change(e);
        return;
    }

    if (e.type == ewmh->_NET_REQUEST_FRAME_EXTENTS)
    {
        ewmh_.set_frame_extents(e.window, 0, 0, 0, 0);
        conn_.flush();
        return;
    }

    if (e.type == ewmh->_NET_MOVERESIZE_WINDOW)
    {
        handle_moveresize_window(e);
        return;
    }

    if (e.type == ewmh->_NET_WM_MOVERESIZE)
    {
        handle_wm_moveresize(e);
        return;
    }

    if (e.type == ewmh->_NET_SHOWING_DESKTOP)
    {
        handle_showing_desktop(e);
        return;
    }

    if (e.type == ewmh->_NET_RESTACK_WINDOW)
    {
        xcb_window_t sibling = static_cast<xcb_window_t>(e.data.data32[1]);
        uint32_t detail = e.data.data32[2];

        uint32_t values[2];
        uint16_t mask = XCB_CONFIG_WINDOW_STACK_MODE;
        values[0] = detail;

        if (sibling != XCB_NONE)
        {
            mask |= XCB_CONFIG_WINDOW_SIBLING;
            values[0] = sibling;
            values[1] = detail;
        }

        xcb_configure_window(conn_.get(), e.window, mask, values);
        update_ewmh_client_list();
        conn_.flush();
    }
}

void WindowManager::handle_wm_state_change(xcb_client_message_event_t const& e)
{
    xcb_ewmh_connection_t* ewmh = ewmh_.get();
    uint32_t action = e.data.data32[0];
    xcb_atom_t first = static_cast<xcb_atom_t>(e.data.data32[1]);
    xcb_atom_t second = static_cast<xcb_atom_t>(e.data.data32[2]);

    auto handle_state = [&](xcb_atom_t state)
    {
        if (state == XCB_ATOM_NONE)
            return;

        auto compute_enable = [action](bool currently_set)
        {
            if (action == 0)
                return false;
            if (action == 1)
                return true;
            return !currently_set;
        };

        if (state == ewmh->_NET_WM_STATE_FULLSCREEN)
        {
            set_fullscreen(e.window, compute_enable(is_client_fullscreen(e.window)));
        }
        else if (state == ewmh->_NET_WM_STATE_ABOVE)
        {
            set_window_above(e.window, compute_enable(is_client_above(e.window)));
        }
        else if (state == ewmh->_NET_WM_STATE_BELOW)
        {
            set_window_below(e.window, compute_enable(is_client_below(e.window)));
        }
        else if (state == ewmh->_NET_WM_STATE_SKIP_TASKBAR)
        {
            set_client_skip_taskbar(e.window, compute_enable(is_client_skip_taskbar(e.window)));
        }
        else if (state == ewmh->_NET_WM_STATE_SKIP_PAGER)
        {
            set_client_skip_pager(e.window, compute_enable(is_client_skip_pager(e.window)));
        }
        else if (state == ewmh->_NET_WM_STATE_HIDDEN)
        {
            bool enable = compute_enable(is_client_iconic(e.window));
            if (enable)
                iconify_window(e.window);
            else
                deiconify_window(e.window, false);
        }
        else if (state == ewmh->_NET_WM_STATE_STICKY)
        {
            set_window_sticky(e.window, compute_enable(is_client_sticky(e.window)));
        }
        else if (state == ewmh->_NET_WM_STATE_MAXIMIZED_HORZ)
        {
            bool enable = compute_enable(is_client_maximized_horz(e.window));
            set_window_maximized(e.window, enable, is_client_maximized_vert(e.window));
        }
        else if (state == ewmh->_NET_WM_STATE_MAXIMIZED_VERT)
        {
            bool enable = compute_enable(is_client_maximized_vert(e.window));
            set_window_maximized(e.window, is_client_maximized_horz(e.window), enable);
        }
        else if (state == ewmh->_NET_WM_STATE_SHADED)
        {
            set_window_shaded(e.window, compute_enable(is_client_shaded(e.window)));
        }
        else if (state == ewmh->_NET_WM_STATE_MODAL)
        {
            set_window_modal(e.window, compute_enable(is_client_modal(e.window)));
        }
        else if (net_wm_state_focused_ != XCB_NONE && state == net_wm_state_focused_)
        {
            return;
        }
        else if (state == ewmh->_NET_WM_STATE_DEMANDS_ATTENTION)
        {
            set_client_demands_attention(e.window, compute_enable(is_client_demands_attention(e.window)));
        }
    };

    handle_state(first);
    handle_state(second);
}

void WindowManager::handle_active_window_request(xcb_client_message_event_t const& e)
{
    xcb_window_t window = e.window;
    uint32_t source = e.data.data32[0];
    uint32_t timestamp = e.data.data32[1];
    LOG_DEBUG("_NET_ACTIVE_WINDOW request: window={:#x} source={}", window, source);

    if (source == 1 && active_window_ != XCB_NONE && timestamp != 0)
    {
        auto* active_client = get_client(active_window_);
        if (active_client && active_client->user_time != 0)
        {
            if (timestamp < active_client->user_time)
            {
                LOG_DEBUG("Focus stealing prevented, setting demands attention");
                set_client_demands_attention(window, true);
                return;
            }
        }
    }

    if (is_client_iconic(window))
    {
        deiconify_window(window, false);
    }
    if (monitor_containing_window(window) || is_floating_window(window))
    {
        focus_any_window(window);
    }
}

void WindowManager::handle_desktop_change(xcb_client_message_event_t const& e)
{
    uint32_t desktop = e.data.data32[0];
    LOG_DEBUG("_NET_WM_DESKTOP request: window={:#x} desktop={}", e.window, desktop);

    if (desktop == 0xFFFFFFFF)
    {
        set_window_sticky(e.window, true);
        return;
    }
    if (is_client_sticky(e.window))
    {
        set_window_sticky(e.window, false);
    }

    size_t workspaces_per_monitor = config_.workspaces.count;
    size_t target_monitor = desktop / workspaces_per_monitor;
    size_t target_workspace = desktop % workspaces_per_monitor;

    if (target_monitor >= monitors_.size())
        return;

    bool was_active = (active_window_ == e.window);
    auto target_visible = [&](size_t monitor_idx, size_t workspace_idx)
    {
        if (showing_desktop_)
            return false;
        return monitor_idx < monitors_.size() && workspace_idx == monitors_[monitor_idx].current_workspace;
    };

    if (auto* source_monitor = monitor_containing_window(e.window))
    {
        auto source_idx = workspace_index_for_window(e.window);
        if (!source_idx)
            return;

        auto& source_ws = source_monitor->workspaces[*source_idx];
        auto it = source_ws.find_window(e.window);
        if (it == source_ws.windows.end())
            return;

        xcb_window_t win = *it;
        source_ws.windows.erase(it);
        if (source_ws.focused_window == e.window)
        {
            source_ws.focused_window = XCB_NONE;
            for (auto rit = source_ws.windows.rbegin(); rit != source_ws.windows.rend(); ++rit)
            {
                if (!is_client_iconic(*rit))
                {
                    source_ws.focused_window = *rit;
                    break;
                }
            }
        }

        auto& target_ws = monitors_[target_monitor].workspaces[target_workspace];
        target_ws.windows.push_back(win);
        if (!target_visible(target_monitor, target_workspace) || was_active)
        {
            target_ws.focused_window = e.window;
        }
        if (auto* client = get_client(e.window))
        {
            client->monitor = target_monitor;
            client->workspace = target_workspace;
        }
        ewmh_.set_window_desktop(e.window, desktop);

        rearrange_monitor(*source_monitor);
        if (source_monitor != &monitors_[target_monitor])
        {
            rearrange_monitor(monitors_[target_monitor]);
        }

        if (!target_visible(target_monitor, target_workspace))
        {
            hide_window(e.window);
        }

        update_ewmh_client_list();
        if (was_active && !target_visible(target_monitor, target_workspace))
        {
            focus_or_fallback(focused_monitor());
        }
        conn_.flush();
    }
    else if (is_floating_window(e.window))
    {
        auto* client = get_client(e.window);
        if (!client)
            return;

        size_t source_monitor = client->monitor;
        if (source_monitor != target_monitor)
        {
            client->floating_geometry = floating::place_floating(
                monitors_[target_monitor].working_area(),
                client->floating_geometry.width,
                client->floating_geometry.height,
                std::nullopt
            );
        }
        client->monitor = target_monitor;
        client->workspace = target_workspace;
        ewmh_.set_window_desktop(e.window, desktop);

        if (source_monitor != target_monitor)
        {
            update_floating_visibility(source_monitor);
        }
        update_floating_visibility(target_monitor);
        update_ewmh_client_list();
        if (was_active && !target_visible(target_monitor, target_workspace))
        {
            focus_or_fallback(focused_monitor());
        }
        conn_.flush();
    }
}

void WindowManager::handle_moveresize_window(xcb_client_message_event_t const& e)
{
    if (!is_floating_window(e.window))
        return;

    auto* client = get_client(e.window);
    if (!client)
        return;

    uint32_t flags = e.data.data32[0];
    bool has_x = flags & (1 << 8);
    bool has_y = flags & (1 << 9);
    bool has_width = flags & (1 << 10);
    bool has_height = flags & (1 << 11);

    if (has_x)
        client->floating_geometry.x = static_cast<int16_t>(e.data.data32[1]);
    if (has_y)
        client->floating_geometry.y = static_cast<int16_t>(e.data.data32[2]);
    if (has_width)
        client->floating_geometry.width = static_cast<uint16_t>(std::max<int32_t>(1, e.data.data32[3]));
    if (has_height)
        client->floating_geometry.height = static_cast<uint16_t>(std::max<int32_t>(1, e.data.data32[4]));

    update_floating_monitor_for_geometry(e.window);
    if (client->monitor < monitors_.size()
        && client->workspace == monitors_[client->monitor].current_workspace && !is_client_iconic(e.window)
        && !is_client_fullscreen(e.window))
    {
        apply_floating_geometry(e.window);
    }
    conn_.flush();
}

void WindowManager::handle_wm_moveresize(xcb_client_message_event_t const& e)
{
    if (!is_floating_window(e.window))
        return;

    int16_t x_root = static_cast<int16_t>(e.data.data32[0]);
    int16_t y_root = static_cast<int16_t>(e.data.data32[1]);
    uint32_t direction = e.data.data32[2];

    if (direction == 11)
    {
        end_drag();
    }
    else if (direction == 8)
    {
        focus_any_window(e.window);
        begin_drag(e.window, false, x_root, y_root);
    }
    else if (direction <= 7)
    {
        focus_any_window(e.window);
        begin_drag(e.window, true, x_root, y_root);
    }
}

void WindowManager::handle_showing_desktop(xcb_client_message_event_t const& e)
{
    bool show = e.data.data32[0] != 0;
    if (show == showing_desktop_)
        return;

    showing_desktop_ = show;
    ewmh_.set_showing_desktop(showing_desktop_);

    if (showing_desktop_)
    {
        for (auto const& monitor : monitors_)
        {
            for (xcb_window_t window : monitor.current().windows)
            {
                hide_window(window);
            }
        }
        for (xcb_window_t fw : floating_windows_)
        {
            auto const* c = get_client(fw);
            if (c && c->monitor < monitors_.size() && c->workspace == monitors_[c->monitor].current_workspace)
            {
                hide_window(fw);
            }
        }
        clear_focus();
    }
    else
    {
        rearrange_all_monitors();
        update_floating_visibility_all();
        if (!monitors_.empty())
        {
            focus_or_fallback(focused_monitor());
        }
    }
    conn_.flush();
}

void WindowManager::handle_configure_request(xcb_configure_request_event_t const& e)
{
    if (monitor_containing_window(e.window))
    {
        send_configure_notify(e.window);
        return;
    }

    if (is_client_fullscreen(e.window))
    {
        apply_fullscreen_if_needed(e.window, fullscreen_policy::ApplyContext::ConfigureTransition);
        send_configure_notify(e.window);
        return;
    }

    bool is_floating = is_floating_window(e.window);
    uint16_t mask = e.value_mask;
    if (is_floating)
    {
        mask &= ~XCB_CONFIG_WINDOW_BORDER_WIDTH;
    }

    if (mask == 0)
        return;

    uint32_t values[7];
    size_t index = 0;

    if (mask & XCB_CONFIG_WINDOW_X)
        values[index++] = static_cast<uint32_t>(e.x);
    if (mask & XCB_CONFIG_WINDOW_Y)
        values[index++] = static_cast<uint32_t>(e.y);
    if (mask & XCB_CONFIG_WINDOW_WIDTH)
        values[index++] = static_cast<uint32_t>(e.width);
    if (mask & XCB_CONFIG_WINDOW_HEIGHT)
        values[index++] = static_cast<uint32_t>(e.height);
    if (mask & XCB_CONFIG_WINDOW_BORDER_WIDTH)
        values[index++] = static_cast<uint32_t>(e.border_width);
    if (mask & XCB_CONFIG_WINDOW_SIBLING)
        values[index++] = static_cast<uint32_t>(e.sibling);
    if (mask & XCB_CONFIG_WINDOW_STACK_MODE)
        values[index++] = static_cast<uint32_t>(e.stack_mode);

    xcb_configure_window(conn_.get(), e.window, mask, values);

    if (is_floating)
    {
        auto* client = get_client(e.window);
        if (!client)
            return;

        if (mask & XCB_CONFIG_WINDOW_X)
            client->floating_geometry.x = e.x;
        if (mask & XCB_CONFIG_WINDOW_Y)
            client->floating_geometry.y = e.y;
        if (mask & XCB_CONFIG_WINDOW_WIDTH)
            client->floating_geometry.width = std::max<uint16_t>(1, e.width);
        if (mask & XCB_CONFIG_WINDOW_HEIGHT)
            client->floating_geometry.height = std::max<uint16_t>(1, e.height);

        uint16_t requested_width = client->floating_geometry.width;
        uint16_t requested_height = client->floating_geometry.height;
        uint32_t hinted_width = requested_width;
        uint32_t hinted_height = requested_height;
        layout_.apply_size_hints(e.window, hinted_width, hinted_height);
        client->floating_geometry.width = static_cast<uint16_t>(std::max<uint32_t>(1, hinted_width));
        client->floating_geometry.height = static_cast<uint16_t>(std::max<uint32_t>(1, hinted_height));

        if (requested_width != client->floating_geometry.width
            || requested_height != client->floating_geometry.height)
        {
            apply_floating_geometry(e.window);
        }

        update_floating_monitor_for_geometry(e.window);
        if (active_window_ == e.window)
        {
            focused_monitor_ = client->monitor;
            update_ewmh_current_desktop();
        }
    }

    conn_.flush();
}

void WindowManager::handle_property_notify(xcb_property_notify_event_t const& e)
{
    if (e.atom == ewmh_.get()->_NET_WM_NAME || e.atom == XCB_ATOM_WM_NAME)
    {
        update_window_title(e.window);
    }
    if (wm_normal_hints_ != XCB_NONE && e.atom == wm_normal_hints_)
    {
        if (is_floating_window(e.window))
        {
            auto* client = get_client(e.window);
            if (client)
            {
                uint32_t hinted_width = client->floating_geometry.width;
                uint32_t hinted_height = client->floating_geometry.height;
                layout_.apply_size_hints(e.window, hinted_width, hinted_height);
                client->floating_geometry.width = static_cast<uint16_t>(std::max<uint32_t>(1, hinted_width));
                client->floating_geometry.height = static_cast<uint16_t>(std::max<uint32_t>(1, hinted_height));
                if (client->monitor < monitors_.size()
                    && client->workspace == monitors_[client->monitor].current_workspace
                    && !is_client_iconic(e.window) && !is_client_fullscreen(e.window))
                {
                    apply_floating_geometry(e.window);
                }
            }
        }
    }
    else if (auto const* client = get_client(e.window))
    {
        if (client->kind == Client::Kind::Tiled && client->monitor < monitors_.size())
            rearrange_monitor(monitors_[client->monitor]);
    }
    xcb_window_t client_window = e.window;
    for (auto& [id, client] : clients_)
    {
        if (client.user_time_window == e.window)
        {
            client.user_time = get_user_time(id);
        }
    }
    if (auto* c = get_client(client_window))
    {
        c->user_time = get_user_time(client_window);
    }
    if (wm_hints_ != XCB_NONE && e.atom == wm_hints_)
    {
        // Only process for managed windows (tiled or floating)
        if (monitor_containing_window(e.window) || is_floating_window(e.window))
        {
            xcb_icccm_wm_hints_t hints;
            if (xcb_icccm_get_wm_hints_reply(
                    conn_.get(),
                    xcb_icccm_get_wm_hints(conn_.get(), e.window),
                    &hints,
                    nullptr
                ))
            {
                // XUrgencyHint is defined as (1L << 8) = 256
                constexpr uint32_t XUrgencyHint = 256;
                bool urgent = (hints.flags & XUrgencyHint) != 0;
                // Set DEMANDS_ATTENTION if urgent, unless this window is already focused
                if (urgent && e.window != active_window_)
                {
                    set_client_demands_attention(e.window, true);
                }
                else if (!urgent)
                {
                    set_client_demands_attention(e.window, false);
                }
            }
        }
    }
    if ((e.atom == ewmh_.get()->_NET_WM_STRUT || e.atom == ewmh_.get()->_NET_WM_STRUT_PARTIAL)
        && std::ranges::find(dock_windows_, e.window) != dock_windows_.end())
    {
        update_struts();
        rearrange_all_monitors();
        conn_.flush();
    }
}

void WindowManager::handle_expose(xcb_expose_event_t const& e)
{
    // No internal bar to redraw
    (void)e;
}

void WindowManager::handle_timeouts()
{
    auto now = std::chrono::steady_clock::now();

    for (auto it = pending_pings_.begin(); it != pending_pings_.end();)
    {
        if (it->second <= now)
        {
            it = pending_pings_.erase(it);
        }
        else
        {
            ++it;
        }
    }

    for (auto it = pending_kills_.begin(); it != pending_kills_.end();)
    {
        if (it->second <= now)
        {
            xcb_kill_client(conn_.get(), it->first);
            it = pending_kills_.erase(it);
        }
        else
        {
            ++it;
        }
    }
    conn_.flush();
}

void WindowManager::handle_randr_screen_change()
{
    // Exit fullscreen for all windows before reconfiguring monitors
    // This prevents stale geometry in Client::fullscreen_restore after monitor layout changes
    std::vector<xcb_window_t> fullscreen_copy;
    for (auto const& [id, client] : clients_)
    {
        if (client.fullscreen)
            fullscreen_copy.push_back(id);
    }
    for (auto window : fullscreen_copy)
    {
        set_fullscreen(window, false);
    }

    // Save window locations by monitor NAME (not index) so we can restore after detect_monitors()
    // This preserves window positions when monitors are turned off and back on
    struct WindowLocation
    {
        xcb_window_t id;
        std::string monitor_name;
        size_t workspace;
    };
    std::vector<WindowLocation> tiled_locations;
    std::vector<std::pair<xcb_window_t, WindowLocation>> floating_locations;

    // Clear fullscreen_monitors from all clients since monitor indices may have changed
    for (auto& [id, client] : clients_)
    {
        client.fullscreen_monitors.reset();
    }

    // Save tiled window locations
    for (size_t mi = 0; mi < monitors_.size(); ++mi)
    {
        auto const& monitor = monitors_[mi];
        for (size_t wi = 0; wi < monitor.workspaces.size(); ++wi)
        {
            for (xcb_window_t window : monitor.workspaces[wi].windows)
            {
                tiled_locations.push_back({ window, monitor.name, wi });
            }
        }
    }

    // Save floating window locations (geometry persists in Client)
    for (xcb_window_t fw : floating_windows_)
    {
        if (auto* client = get_client(fw))
        {
            std::string monitor_name = (client->monitor < monitors_.size()) ? monitors_[client->monitor].name : "";
            floating_locations.push_back({ fw, { fw, monitor_name, client->workspace } });
        }
    }

    // Save focused monitor name for restoration
    std::string focused_monitor_name = monitors_.empty() ? "" : monitors_[focused_monitor_].name;

    detect_monitors();
    update_struts();

    if (!monitors_.empty())
    {
        // Build map from monitor name to new index
        std::unordered_map<std::string, size_t> name_to_index;
        for (size_t i = 0; i < monitors_.size(); ++i)
        {
            name_to_index[monitors_[i].name] = i;
        }

        // Restore tiled windows to their original monitors (by name)
        for (auto const& loc : tiled_locations)
        {
            size_t target_monitor = 0;
            size_t target_workspace = loc.workspace;

            // Try to find the original monitor by name
            if (auto it = name_to_index.find(loc.monitor_name); it != name_to_index.end())
            {
                target_monitor = it->second;
            }
            // else: monitor no longer exists, fall back to monitor 0

            // Clamp workspace to valid range
            if (target_workspace >= monitors_[target_monitor].workspaces.size())
            {
                target_workspace = monitors_[target_monitor].current_workspace;
            }

            monitors_[target_monitor].workspaces[target_workspace].windows.push_back(loc.id);
            uint32_t desktop = get_ewmh_desktop_index(target_monitor, target_workspace);
            ewmh_.set_window_desktop(loc.id, desktop);

            if (auto* client = get_client(loc.id))
            {
                client->monitor = target_monitor;
                client->workspace = target_workspace;
            }
        }

        // Restore floating windows to their original monitors
        floating_windows_.clear();
        for (auto& [floating_window, loc] : floating_locations)
        {
            size_t target_monitor = 0;
            size_t target_workspace = loc.workspace;

            // Try to find the original monitor by name
            if (auto it = name_to_index.find(loc.monitor_name); it != name_to_index.end())
            {
                target_monitor = it->second;
            }

            // Clamp workspace to valid range
            if (target_workspace >= monitors_[target_monitor].workspaces.size())
            {
                target_workspace = monitors_[target_monitor].current_workspace;
            }

            uint32_t desktop = get_ewmh_desktop_index(target_monitor, target_workspace);

            if (auto* client = get_client(floating_window))
            {
                client->monitor = target_monitor;
                client->workspace = target_workspace;
                // Keep original geometry - don't reposition
            }
            floating_windows_.push_back(floating_window);
            ewmh_.set_window_desktop(floating_window, desktop);
        }

        // Restore focused monitor by name, or fall back to 0
        focused_monitor_ = 0;
        if (auto it = name_to_index.find(focused_monitor_name); it != name_to_index.end())
        {
            focused_monitor_ = it->second;
        }
    }

    // Update EWMH for new monitor configuration
    update_ewmh_desktops();
    update_ewmh_client_list();

    update_ewmh_current_desktop();
    rearrange_all_monitors();

    // Focus a window after reconfiguration (Bug fix: was leaving focus unset)
    if (!monitors_.empty())
    {
        focus_or_fallback(monitors_[focused_monitor_]);
    }

    conn_.flush();
}

} // namespace lwm
