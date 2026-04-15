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
#include "lwm/core/policy.hpp"
#include "lwm/core/window_rules.hpp"
#include "wm.hpp"
#include <xcb/xcb_icccm.h>

namespace lwm {

namespace {

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

char const* client_kind_str(Client::Kind kind)
{
    switch (kind)
    {
        case Client::Kind::Tiled: return "tiled";
        case Client::Kind::Floating: return "floating";
        case Client::Kind::Dock: return "dock";
        case Client::Kind::Desktop: return "desktop";
    }
    return "unknown";
}

std::string key_action_event_name(Action const& action)
{
    if (action.type == "focus_monitor")
    {
        if (action.direction < 0)
            return "focus_monitor_left";
        if (action.direction > 0)
            return "focus_monitor_right";
    }

    if (action.type == "move_to_monitor")
    {
        if (action.direction < 0)
            return "move_to_monitor_left";
        if (action.direction > 0)
            return "move_to_monitor_right";
    }

    return action.type;
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
            && visibility_policy::is_window_visible(
                showing_desktop_, false, client->sticky, client->monitor, client->workspace, monitors_
            );
        deiconify_window(e.window, focus);
        return;
    }

    if (is_override_redirect_window(e.window))
        return;

    auto [classification, rule_result] = classify_managed_window(e.window);

    bool start_iconic = false;
    bool urgent = false;

    xcb_icccm_wm_hints_t hints;
    if (xcb_icccm_get_wm_hints_reply(conn_.get(), xcb_icccm_get_wm_hints(conn_.get(), e.window), &hints, nullptr))
    {
        if ((hints.flags & XCB_ICCCM_WM_HINT_STATE) && hints.initial_state == XCB_ICCCM_WM_STATE_ICONIC)
            start_iconic = true;
        if (hints.flags & XUrgencyHint)
            urgent = true;
    }
    if (ewmh_.has_window_state(e.window, ewmh_.get()->_NET_WM_STATE_HIDDEN))
        start_iconic = true;

    // Detect scratchpad match BEFORE mapping so the window never enters the tiled layout.
    // If matched, force floating + iconic so it maps hidden, then finalize after.
    auto scratchpad_match = (classification.kind == WindowClassification::Kind::Tiled
        || classification.kind == WindowClassification::Kind::Floating)
        ? match_scratchpad_for_window(e.window, rule_result)
        : std::optional<std::string>{};

    if (scratchpad_match)
    {
        classification.kind = WindowClassification::Kind::Floating;
        start_iconic = true;
    }

    char const* kind_str = nullptr;
    switch (classification.kind)
    {
        case WindowClassification::Kind::Desktop:
            map_desktop_window(e.window);
            kind_str = "desktop";
            break;
        case WindowClassification::Kind::Dock:
            map_dock_window(e.window);
            kind_str = "dock";
            break;
        case WindowClassification::Kind::Popup:
            xcb_map_window(conn_.get(), e.window);
            conn_.flush();
            kind_str = "popup";
            break;
        case WindowClassification::Kind::Floating:
            map_floating_window(e.window, classification, rule_result, start_iconic, urgent);
            kind_str = "floating";
            break;
        case WindowClassification::Kind::Tiled:
            map_tiled_window(e.window, rule_result, start_iconic, urgent);
            kind_str = "tiled";
            break;
    }

    // Finalize scratchpad claim after the window is managed
    if (scratchpad_match)
    {
        auto* client_ptr = get_client(e.window);
        auto* state = find_named_scratchpad(*scratchpad_match);
        if (client_ptr && state && state->window == XCB_NONE)
        {
            client_ptr->scratchpad_name = scratchpad_match;
            finalize_scratchpad_claim(e.window, *state, *scratchpad_match);
        }
    }

