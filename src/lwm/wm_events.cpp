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

#include "wm.hpp"
#include "lwm/core/floating.hpp"
#include "lwm/core/focus.hpp"
#include "lwm/core/log.hpp"
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

} // namespace

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
            // ICCCM requires distinguishing WM-initiated unmaps (hide for workspace switch)
            // from client-initiated unmaps (withdraw request). We track WM-initiated unmaps
            // and decrement the counter here. If counter reaches zero, the entry is removed.
            // We receive exactly ONE UnmapNotify per unmap via root's SubstructureNotifyMask
            // (we intentionally do NOT select STRUCTURE_NOTIFY on client windows).
            if (auto it = wm_unmapped_windows_.find(e.window); it != wm_unmapped_windows_.end())
            {
                if (it->second > 0)
                {
                    if (--it->second == 0)
                        wm_unmapped_windows_.erase(it);
                    break;
                }
            }
            // Client-initiated unmap - unmanage the window and set WM_STATE to Withdrawn
            handle_window_removal(e.window);
            break;
        }
        case XCB_DESTROY_NOTIFY:
        {
            auto const& e = reinterpret_cast<xcb_destroy_notify_event_t const&>(event);
            handle_window_removal(e.window);
            wm_unmapped_windows_.erase(e.window); // Clean up tracking
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
            handle_key_press(reinterpret_cast<xcb_key_press_event_t const&>(event));
            break;
        case XCB_KEY_RELEASE:
            handle_key_release(reinterpret_cast<xcb_key_release_event_t const&>(event));
            break;
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
    // Case 1: Already managed window requesting map (deiconify)
    if (auto const* client = get_client(e.window))
    {
        bool focus = client->monitor == focused_monitor_
            && client->workspace == monitors_[client->monitor].current_workspace;
        deiconify_window(e.window, focus);
        return;
    }

    // Case 2: Override redirect windows are not managed
    if (is_override_redirect_window(e.window))
    {
        return;
    }

    // Classify the window using centralized EWMH type classification
    bool has_transient = transient_for_window(e.window).has_value();
    auto classification = ewmh_.classify_window(e.window, has_transient);

    // Read WM_HINTS for initial_state and urgency (ICCCM)
    bool start_iconic = false;
    bool urgent = false;
    constexpr uint32_t XUrgencyHint = 256; // (1L << 8)
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
    // Also check _NET_WM_STATE_HIDDEN
    if (ewmh_.has_window_state(e.window, ewmh_.get()->_NET_WM_STATE_HIDDEN))
    {
        start_iconic = true;
    }

    // Handle based on classification
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
            // Popup windows (menus, tooltips, notifications) are just mapped, not managed
            xcb_map_window(conn_.get(), e.window);
            conn_.flush();
            return;
        }

        case WindowClassification::Kind::Floating:
        {
            manage_floating_window(e.window, start_iconic);

            // Apply classification flags
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
            return;
        }

        case WindowClassification::Kind::Tiled:
        {
            manage_window(e.window, start_iconic);

            if (urgent)
                set_client_demands_attention(e.window, true);
            if (is_sticky_desktop(e.window) && !is_client_sticky(e.window))
                set_window_sticky(e.window, true);

            if (!start_iconic)
            {
                if (auto const* client = get_client(e.window))
                {
                    if (client->monitor == focused_monitor_
                        && client->workspace == monitors_[client->monitor].current_workspace)
                    {
                        focus_window(e.window);
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
    if (drag_state_.active)
        return;

    // For non-root windows: only handle if mode=NORMAL and detail!=INFERIOR
    // detail=INFERIOR means pointer entered because a child window closed,
    // not because the mouse actually moved - ignore these spurious events.
    // For root window: always handle (following DWM behavior)
    if (e.event != conn_.screen()->root)
    {
        if (e.mode != XCB_NOTIFY_MODE_NORMAL || e.detail == XCB_NOTIFY_DETAIL_INFERIOR)
            return;
    }
    else
    {
        // Root window: only require normal mode
        if (e.mode != XCB_NOTIFY_MODE_NORMAL)
            return;
    }

    // Case 1: Entering a managed window - focus-follows-mouse
    if (e.event != conn_.screen()->root)
    {
        if (find_floating_window(e.event))
        {
            focus_floating_window(e.event);
            return;
        }
        if (monitor_containing_window(e.event))
        {
            focus_window(e.event);
            return;
        }
    }

    // Case 2: Entering root or unmanaged window area (gaps/empty space)
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
    xcb_window_t window_under_cursor = (e.event == conn_.screen()->root && e.child != XCB_NONE)
        ? e.child : e.event;

    // Focus-follows-mouse on motion: if motion occurs within a managed window
    // that is not currently focused, focus it. This handles the case where a
    // new window took focus (per EWMH compliance) but the cursor remained in
    // another window. Moving the mouse within that window re-establishes focus.
    if (window_under_cursor != conn_.screen()->root)
    {
        if (find_floating_window(window_under_cursor))
        {
            if (window_under_cursor != active_window_)
                focus_floating_window(window_under_cursor);
            return;
        }
        if (monitor_containing_window(window_under_cursor))
        {
            if (window_under_cursor != active_window_)
                focus_window(window_under_cursor);
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

    if (auto const* binding = resolve_mouse_binding(e.state, e.detail))
    {
        if (binding->action == "drag_window")
        {
            if (find_floating_window(target))
            {
                focus_floating_window(target);
                begin_drag(target, false, e.root_x, e.root_y);
                return;
            }
            if (monitor_containing_window(target))
            {
                focus_window(target);
                begin_tiled_drag(target, e.root_x, e.root_y);
                return;
            }
        }
        else if (binding->action == "resize_floating")
        {
            if (find_floating_window(target))
            {
                focus_floating_window(target);
                begin_drag(target, true, e.root_x, e.root_y);
                return;
            }
        }
    }

    // Click-to-focus: clicking on a managed window focuses it (even without modifiers).
    // This complements motion-based FFM for cases where the user clicks in a window
    // that lost focus to a newly created window.
    //
    // Note: For floating windows, e.event is the window itself (they select BUTTON_PRESS).
    // For tiled windows, e.event is root and target was set to e.child above.
    if (target != conn_.screen()->root)
    {
        if (find_floating_window(target))
        {
            if (target != active_window_)
                focus_floating_window(target);
            return;
        }
        if (monitor_containing_window(target))
        {
            if (target != active_window_)
                focus_window(target);
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

    LWM_DEBUG_KEY(e.state, keysym);

    auto action = keybinds_.resolve(e.state, keysym);
    if (!action)
    {
        LWM_DEBUG("No action for keysym");
        return;
    }

    LWM_DEBUG("Action: " << action->type);

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
        // Prevent key auto-repeat from triggering multiple toggles.
        // Only toggle on the first keypress; ignore until key is released.
        if (keysym == last_toggle_keysym_ && !toggle_key_released_)
            return;
        last_toggle_keysym_ = keysym;
        toggle_key_released_ = false;
        toggle_workspace();
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
}

void WindowManager::handle_key_release(xcb_key_release_event_t const& e)
{
    xcb_keysym_t keysym = xcb_key_press_lookup_keysym(conn_.keysyms(), const_cast<xcb_key_release_event_t*>(&e), 0);

    // Reset toggle key state when the key is released.
    // This allows the next press to trigger a toggle.
    if (keysym == last_toggle_keysym_)
    {
        toggle_key_released_ = true;
    }
}

void WindowManager::handle_client_message(xcb_client_message_event_t const& e)
{
    xcb_ewmh_connection_t* ewmh = ewmh_.get();

    if (e.type == wm_protocols_ && e.data.data32[0] == net_wm_ping_)
    {
        // Ping response received - window is responsive
        xcb_window_t window = static_cast<xcb_window_t>(e.data.data32[2]);
        if (window == XCB_NONE)
        {
            window = e.window;
        }
        pending_pings_.erase(window);
        // Cancel pending kill since window proved responsive.
        // The window will close itself in response to WM_DELETE_WINDOW.
        // Force kill only happens if ping times out (window is hung).
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
        {
            iconify_window(e.window);
        }
        return;
    }

    if (e.type == ewmh->_NET_WM_STATE)
    {
        uint32_t action = e.data.data32[0];
        xcb_atom_t first = static_cast<xcb_atom_t>(e.data.data32[1]);
        xcb_atom_t second = static_cast<xcb_atom_t>(e.data.data32[2]);

        auto handle_state = [&](xcb_atom_t state)
        {
            if (state == XCB_ATOM_NONE)
                return;

            // Helper to compute enable state based on action
            auto compute_enable = [action](bool currently_set)
            {
                if (action == 0)
                    return false; // Remove
                if (action == 1)
                    return true;       // Add
                return !currently_set; // Toggle
            };

            if (state == ewmh->_NET_WM_STATE_FULLSCREEN)
            {
                bool enable = compute_enable(is_client_fullscreen(e.window));
                set_fullscreen(e.window, enable);
            }
            else if (state == ewmh->_NET_WM_STATE_ABOVE)
            {
                bool enable = compute_enable(is_client_above(e.window));
                set_window_above(e.window, enable);
            }
            else if (state == ewmh->_NET_WM_STATE_BELOW)
            {
                bool enable = compute_enable(is_client_below(e.window));
                set_window_below(e.window, enable);
            }
            else if (state == ewmh->_NET_WM_STATE_SKIP_TASKBAR)
            {
                bool enable = compute_enable(is_client_skip_taskbar(e.window));
                set_client_skip_taskbar(e.window, enable);
            }
            else if (state == ewmh->_NET_WM_STATE_SKIP_PAGER)
            {
                bool enable = compute_enable(is_client_skip_pager(e.window));
                set_client_skip_pager(e.window, enable);
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
                bool enable = compute_enable(is_client_sticky(e.window));
                set_window_sticky(e.window, enable);
            }
            else if (state == ewmh->_NET_WM_STATE_MAXIMIZED_HORZ)
            {
                bool enable = compute_enable(is_client_maximized_horz(e.window));
                bool vert = is_client_maximized_vert(e.window);
                set_window_maximized(e.window, enable, vert);
            }
            else if (state == ewmh->_NET_WM_STATE_MAXIMIZED_VERT)
            {
                bool enable = compute_enable(is_client_maximized_vert(e.window));
                bool horiz = is_client_maximized_horz(e.window);
                set_window_maximized(e.window, horiz, enable);
            }
            else if (state == ewmh->_NET_WM_STATE_SHADED)
            {
                bool enable = compute_enable(is_client_shaded(e.window));
                set_window_shaded(e.window, enable);
            }
            else if (state == ewmh->_NET_WM_STATE_MODAL)
            {
                bool enable = compute_enable(is_client_modal(e.window));
                set_window_modal(e.window, enable);
            }
            else if (net_wm_state_focused_ != XCB_NONE && state == net_wm_state_focused_)
            {
                return;
            }
            else if (state == ewmh->_NET_WM_STATE_DEMANDS_ATTENTION)
            {
                bool enable = compute_enable(is_client_demands_attention(e.window));
                set_client_demands_attention(e.window, enable);
                update_all_bars();
            }
        };

        handle_state(first);
        handle_state(second);
        return;
    }

    // Handle _NET_CURRENT_DESKTOP requests (e.g., from Polybar clicks)
    if (e.type == ewmh->_NET_CURRENT_DESKTOP)
    {
        uint32_t desktop = e.data.data32[0];
        LWM_DEBUG("_NET_CURRENT_DESKTOP request: desktop=" << desktop);
        switch_to_ewmh_desktop(desktop);
    }
    // Handle _NET_ACTIVE_WINDOW requests
    else if (e.type == ewmh->_NET_ACTIVE_WINDOW)
    {
        xcb_window_t window = e.window;
        uint32_t source = e.data.data32[0]; // 1 = application, 2 = pager
        uint32_t timestamp = e.data.data32[1];
        LWM_DEBUG("_NET_ACTIVE_WINDOW request: window=0x" << std::hex << window << std::dec << " source=" << source);

        // Focus stealing prevention: for application-initiated requests (source=1),
        // check if the request timestamp is newer than the current active window's user time
        if (source == 1 && active_window_ != XCB_NONE && timestamp != 0)
        {
            auto* active_client = get_client(active_window_);
            if (active_client && active_client->user_time != 0)
            {
                // If the requesting window's timestamp is older than the active window's last
                // user interaction, set demands attention instead of stealing focus
                if (timestamp < active_client->user_time)
                {
                    LWM_DEBUG("Focus stealing prevented, setting demands attention");
                    set_client_demands_attention(window, true);
                    update_all_bars();
                    return;
                }
            }
        }

        if (is_client_iconic(window))
        {
            deiconify_window(window, false);
        }
        if (monitor_containing_window(window))
        {
            focus_window(window);
        }
        else if (find_floating_window(window))
        {
            focus_floating_window(window);
        }
    }
    // Handle _NET_WM_DESKTOP requests (move window to another desktop)
    else if (e.type == ewmh->_NET_WM_DESKTOP)
    {
        uint32_t desktop = e.data.data32[0];
        LWM_DEBUG("_NET_WM_DESKTOP request: window=0x" << std::hex << e.window << std::dec << " desktop=" << desktop);

        // 0xFFFFFFFF means sticky (visible on all desktops)
        if (desktop == 0xFFFFFFFF)
        {
            set_window_sticky(e.window, true);
            return;
        }
        if (is_client_sticky(e.window))
        {
            set_window_sticky(e.window, false);
        }

        // Calculate target monitor and workspace from desktop index
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

        // Handle tiled windows
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
            // Sync Client for O(1) lookup
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

            // Hide if moved to non-current workspace
            if (!target_visible(target_monitor, target_workspace))
            {
                wm_unmap_window(e.window);
            }

            update_ewmh_client_list();
            if (was_active && !target_visible(target_monitor, target_workspace))
            {
                focus_or_fallback(focused_monitor());
            }
            update_all_bars();
            conn_.flush();
        }
        // Handle floating windows
        else if (auto* fw = find_floating_window(e.window))
        {
            auto* client = get_client(e.window);
            if (!client)
                return;

            size_t source_monitor = client->monitor;
            if (source_monitor != target_monitor)
            {
                fw->geometry = floating::place_floating(
                    monitors_[target_monitor].working_area(),
                    fw->geometry.width,
                    fw->geometry.height,
                    std::nullopt
                );
            }
            // Update Client (authoritative for monitor, workspace, geometry)
            client->monitor = target_monitor;
            client->workspace = target_workspace;
            client->floating_geometry = fw->geometry;
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
            update_all_bars();
            conn_.flush();
        }
    }
    // Handle _NET_REQUEST_FRAME_EXTENTS (apps query this before mapping)
    else if (e.type == ewmh->_NET_REQUEST_FRAME_EXTENTS)
    {
        // LWM doesn't add frames, so extents are all zeros
        ewmh_.set_frame_extents(e.window, 0, 0, 0, 0);
        conn_.flush();
    }
    // Handle _NET_MOVERESIZE_WINDOW (programmatic move/resize for floating windows)
    else if (e.type == ewmh->_NET_MOVERESIZE_WINDOW)
    {
        auto* fw = find_floating_window(e.window);
        if (!fw)
            return; // Only floating windows support this

        uint32_t flags = e.data.data32[0];
        // flags: bits 8-11 = gravity, bit 12 = x, bit 13 = y, bit 14 = width, bit 15 = height
        bool has_x = flags & (1 << 8);
        bool has_y = flags & (1 << 9);
        bool has_width = flags & (1 << 10);
        bool has_height = flags & (1 << 11);

        if (has_x)
            fw->geometry.x = static_cast<int16_t>(e.data.data32[1]);
        if (has_y)
            fw->geometry.y = static_cast<int16_t>(e.data.data32[2]);
        if (has_width)
            fw->geometry.width = static_cast<uint16_t>(std::max<int32_t>(1, e.data.data32[3]));
        if (has_height)
            fw->geometry.height = static_cast<uint16_t>(std::max<int32_t>(1, e.data.data32[4]));

        // Sync Client.floating_geometry (authoritative)
        auto* client = get_client(e.window);
        if (client)
            client->floating_geometry = fw->geometry;

        update_floating_monitor_for_geometry(*fw);
        if (client && client->workspace == monitors_[client->monitor].current_workspace
            && !is_client_iconic(fw->id) && !is_client_fullscreen(fw->id))
        {
            apply_floating_geometry(*fw);
        }
        conn_.flush();
    }
    // Handle _NET_WM_MOVERESIZE (interactive move/resize initiated by application)
    else if (e.type == ewmh->_NET_WM_MOVERESIZE)
    {
        auto* fw = find_floating_window(e.window);
        if (!fw)
            return; // Only floating windows support this

        int16_t x_root = static_cast<int16_t>(e.data.data32[0]);
        int16_t y_root = static_cast<int16_t>(e.data.data32[1]);
        uint32_t direction = e.data.data32[2];

        // Direction: 8 = move, 11 = cancel, 0-7 = resize edges
        if (direction == 11)
        {
            // Cancel
            end_drag();
        }
        else if (direction == 8)
        {
            // Move
            focus_floating_window(e.window);
            begin_drag(e.window, false, x_root, y_root);
        }
        else if (direction <= 7)
        {
            // Resize (direction indicates edge/corner)
            focus_floating_window(e.window);
            begin_drag(e.window, true, x_root, y_root);
        }
    }
    // Handle _NET_SHOWING_DESKTOP (show desktop mode toggle)
    else if (e.type == ewmh->_NET_SHOWING_DESKTOP)
    {
        bool show = e.data.data32[0] != 0;
        if (show != showing_desktop_)
        {
            showing_desktop_ = show;
            ewmh_.set_showing_desktop(showing_desktop_);

            if (showing_desktop_)
            {
                // Hide all windows
                for (auto const& monitor : monitors_)
                {
                    for (xcb_window_t window : monitor.current().windows)
                    {
                        wm_unmap_window(window);
                    }
                }
                for (auto const& fw : floating_windows_)
                {
                    auto const* c = get_client(fw.id);
                    if (c && c->monitor < monitors_.size() && c->workspace == monitors_[c->monitor].current_workspace)
                    {
                        wm_unmap_window(fw.id);
                    }
                }
                clear_focus();
            }
            else
            {
                // Show windows again
                rearrange_all_monitors();
                update_floating_visibility_all();
                if (!monitors_.empty())
                {
                    focus_or_fallback(focused_monitor());
                }
            }
            conn_.flush();
        }
    }
    // Handle _NET_RESTACK_WINDOW (restack window relative to sibling)
    else if (e.type == ewmh->_NET_RESTACK_WINDOW)
    {
        xcb_window_t sibling = static_cast<xcb_window_t>(e.data.data32[1]);
        uint32_t detail = e.data.data32[2];

        uint32_t values[2];
        uint16_t mask = XCB_CONFIG_WINDOW_STACK_MODE;
        values[0] = detail; // Stack mode

        if (sibling != XCB_NONE)
        {
            mask |= XCB_CONFIG_WINDOW_SIBLING;
            values[0] = sibling;
            values[1] = detail;
        }

        xcb_configure_window(conn_.get(), e.window, mask, values);
        // Update _NET_CLIENT_LIST_STACKING to reflect the new stacking order
        update_ewmh_client_list();
        conn_.flush();
    }
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
        apply_fullscreen_if_needed(e.window);
        send_configure_notify(e.window);
        return;
    }

    auto* floating_window = find_floating_window(e.window);
    uint16_t mask = e.value_mask;
    if (floating_window)
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

    if (floating_window)
    {
        if (mask & XCB_CONFIG_WINDOW_X)
            floating_window->geometry.x = e.x;
        if (mask & XCB_CONFIG_WINDOW_Y)
            floating_window->geometry.y = e.y;
        if (mask & XCB_CONFIG_WINDOW_WIDTH)
            floating_window->geometry.width = std::max<uint16_t>(1, e.width);
        if (mask & XCB_CONFIG_WINDOW_HEIGHT)
            floating_window->geometry.height = std::max<uint16_t>(1, e.height);

        uint16_t requested_width = floating_window->geometry.width;
        uint16_t requested_height = floating_window->geometry.height;
        uint32_t hinted_width = requested_width;
        uint32_t hinted_height = requested_height;
        layout_.apply_size_hints(floating_window->id, hinted_width, hinted_height);
        floating_window->geometry.width = static_cast<uint16_t>(std::max<uint32_t>(1, hinted_width));
        floating_window->geometry.height = static_cast<uint16_t>(std::max<uint32_t>(1, hinted_height));

        // Sync Client.floating_geometry (authoritative)
        auto* client = get_client(e.window);
        if (client)
            client->floating_geometry = floating_window->geometry;

        if (requested_width != floating_window->geometry.width || requested_height != floating_window->geometry.height)
        {
            apply_floating_geometry(*floating_window);
        }

        update_floating_monitor_for_geometry(*floating_window);
        if (client && active_window_ == floating_window->id)
        {
            focused_monitor_ = client->monitor;
            update_ewmh_current_desktop();
            update_all_bars();
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
        if (auto* floating_window = find_floating_window(e.window))
        {
            uint32_t hinted_width = floating_window->geometry.width;
            uint32_t hinted_height = floating_window->geometry.height;
            layout_.apply_size_hints(floating_window->id, hinted_width, hinted_height);
            floating_window->geometry.width = static_cast<uint16_t>(std::max<uint32_t>(1, hinted_width));
            floating_window->geometry.height = static_cast<uint16_t>(std::max<uint32_t>(1, hinted_height));
            // Sync Client.floating_geometry (authoritative)
            auto* client = get_client(e.window);
            if (client)
                client->floating_geometry = floating_window->geometry;
            if (client && client->workspace == monitors_[client->monitor].current_workspace
                && !is_client_iconic(floating_window->id) && !is_client_fullscreen(floating_window->id))
            {
                apply_floating_geometry(*floating_window);
            }
        }
        else if (auto const* client = get_client(e.window))
        {
            if (client->kind == Client::Kind::Tiled && client->monitor < monitors_.size())
                rearrange_monitor(monitors_[client->monitor]);
        }
    }
    if ((wm_protocols_ != XCB_NONE && e.atom == wm_protocols_)
        || (net_wm_sync_request_counter_ != XCB_NONE && e.atom == net_wm_sync_request_counter_))
    {
        update_sync_state(e.window);
    }
    if (net_wm_fullscreen_monitors_ != XCB_NONE && e.atom == net_wm_fullscreen_monitors_)
    {
        update_fullscreen_monitor_state(e.window);
        if (is_client_fullscreen(e.window))
        {
            apply_fullscreen_if_needed(e.window);
        }
    }
    if (net_wm_user_time_ != XCB_NONE && e.atom == net_wm_user_time_)
    {
        // Check if this PropertyNotify is from a user time window
        // If so, find the client that owns it and update that client's user time
        xcb_window_t client_window = e.window;
        for (auto const& [id, client] : clients_)
        {
            if (client.user_time_window == e.window)
            {
                client_window = id;
                break;
            }
        }
        // User time is authoritative in Client
        if (auto* c = get_client(client_window))
            c->user_time = get_user_time(client_window);
    }
    // Handle WM_HINTS urgency flag changes (ICCCM → EWMH DEMANDS_ATTENTION)
    if (wm_hints_ != XCB_NONE && e.atom == wm_hints_)
    {
        // Only process for managed windows (tiled or floating)
        if (monitor_containing_window(e.window) || find_floating_window(e.window))
        {
            xcb_icccm_wm_hints_t hints;
            if (xcb_icccm_get_wm_hints_reply(conn_.get(), xcb_icccm_get_wm_hints(conn_.get(), e.window), &hints, nullptr))
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
        update_all_bars();
        conn_.flush();
    }
}

void WindowManager::handle_expose(xcb_expose_event_t const& e)
{
    if (!bar_ || e.count != 0)
        return;

    auto active_info = get_active_window_info();
    for (size_t i = 0; i < monitors_.size(); ++i)
    {
        auto const& monitor = monitors_[i];
        if (monitor.bar_window == e.window)
        {
            bar_->update(monitor, build_bar_state(i, active_info));
            break;
        }
    }
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

    std::vector<xcb_window_t> all_windows;
    std::vector<FloatingWindow> all_floating = floating_windows_;
    // Clear fullscreen_monitors from all clients since monitor indices may have changed
    for (auto& [id, client] : clients_)
    {
        client.fullscreen_monitors.reset();
    }
    for (auto& monitor : monitors_)
    {
        for (auto& workspace : monitor.workspaces)
        {
            for (xcb_window_t window : workspace.windows)
            {
                all_windows.push_back(window);
            }
        }
    }

    // Destroy old bar windows before detecting new monitors
    if (bar_)
    {
        for (auto const& monitor : monitors_)
        {
            if (monitor.bar_window != XCB_NONE)
            {
                bar_->destroy(monitor.bar_window);
            }
        }
    }

    detect_monitors();
    if (bar_)
    {
        setup_monitor_bars();
    }
    update_struts();

    if (!monitors_.empty())
    {
        // Move all windows to first monitor, current workspace
        uint32_t desktop = get_ewmh_desktop_index(0, monitors_[0].current_workspace);
        for (xcb_window_t window : all_windows)
        {
            monitors_[0].current().windows.push_back(window);
            ewmh_.set_window_desktop(window, desktop);
            // Sync Client for O(1) lookup
            if (auto* client = get_client(window))
            {
                client->monitor = 0;
                client->workspace = monitors_[0].current_workspace;
            }
        }

        floating_windows_.clear();
        for (auto& floating_window : all_floating)
        {
            // Update Client (authoritative) for monitor/workspace
            if (auto* client = get_client(floating_window.id))
            {
                client->monitor = 0;
                client->workspace = monitors_[0].current_workspace;
                client->floating_geometry = floating::place_floating(
                    monitors_[0].working_area(),
                    floating_window.geometry.width,
                    floating_window.geometry.height,
                    std::nullopt
                );
                floating_window.geometry = client->floating_geometry;
            }
            else
            {
                floating_window.geometry = floating::place_floating(
                    monitors_[0].working_area(),
                    floating_window.geometry.width,
                    floating_window.geometry.height,
                    std::nullopt
                );
            }
            floating_windows_.push_back(floating_window);
            ewmh_.set_window_desktop(floating_window.id, desktop);
        }
    }

    // Update EWMH for new monitor configuration
    update_ewmh_desktops();
    update_ewmh_client_list();

    focused_monitor_ = 0;
    update_ewmh_current_desktop();
    rearrange_all_monitors();

    // Focus a window after reconfiguration (Bug fix: was leaving focus unset)
    if (!monitors_.empty())
    {
        focus_or_fallback(monitors_[0]);
    }

    update_all_bars();
    conn_.flush();
}

} // namespace lwm