    if (kind_str && !subscribers_.empty())
    {
        auto const* client = get_client(e.window);
        std::string json = "{\"event\":\"window_map\",\"window\":" + std::to_string(e.window)
            + ",\"class\":\"" + json_escape(client ? client->wm_class : std::string{})
            + "\",\"kind\":\"" + kind_str + "\"";
        if (client)
        {
            json += ",\"monitor\":" + std::to_string(client->monitor)
                + ",\"workspace\":" + std::to_string(client->workspace);
        }
        json += "}";
        emit_event(Event_WindowMap, json);
    }
}

void WindowManager::apply_initial_state_flags(xcb_window_t window, WindowRuleResult const& rule)
{
    if (rule.layer == WindowLayer::Overlay)
        set_window_layer(window, WindowLayer::Overlay);
    else if (rule.above.has_value() && *rule.above)
        set_window_above(window, true);
    if (rule.below.has_value() && *rule.below)
        set_window_below(window, true);
    if (rule.sticky.has_value() && *rule.sticky)
        set_window_sticky(window, true);
    if (rule.layer != WindowLayer::Overlay && rule.fullscreen.has_value() && *rule.fullscreen)
        set_fullscreen(window, true);
}

void WindowManager::map_desktop_window(xcb_window_t window)
{
    uint32_t values[] = { XCB_EVENT_MASK_PROPERTY_CHANGE };
    xcb_change_window_attributes(conn_.get(), window, XCB_CW_EVENT_MASK, values);
    xcb_map_window(conn_.get(), window);
    uint32_t stack_mode = XCB_STACK_MODE_BELOW;
    xcb_configure_window(conn_.get(), window, XCB_CONFIG_WINDOW_STACK_MODE, &stack_mode);
    if (!clients_.contains(window))
    {
        Client client;
        client.id = window;
        client.kind = Client::Kind::Desktop;
        client.skip_taskbar = true;
        client.skip_pager = true;
        client.order = next_client_order_++;
        clients_[window] = std::move(client);
    }
    update_ewmh_client_list();
    conn_.flush();
}

void WindowManager::map_dock_window(xcb_window_t window)
{
    uint32_t values[] = { XCB_EVENT_MASK_ENTER_WINDOW | XCB_EVENT_MASK_POINTER_MOTION
                          | XCB_EVENT_MASK_PROPERTY_CHANGE };
    xcb_change_window_attributes(conn_.get(), window, XCB_CW_EVENT_MASK, values);
    xcb_map_window(conn_.get(), window);
    if (!clients_.contains(window))
    {
        Client client;
        client.id = window;
        client.kind = Client::Kind::Dock;
        client.skip_taskbar = true;
        client.skip_pager = true;
        client.order = next_client_order_++;
        clients_[window] = std::move(client);
    }
    update_struts();
    rearrange_all_monitors();
    update_ewmh_client_list();
    conn_.flush();
}

void WindowManager::map_floating_window(
    xcb_window_t window,
    WindowClassification const& classification,
    WindowRuleResult const& rule_result,
    bool start_iconic,
    bool urgent)
{
    manage_floating_window(window, start_iconic);

    if (classification.skip_taskbar)
        set_client_skip_taskbar(window, true);
    if (classification.skip_pager)
        set_client_skip_pager(window, true);
    if (classification.above)
        set_window_above(window, true);
    if (urgent)
        set_client_demands_attention(window, true);
    if (is_sticky_desktop(window) && !is_client_sticky(window))
        set_window_sticky(window, true);

    if (!rule_result.matched)
        return;

    auto* client = get_client(window);

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
            ewmh_.set_window_desktop(window, desktop);

            client->floating_geometry = floating::place_floating(
                monitors_[target_mon].working_area(),
                client->floating_geometry.width,
                client->floating_geometry.height,
                std::nullopt
            );
        }
    }

    if (client && rule_result.geometry.has_value())
        client->floating_geometry = *rule_result.geometry;

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

    if (client)
        client->suppress_next_configure_request = rule_result.geometry.has_value() || rule_result.center;

    apply_initial_state_flags(window, rule_result);

    // Update visibility after placement changes
    client = get_client(window);
    if (client)
    {
        sync_visibility_for_monitor(client->monitor);
        if (!client->hidden && should_be_visible(window))
        {
            if (client->fullscreen)
                apply_fullscreen_if_needed(window);
            else if (client->maximized_horz || client->maximized_vert)
                apply_maximized_geometry(window);
            else
                apply_floating_geometry(window);
        }
        restack_monitor_layers(client->monitor);
    }

    flush_and_drain_crossing();
}

void WindowManager::map_tiled_window(
    xcb_window_t window,
    WindowRuleResult const& rule_result,
    bool start_iconic,
    bool urgent)
{
    manage_window(window, start_iconic);

    if (urgent)
        set_client_demands_attention(window, true);
    if (is_sticky_desktop(window) && !is_client_sticky(window))
        set_window_sticky(window, true);

    if (rule_result.matched)
    {
        auto* client = get_client(window);

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
                    remove_tiled_from_workspace(window, source_mon, source_ws);
                    add_tiled_to_workspace(window, target_mon, target_ws);

                    sync_visibility_for_monitor(source_mon);
                    rearrange_monitor(monitors_[source_mon]);
                    if (target_mon != source_mon)
                    {
                        sync_visibility_for_monitor(target_mon);
                        rearrange_monitor(monitors_[target_mon]);
                    }
                }
            }
        }

        apply_initial_state_flags(window, rule_result);
    }

    if (!start_iconic)
    {
        if (auto const* client = get_client(window))
        {
            if (client->monitor < monitors_.size() && client->monitor == focused_monitor_
                && should_be_visible(window))
            {
                focus_any_window(window);
            }
        }
    }

    flush_and_drain_crossing();
}

void WindowManager::handle_window_removal(xcb_window_t window)
{
    auto const* client = get_client(window);
    if (!client)
        return;

    // Capture info before unmanage destroys the client record
    std::string unmap_json;
    if (!subscribers_.empty())
    {
        unmap_json = "{\"event\":\"window_unmap\",\"window\":" + std::to_string(window)
            + ",\"kind\":\"" + client_kind_str(client->kind) + "\""
            + ",\"monitor\":" + std::to_string(client->monitor)
            + ",\"workspace\":" + std::to_string(client->workspace) + "}";
    }

    // Release scratchpad slot before unmanage destroys the client
    release_scratchpad_window(window);

    switch (client->kind)
    {
        case Client::Kind::Tiled:
            unmanage_window(window);
            break;
        case Client::Kind::Floating:
            unmanage_floating_window(window);
            break;
        case Client::Kind::Dock:
            unmanage_dock_window(window);
            break;
        case Client::Kind::Desktop:
            unmanage_desktop_window(window);
            break;
    }

    if (!unmap_json.empty())
        emit_event(Event_WindowUnmap, unmap_json);
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

    }

    if (e.event != conn_.screen()->root)
    {
        auto const* client = get_client(e.event);
        if (client && client->hidden)
        {
            LOG_TRACE("EnterNotify: ignored (window is hidden)");
            return;
        }

        if (client && (client->kind == Client::Kind::Floating || client->kind == Client::Kind::Tiled))
        {
            if (e.event != active_window_)
            {
                LOG_DEBUG("EnterNotify: focusing window {:#x}", e.event);
                focus_any_window(e.event);
            }
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
        auto const* client = get_client(window_under_cursor);
        if (client && client->hidden)
            return;

        if (client && (client->kind == Client::Kind::Floating || client->kind == Client::Kind::Tiled))
        {
            if (window_under_cursor != active_window_)
            {
                LOG_DEBUG("MotionNotify: focusing window {:#x} (was {:#x})", window_under_cursor, active_window_);
                focus_any_window(window_under_cursor);
            }
            return;
        }
    }

    // Cursor feedback: change root cursor to resize arrow when pointer is near a split border
    if (cursor_default_ != XCB_NONE)
    {
        xcb_cursor_t desired = cursor_default_;
        if (auto border_hit = try_hit_split_border(e.root_x, e.root_y))
            desired = (border_hit->hit.direction == SplitDirection::Horizontal) ? cursor_resize_h_ : cursor_resize_v_;
        set_root_cursor(desired);
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

    auto const* client = get_client(target);

    // Ignore button press on hidden (off-screen) windows
    if (client && client->hidden)
        return;

    bool is_floating = client && client->kind == Client::Kind::Floating;
    bool is_tiled = client && client->kind == Client::Kind::Tiled;

    if (auto const* binding = resolve_mouse_binding(e.state, e.detail))
    {
        if (binding->action == "drag_window")
        {
            if (is_floating)
            {
                focus_any_window(target);
                begin_drag(target, false, e.root_x, e.root_y);
                return;
            }
            if (is_tiled)
            {
                focus_any_window(target);
                begin_tiled_drag(target, e.root_x, e.root_y);
                return;
            }
        }
        else if (binding->action == "resize_floating")
        {
            if (is_floating)
            {
                focus_any_window(target);
                begin_drag(target, true, e.root_x, e.root_y);
                return;
            }
            // For tiled windows or root: try tiled split resize first
            if (auto border_hit = try_hit_split_border(e.root_x, e.root_y))
            {
                begin_tiled_resize(border_hit->hit, border_hit->monitor_idx, e.root_x, e.root_y);
                return;
            }
            // Fallback: convert tiled to floating and resize
            if (is_tiled)
            {
                if (client->fullscreen || client->iconic || client->layer == WindowLayer::Overlay || showing_desktop_)
                    return;

                size_t monitor_idx = client->monitor;
                if (monitor_idx >= monitors_.size())
                    return;

                convert_window_to_floating(target);
                auto* c = get_client(target);
                if (!c || c->kind != Client::Kind::Floating)
                    return;

                sync_visibility_for_monitor(monitor_idx);
                rearrange_monitor(monitors_[monitor_idx]);
                flush_and_drain_crossing();
                LWM_ASSERT_INVARIANTS(clients_, monitors_);

                focus_any_window(target);
                begin_drag(target, true, e.root_x, e.root_y);
                return;
            }
        }
        else if (binding->action == "toggle_float")
        {
            if (is_tiled || is_floating)
            {
                toggle_window_float(target);
                return;
            }
        }
    }

    if (target != conn_.screen()->root)
    {
        if (is_floating || is_tiled)
        {
            if (target != active_window_)
                focus_any_window(target);
            return;
        }
    }

    // Bare click on root: check if in a gap between tiled windows
    // Only handle unmodified or ctrl-only clicks to avoid stealing Mod+click bindings
    {
        uint16_t clean_mod = e.state & ~(XCB_MOD_MASK_LOCK | XCB_MOD_MASK_2 | XCB_MOD_MASK_CONTROL);
        if (target == conn_.screen()->root && e.child == XCB_NONE && e.detail == 1 && clean_mod == 0)
        {
            if (auto border_hit = try_hit_split_border(e.root_x, e.root_y))
            {
                bool ctrl_held = (e.state & XCB_MOD_MASK_CONTROL) != 0;

                bool is_double_click = !ctrl_held
                    && static_cast<int32_t>(e.time - last_gap_click_time_) > 0
                    && static_cast<int32_t>(e.time - last_gap_click_time_) < 400
                    && last_gap_click_address_ == border_hit->hit.address
                    && last_gap_click_monitor_ == border_hit->monitor_idx;

                last_gap_click_time_ = e.time;
                last_gap_click_address_ = border_hit->hit.address;
                last_gap_click_monitor_ = border_hit->monitor_idx;

                if (is_double_click || ctrl_held)
                {
                    reset_split_ratio(border_hit->hit.address, border_hit->monitor_idx);
                    last_gap_click_time_ = 0;
                    return;
                }

                begin_tiled_resize(border_hit->hit, border_hit->monitor_idx, e.root_x, e.root_y);
                return;
            }
        }
    }

    // Update focused monitor based on click position (for clicks on empty space)
    update_focused_monitor_at_point(e.root_x, e.root_y);
}

bool WindowManager::is_auto_repeat_toggle(xcb_keysym_t keysym, xcb_timestamp_t time)
{
    LOG_TRACE(
        "KeyPress: keysym={:#x} time={} last_keysym={:#x} last_release_time={}",
        keysym,
        time,
        last_toggle_keysym_,
        last_toggle_release_time_
    );
    bool same_key = (keysym == last_toggle_keysym_);
    bool same_time = (time == last_toggle_release_time_);
    LOG_TRACE("check: same_key={} same_time={} would_block={}", same_key, same_time, (same_key && same_time));

    if (same_key && same_time)
    {
        LOG_TRACE("BLOCKED (auto-repeat detected)");
        return true;
    }

    last_toggle_keysym_ = keysym;
    last_toggle_release_time_ = 0;
    return false;
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
        if (!is_auto_repeat_toggle(keysym, e.time))
            toggle_workspace();
    }
    else if (action->type == "move_to_workspace" && action->workspace >= 0)
    {
        move_window_to_workspace(action->workspace);
    }
    else if (action->type == "focus_monitor")
    {
        focus_monitor(action->direction);
    }
    else if (action->type == "move_to_monitor")
    {
        move_window_to_monitor(action->direction);
    }
    else if (action->type == "spawn")
    {
        if (action->command.has_value())
            launch_program(*action->command);
    }
    else if (action->type == "toggle_fullscreen")
    {
        if (active_window_ != XCB_NONE)
        {
            set_fullscreen(active_window_, !is_client_fullscreen(active_window_));
        }
    }
    else if (action->type == "toggle_float")
    {
        if (active_window_ != XCB_NONE)
            toggle_window_float(active_window_);
    }
    else if (action->type == "focus_next")
    {
        cycle_focus(true);
    }
    else if (action->type == "focus_prev")
    {
        cycle_focus(false);
    }
    else if (action->type == "reload_config")
    {
        auto result = reload_config();
        if (!result)
            LOG_WARN("Config reload failed: {}", result.error());
        emit_config_reload_result(result, "keybind");
    }
    else if (action->type == "restart")
    {
        LOG_INFO("Restart triggered by keybind");
        initiate_restart();
    }
    else if (action->type == "ratio_grow")
    {
        adjust_master_ratio(0.05);
        emit_event(Event_LayoutChange, "{\"event\":\"layout_change\",\"action\":\"ratio_grow\"}");
    }
    else if (action->type == "ratio_shrink")
    {
        adjust_master_ratio(-0.05);
        emit_event(Event_LayoutChange, "{\"event\":\"layout_change\",\"action\":\"ratio_shrink\"}");
    }
    else if (action->type == "toggle_scratchpad")
    {
        toggle_named_scratchpad(action->target);
    }
    else if (action->type == "scratchpad_stash")
    {
        if (active_window_ != XCB_NONE)
            stash_to_scratchpad(active_window_);
    }
    else if (action->type == "scratchpad_cycle")
    {
        cycle_scratchpad_pool();
    }
    else
    {
        return;
    }

    std::string event_action = key_action_event_name(*action);
    emit_event(Event_KeyAction,
        "{\"event\":\"key_action\",\"action\":\"" + json_escape(event_action) + "\"}");
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

    // WM_PROTOCOLS has a compound condition — handle before dispatch table
    if (e.type == wm_protocols_ && e.data.data32[0] == net_wm_ping_)
    {
        xcb_window_t window = static_cast<xcb_window_t>(e.data.data32[2]);
        if (window == XCB_NONE)
            window = e.window;
        pending_kills_.erase(window);
        return;
    }

    using Handler = void (WindowManager::*)(xcb_client_message_event_t const&);
    struct Entry
    {
        xcb_atom_t atom;
        Handler handler;
    };
    Entry const dispatch[] = {
        { net_close_window_, &WindowManager::handle_close_window_message },
        { net_wm_fullscreen_monitors_, &WindowManager::handle_fullscreen_monitors_message },
        { wm_change_state_, &WindowManager::handle_change_state_message },
        { ewmh->_NET_WM_STATE, &WindowManager::handle_wm_state_change },
        { ewmh->_NET_CURRENT_DESKTOP, &WindowManager::handle_current_desktop_message },
        { ewmh->_NET_ACTIVE_WINDOW, &WindowManager::handle_active_window_request },
        { ewmh->_NET_WM_DESKTOP, &WindowManager::handle_desktop_change },
        { ewmh->_NET_REQUEST_FRAME_EXTENTS, &WindowManager::handle_frame_extents_message },
        { ewmh->_NET_MOVERESIZE_WINDOW, &WindowManager::handle_moveresize_window },
        { ewmh->_NET_WM_MOVERESIZE, &WindowManager::handle_wm_moveresize },
        { ewmh->_NET_SHOWING_DESKTOP, &WindowManager::handle_showing_desktop },
        { ewmh->_NET_RESTACK_WINDOW, &WindowManager::handle_restack_message },
    };

    for (auto const& entry : dispatch)
    {
        if (e.type == entry.atom)
        {
            (this->*entry.handler)(e);
            return;
        }
    }
}

void WindowManager::handle_close_window_message(xcb_client_message_event_t const& e)
{
    kill_window(e.window);
}

void WindowManager::handle_fullscreen_monitors_message(xcb_client_message_event_t const& e)
{
    FullscreenMonitors monitors;
    monitors.top = e.data.data32[0];
    monitors.bottom = e.data.data32[1];
    monitors.left = e.data.data32[2];
    monitors.right = e.data.data32[3];
    set_fullscreen_monitors(e.window, monitors);
}

void WindowManager::handle_change_state_message(xcb_client_message_event_t const& e)
{
    if (e.data.data32[0] == WM_STATE_ICONIC)
        iconify_window(e.window);
}

void WindowManager::handle_current_desktop_message(xcb_client_message_event_t const& e)
{
    uint32_t desktop = e.data.data32[0];
    LOG_DEBUG("_NET_CURRENT_DESKTOP request: desktop={}", desktop);
    switch_to_ewmh_desktop(desktop);
}

void WindowManager::handle_frame_extents_message(xcb_client_message_event_t const& e)
{
    ewmh_.set_frame_extents(e.window, 0, 0, 0, 0);
    conn_.flush();
}

void WindowManager::handle_restack_message(xcb_client_message_event_t const& e)
{
    if (auto const* client = get_client(e.window);
        client && (client->kind == Client::Kind::Tiled || client->kind == Client::Kind::Floating))
    {
        if (client->monitor < monitors_.size())
            restack_monitor_layers(client->monitor);
        conn_.flush();
        return;
    }

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
    stacking_dirty_ = true;
    conn_.flush();
}

void WindowManager::handle_wm_state_change(xcb_client_message_event_t const& e)
{
    xcb_ewmh_connection_t* ewmh = ewmh_.get();
    uint32_t action = e.data.data32[0];
    xcb_atom_t first = static_cast<xcb_atom_t>(e.data.data32[1]);
    xcb_atom_t second = static_cast<xcb_atom_t>(e.data.data32[2]);

    auto compute_enable = [action](bool currently_set)
    {
        if (action == 0)
            return false;
        if (action == 1)
            return true;
        return !currently_set;
    };

    // Simple state entries: {atom, getter, setter}
    using Getter = bool (WindowManager::*)(xcb_window_t) const;
    using Setter = void (WindowManager::*)(xcb_window_t, bool);
    struct StateEntry
    {
        xcb_atom_t atom;
        Getter getter;
        Setter setter;
    };
    StateEntry const simple_states[] = {
        { ewmh->_NET_WM_STATE_FULLSCREEN, &WindowManager::is_client_fullscreen, &WindowManager::set_fullscreen },
        { ewmh->_NET_WM_STATE_ABOVE, &WindowManager::is_client_above, &WindowManager::set_window_above },
        { ewmh->_NET_WM_STATE_BELOW, &WindowManager::is_client_below, &WindowManager::set_window_below },
        { ewmh->_NET_WM_STATE_SKIP_TASKBAR, &WindowManager::is_client_skip_taskbar, &WindowManager::set_client_skip_taskbar },
        { ewmh->_NET_WM_STATE_SKIP_PAGER, &WindowManager::is_client_skip_pager, &WindowManager::set_client_skip_pager },
        { ewmh->_NET_WM_STATE_STICKY, &WindowManager::is_client_sticky, &WindowManager::set_window_sticky },

        { ewmh->_NET_WM_STATE_MODAL, &WindowManager::is_client_modal, &WindowManager::set_window_modal },
        { ewmh->_NET_WM_STATE_DEMANDS_ATTENTION, &WindowManager::is_client_demands_attention, &WindowManager::set_client_demands_attention },
    };

    auto handle_state = [&](xcb_atom_t state)
    {
        if (state == XCB_ATOM_NONE)
            return;

        for (auto const& entry : simple_states)
        {
            if (state == entry.atom)
            {
                (this->*entry.setter)(e.window, compute_enable((this->*entry.getter)(e.window)));
                return;
            }
        }

        // Special cases that don't fit the getter/setter pattern
        if (state == ewmh->_NET_WM_STATE_HIDDEN)
        {
            bool enable = compute_enable(is_client_iconic(e.window));
            if (enable)
                iconify_window(e.window);
            else
                deiconify_window(e.window, false);
        }
        else if (state == ewmh->_NET_WM_STATE_MAXIMIZED_HORZ)
        {
            set_window_maximized(e.window, compute_enable(is_client_maximized_horz(e.window)), is_client_maximized_vert(e.window));
        }
        else if (state == ewmh->_NET_WM_STATE_MAXIMIZED_VERT)
        {
            set_window_maximized(e.window, is_client_maximized_horz(e.window), compute_enable(is_client_maximized_vert(e.window)));
        }
        // _NET_WM_STATE_FOCUSED is read-only (WM-managed), ignore requests
    };

    handle_state(first);
    handle_state(second);

    // Keep app_* preferences in sync when the app explicitly toggles these atoms.
    if (auto* client = get_client(e.window))
    {
        auto sync_app_pref = [&](xcb_atom_t state)
        {
            if (state == ewmh->_NET_WM_STATE_SKIP_TASKBAR)
                client->app_skip_taskbar = client->skip_taskbar;
            else if (state == ewmh->_NET_WM_STATE_SKIP_PAGER)
                client->app_skip_pager = client->skip_pager;
            else if (state == ewmh->_NET_WM_STATE_ABOVE)
                client->app_above = client->above;
        };
        if (first != XCB_ATOM_NONE)
            sync_app_pref(first);
        if (second != XCB_ATOM_NONE)
            sync_app_pref(second);
    }
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

    if (is_suppressed_by_fullscreen(window))
    {
        if (source == 1)
            set_client_demands_attention(window, true);
        return;
    }

    if (is_client_iconic(window))
    {
        deiconify_window(window, false);
    }
    if (auto const* req_client = get_client(window);
        req_client && (req_client->kind == Client::Kind::Tiled || req_client->kind == Client::Kind::Floating))
    {
        focus_any_window(window);
    }
}

void WindowManager::finalize_after_desktop_move(
    xcb_window_t window, bool was_active, size_t target_monitor, size_t target_workspace)
{
    bool visible = target_monitor < monitors_.size() && !showing_desktop_
        && target_workspace == monitors_[target_monitor].current_workspace;

    if (was_active && target_monitor < monitors_.size() && is_suppressed_by_fullscreen(window))
        focus_or_fallback(monitors_[target_monitor], false);
    else if (was_active && !visible)
        focus_or_fallback(focused_monitor());

    flush_and_drain_crossing();
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
    if (workspaces_per_monitor == 0)
        return;
    size_t target_monitor = desktop / workspaces_per_monitor;
    size_t target_workspace = desktop % workspaces_per_monitor;

    if (target_monitor >= monitors_.size())
        return;

    bool was_active = (active_window_ == e.window);

    auto* source_client = get_client(e.window);
    if (source_client && source_client->kind == Client::Kind::Tiled)
    {
        size_t source_mon_idx = source_client->monitor;
        size_t source_ws_idx = source_client->workspace;

        if (source_mon_idx >= monitors_.size()
            || source_ws_idx >= monitors_[source_mon_idx].workspaces.size())
            return;

        auto& source_ws = monitors_[source_mon_idx].workspaces[source_ws_idx];
        if (source_ws.find_window(e.window) == source_ws.windows.end())
            return;

        remove_tiled_from_workspace(e.window, source_mon_idx, source_ws_idx);
        add_tiled_to_workspace(e.window, target_monitor, target_workspace);

        bool target_ws_visible = !showing_desktop_ && target_workspace == monitors_[target_monitor].current_workspace;
        if (!target_ws_visible || was_active)
        {
            workspace_policy::set_workspace_focus(monitors_[target_monitor].workspaces[target_workspace], e.window);
        }
        LWM_ASSERT_INVARIANTS(clients_, monitors_);

        update_fullscreen_owner_after_visibility_change(source_mon_idx);
        if (source_mon_idx != target_monitor)
            update_fullscreen_owner_after_visibility_change(target_monitor);

        sync_visibility_for_monitor(source_mon_idx);
        rearrange_monitor(monitors_[source_mon_idx]);
        if (source_mon_idx != target_monitor)
        {
            sync_visibility_for_monitor(target_monitor);
            rearrange_monitor(monitors_[target_monitor]);
        }

        finalize_after_desktop_move(e.window, was_active, target_monitor, target_workspace);
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
        assign_window_workspace(e.window, target_monitor, target_workspace);

        update_fullscreen_owner_after_visibility_change(source_monitor);
        if (source_monitor != target_monitor)
            update_fullscreen_owner_after_visibility_change(target_monitor);

        if (source_monitor != target_monitor)
            sync_visibility_for_monitor(source_monitor);
        sync_visibility_for_monitor(target_monitor);
        if (source_monitor != target_monitor)
            restack_monitor_layers(source_monitor);
        restack_monitor_layers(target_monitor);

        finalize_after_desktop_move(e.window, was_active, target_monitor, target_workspace);
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
    if (client->monitor < monitors_.size() && should_be_visible(e.window) && !client->hidden
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
        sync_visibility_all();
        clear_focus();
    }
    else
    {
        for (size_t i = 0; i < monitors_.size(); ++i)
            update_fullscreen_owner_after_visibility_change(i);
        rearrange_all_monitors();
        if (!monitors_.empty())
        {
            focus_or_fallback(focused_monitor());
        }
    }
    flush_and_drain_crossing();
}

void WindowManager::handle_configure_request(xcb_configure_request_event_t const& e)
{
    if (auto const* client = get_client(e.window); client && client->kind == Client::Kind::Tiled)
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

    if (is_client_overlay(e.window))
    {
        apply_floating_geometry(e.window);
        send_configure_notify(e.window);
        return;
    }

    bool is_floating = is_floating_window(e.window);
    uint16_t mask = e.value_mask;
    if (is_floating)
        mask &= ~(XCB_CONFIG_WINDOW_BORDER_WIDTH | XCB_CONFIG_WINDOW_SIBLING | XCB_CONFIG_WINDOW_STACK_MODE);

    if (mask == 0)
    {
        if (is_floating)
            send_configure_notify(e.window);
        return;
    }

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

        uint16_t geometry_mask = XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y | XCB_CONFIG_WINDOW_WIDTH
            | XCB_CONFIG_WINDOW_HEIGHT;
        if (client->suppress_next_configure_request && (mask & geometry_mask) != 0)
        {
            client->suppress_next_configure_request = false;
            apply_floating_geometry(e.window);
            send_configure_notify(e.window);
            conn_.flush();
            return;
        }

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

        if (requested_width != client->floating_geometry.width || requested_height != client->floating_geometry.height)
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
    else if (e.atom == ewmh_.get()->_NET_WM_WINDOW_TYPE || (wm_transient_for_ != XCB_NONE && e.atom == wm_transient_for_))
    {
        reevaluate_managed_window(e.window);
        conn_.flush();
    }
    else if (wm_normal_hints_ != XCB_NONE && e.atom == wm_normal_hints_)
    {
        if (is_floating_window(e.window))
        {
            auto* client = get_client(e.window);
            if (client)
            {
                if (client->layer != WindowLayer::Overlay)
                {
                    uint32_t hinted_width = client->floating_geometry.width;
                    uint32_t hinted_height = client->floating_geometry.height;
                    layout_.apply_size_hints(e.window, hinted_width, hinted_height);
                    client->floating_geometry.width = static_cast<uint16_t>(std::max<uint32_t>(1, hinted_width));
                    client->floating_geometry.height = static_cast<uint16_t>(std::max<uint32_t>(1, hinted_height));
                }
                if (client->monitor < monitors_.size() && should_be_visible(e.window) && !client->hidden
                    && !is_client_fullscreen(e.window))
                {
                    apply_floating_geometry(e.window);
                }
            }
        }
        else if (auto const* client = get_client(e.window))
        {
            if (client->kind == Client::Kind::Tiled && client->monitor < monitors_.size())
                rearrange_monitor(monitors_[client->monitor]);
        }
    }
    else if (wm_hints_ != XCB_NONE && e.atom == wm_hints_)
    {
        if (auto* client = get_client(e.window))
        {
            xcb_icccm_wm_hints_t hints;
            if (xcb_icccm_get_wm_hints_reply(
                    conn_.get(),
                    xcb_icccm_get_wm_hints(conn_.get(), e.window),
                    &hints,
                    nullptr
                ))
            {
                client->accepts_input = !(hints.flags & XCB_ICCCM_WM_HINT_INPUT) || hints.input;


                bool urgent = (hints.flags & XUrgencyHint) != 0;
                // Only act on non-active windows. The active window may have
                // WM-initiated urgency (via notify-attention IPC) that should
                // persist even if the app clears its own WM_HINTS flag — the
                // WM_HINTS urgency is re-synced on workspace switch.
                if (urgent && e.window != active_window_)
                {
                    set_client_demands_attention(e.window, true);
                }
                else if (!urgent && e.window != active_window_)
                {
                    set_client_demands_attention(e.window, false);
                }
            }
            else
            {
                client->accepts_input = true; // ICCCM default when WM_HINTS absent
            }

            if (active_window_ == e.window && !is_focus_eligible(e.window))
            {
                if (client->monitor < monitors_.size())
                    focus_or_fallback(monitors_[client->monitor], false);
                else
                    clear_focus();
            }
        }
    }
    else if (wm_protocols_ != XCB_NONE && e.atom == wm_protocols_)
    {
        if (auto* client = get_client(e.window))
            client->supports_take_focus = supports_protocol(e.window, wm_take_focus_);
    }
    else if (auto const* strut_client = get_client(e.window);
             strut_client && strut_client->kind == Client::Kind::Dock
             && (e.atom == ewmh_.get()->_NET_WM_STRUT || e.atom == ewmh_.get()->_NET_WM_STRUT_PARTIAL))
    {
        update_struts();
        rearrange_all_monitors();
        conn_.flush();
    }

    // User time tracking: only scan when relevant atoms change
    if (e.atom == net_wm_user_time_ || e.atom == net_wm_user_time_window_)
    {
        std::vector<xcb_window_t> time_update_ids;
        for (auto const& [id, client] : clients_)
        {
            if (client.user_time_window == e.window)
                time_update_ids.push_back(id);
        }
        for (xcb_window_t id : time_update_ids)
        {
            if (auto* client = get_client(id))
                client->user_time = get_user_time(id);
        }
        if (auto* c = get_client(e.window))
        {
            if (e.atom == net_wm_user_time_window_)
                refresh_user_time_tracking(e.window);
            else
                c->user_time = get_user_time(e.window);
        }
    }
}

void WindowManager::handle_timeouts()
{
    auto now = std::chrono::steady_clock::now();

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
    // Save window locations using policy types directly (by monitor NAME, not index)
    std::vector<hotplug_policy::SavedWindowLocation> tiled_locations;
    std::vector<hotplug_policy::SavedWindowLocation> floating_locations;

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
    for (auto const& [fw, client] : clients_)
    {
        if (client.kind != Client::Kind::Floating)
            continue;
        std::string monitor_name = (client.monitor < monitors_.size()) ? monitors_[client.monitor].name : "";
        floating_locations.push_back({ fw, monitor_name, client.workspace });
    }

    // Save workspace state per monitor name for restoration
    std::unordered_map<std::string, hotplug_policy::SavedWorkspaceState> saved_workspace_state;
    for (auto const& monitor : monitors_)
    {
        saved_workspace_state[monitor.name] = { monitor.current_workspace, monitor.previous_workspace };
    }

    // Save focused monitor name for restoration
    std::string focused_monitor_name = monitors_.empty() ? "" : monitors_[focused_monitor_].name;

    detect_monitors();
    update_struts();

    // Compute the relocation plan (pure function — no side effects)
    auto plan = hotplug_policy::plan_hotplug(
        monitors_, tiled_locations, floating_locations, saved_workspace_state, focused_monitor_name
    );

    // Apply workspace state
    for (auto const& [mi, ws] : plan.workspace_current)
    {
        if (mi < monitors_.size())
            monitors_[mi].current_workspace = ws;
    }
    for (auto const& [mi, ws] : plan.workspace_previous)
    {
        if (mi < monitors_.size())
            monitors_[mi].previous_workspace = ws;
    }

    if (!monitors_.empty())
    {
        // Apply tiled relocations
        for (auto const& rel : plan.tiled_relocations)
            add_tiled_to_workspace(rel.id, rel.target_monitor, rel.target_workspace);

        // Apply floating relocations
        for (auto const& rel : plan.floating_relocations)
        {
            uint32_t desktop = get_ewmh_desktop_index(rel.target_monitor, rel.target_workspace);

            if (auto* client = get_client(rel.id))
            {
                client->monitor = rel.target_monitor;
                client->workspace = rel.target_workspace;

                // Invalidate fullscreen_restore if window moved to a different monitor
                if (rel.original_monitor_name != monitors_[rel.target_monitor].name
                    && client->fullscreen_restore)
                {
                    auto area = monitors_[rel.target_monitor].working_area();
                    client->fullscreen_restore = floating::place_floating(
                        area,
                        client->fullscreen_restore->width,
                        client->fullscreen_restore->height,
                        std::nullopt
                    );
                }
            }
            ewmh_.set_window_desktop(rel.id, desktop);
        }

        focused_monitor_ = plan.focused_monitor;
    }

    // Update EWMH for new monitor configuration
    update_ewmh_desktops();
    update_ewmh_current_desktop();

    // Rebuild per-monitor fullscreen tracking lists from client state
    for (size_t i = 0; i < monitors_.size(); ++i)
    {
        monitors_[i].fullscreen_owner = XCB_NONE;
        monitors_[i].fullscreen_windows.clear();
    }
    for (auto const& [id, client] : clients_)
    {
        if (client.fullscreen && client.monitor < monitors_.size())
            monitors_[client.monitor].fullscreen_windows.push_back(id);
    }
    for (size_t i = 0; i < monitors_.size(); ++i)
        update_fullscreen_owner_after_visibility_change(i);

    rearrange_all_monitors();

    // Reapply fullscreen geometry for windows that stayed fullscreen
    for (size_t i = 0; i < monitors_.size(); ++i)
    {
        if (monitors_[i].fullscreen_owner != XCB_NONE)
            apply_fullscreen_if_needed(monitors_[i].fullscreen_owner);
    }

    // Focus a window after reconfiguration
    if (!monitors_.empty())
    {
        focus_or_fallback(monitors_[focused_monitor_]);
    }

    flush_and_drain_crossing();
}

} // namespace lwm
