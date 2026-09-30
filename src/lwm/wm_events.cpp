#include "lwm/core/floating.hpp"
#include "lwm/core/log.hpp"
#include "lwm/core/policy.hpp"
#include "lwm/core/window_rules.hpp"
#include "wm.hpp"
#include <unordered_set>
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

template <class... Ts> struct Overloaded : Ts...
{
    using Ts::operator()...;
};

template <class... Ts> Overloaded(Ts...) -> Overloaded<Ts...>;

std::string key_action_event_name(Action const& action)
{
    return std::visit(
        Overloaded{
            [](KillAction const&) { return std::string("kill"); },
            [](ReloadConfigAction const&) { return std::string("reload_config"); },
            [](RestartAction const&) { return std::string("restart"); },
            [](ToggleWorkspaceAction const&) { return std::string("toggle_workspace"); },
            [](ToggleFullscreenAction const&) { return std::string("toggle_fullscreen"); },
            [](ToggleFloatAction const&) { return std::string("toggle_float"); },
            [](FocusNextAction const&) { return std::string("focus_next"); },
            [](FocusPrevAction const&) { return std::string("focus_prev"); },
            [](RatioGrowAction const&) { return std::string("ratio_grow"); },
            [](RatioShrinkAction const&) { return std::string("ratio_shrink"); },
            [](SwapNextAction const&) { return std::string("swap_next"); },
            [](SwapPrevAction const&) { return std::string("swap_prev"); },
            [](ScratchpadStashAction const&) { return std::string("scratchpad_stash"); },
            [](ScratchpadCycleAction const&) { return std::string("scratchpad_cycle"); },
            [](SpawnAction const&) { return std::string("spawn"); },
            [](SwitchWorkspaceAction const&) { return std::string("switch_workspace"); },
            [](MoveToWorkspaceAction const&) { return std::string("move_to_workspace"); },
            [](FocusMonitorAction const& action)
            {
                if (action.direction < 0)
                    return std::string("focus_monitor_left");
                if (action.direction > 0)
                    return std::string("focus_monitor_right");
                return std::string("focus_monitor");
            },
            [](MoveToMonitorAction const& action)
            {
                if (action.direction < 0)
                    return std::string("move_to_monitor_left");
                if (action.direction > 0)
                    return std::string("move_to_monitor_right");
                return std::string("move_to_monitor");
            },
            [](ToggleScratchpadAction const&) { return std::string("toggle_scratchpad"); },
        },
        action
    );
}

}

void WindowManager::handle_event(xcb_generic_event_t const& event)
{
    uint8_t response_type = event.response_type & ~0x80;
    uint32_t event_time = extract_event_time(response_type, event);
    if (event_time != 0)
    {
        last_event_time_ = event_time;
        if (response_type != XCB_PROPERTY_NOTIFY)
            last_input_time_ = event_time;
    }

    if (conn_.has_randr() && response_type == conn_.randr_event_base() + XCB_RANDR_SCREEN_CHANGE_NOTIFY)
    {
        monitors_dirty_ = true;
        return;
    }

    if (conn_.has_randr() && response_type == conn_.randr_event_base() + XCB_RANDR_NOTIFY)
    {
        auto const& notify = reinterpret_cast<xcb_randr_notify_event_t const&>(event);
        if (notify.subCode == XCB_RANDR_NOTIFY_CRTC_CHANGE || notify.subCode == XCB_RANDR_NOTIFY_OUTPUT_CHANGE)
            monitors_dirty_ = true;
        return;
    }

    switch (response_type)
    {
        case XCB_CONFIGURE_NOTIFY:
        {
            auto const& e = reinterpret_cast<xcb_configure_notify_event_t const&>(event);
            if (auto* client = get_client(e.window); client && client->presentation.applied_geometry
                && (client->presentation.applied_geometry != Geometry{ e.x, e.y, e.width, e.height }
                    || client->presentation.applied_border != e.border_width))
                state_.presentation(client->id).applied_geometry.reset();
            break;
        }
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
            LWM_LOG_TRACE(
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
            LWM_LOG_TRACE(
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
        bool focus = client->monitor == focused_monitor_
            && visibility_policy::is_window_visible(
                         showing_desktop_,
                         false,
                         client->sticky,
                         client->monitor,
                         client->workspace,
                         monitors_
            );
        deiconify_window(e.window, focus);
        return;
    }

    if (is_override_redirect_window(e.window))
        return;

    auto initial = classify_managed_window(e.window);
    auto& classification = initial.classification;
    auto const& rule_result = initial.rule_result;

    bool start_iconic = false;
    // Detect scratchpad match BEFORE mapping so the window never enters the tiled layout.
    // If matched, force floating + iconic so it maps hidden, then finalize after.
    auto scratchpad_match = (classification.kind == WindowClassification::Kind::Tiled
                             || classification.kind == WindowClassification::Kind::Floating)
        ? match_scratchpad_for_window(initial.properties, rule_result)
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
            manage_client(e.window, initial, start_iconic);
            kind_str = "floating";
            break;
        case WindowClassification::Kind::Tiled:
            manage_client(e.window, initial, start_iconic);
            kind_str = "tiled";
            break;
    }

    // Finalize scratchpad claim after the window is managed
    if (scratchpad_match)
    {
        auto* client_ptr = get_client(e.window);
        auto* state = find_named_scratchpad(*scratchpad_match);
        if (client_ptr && state && state->window() == XCB_NONE)
        {
            finalize_scratchpad_claim(e.window, *state, *scratchpad_match);
        }
    }

    if (kind_str && ipc_.has_subscribers(Event_WindowMap))
    {
        auto const* client = get_client(e.window);
        if (client)
            kind_str = client_kind_str(client->kind());
        std::string json = "{\"event\":\"window_map\",\"window\":" + std::to_string(e.window) + ",\"class\":\""
            + json_escape(client ? client->wm_class : std::string{}) + "\",\"kind\":\"" + kind_str + "\"";
        if (client)
        {
            json += ",\"monitor\":" + std::to_string(client->monitor)
                + ",\"workspace\":" + std::to_string(client->workspace);
        }
        json += "}";
        queue_event(Event_WindowMap, std::move(json));
    }
}

void WindowManager::map_desktop_window(xcb_window_t window)
{
    uint32_t values[] = { XCB_EVENT_MASK_PROPERTY_CHANGE };
    xcb_change_window_attributes(conn_.get(), window, XCB_CW_EVENT_MASK, values);
    if (wm_state_ != XCB_NONE)
    {
        uint32_t data[] = { WM_STATE_NORMAL, 0 };
        xcb_change_property(conn_.get(), XCB_PROP_MODE_REPLACE, window, wm_state_, wm_state_, 32, 2, data);
    }
    xcb_map_window(conn_.get(), window);
    uint32_t stack_mode = XCB_STACK_MODE_BELOW;
    xcb_configure_window(conn_.get(), window, XCB_CONFIG_WINDOW_STACK_MODE, &stack_mode);
    if (!clients_.contains(window))
    {
        Client client;
        client.id = window;
        client.state = DesktopState{};
        client.skip_taskbar = true;
        client.skip_pager = true;
        state_.insert(std::move(client));
    }
    if (auto* c = get_client(window))
        publish_lwm_window_class(*c);
    request_client_list_update();
    conn_.flush();
}

void WindowManager::map_dock_window(xcb_window_t window)
{
    uint32_t values[] = { XCB_EVENT_MASK_ENTER_WINDOW | XCB_EVENT_MASK_POINTER_MOTION
                          | XCB_EVENT_MASK_PROPERTY_CHANGE };
    xcb_change_window_attributes(conn_.get(), window, XCB_CW_EVENT_MASK, values);
    if (wm_state_ != XCB_NONE)
    {
        uint32_t data[] = { WM_STATE_NORMAL, 0 };
        xcb_change_property(conn_.get(), XCB_PROP_MODE_REPLACE, window, wm_state_, wm_state_, 32, 2, data);
    }
    xcb_map_window(conn_.get(), window);
    if (!clients_.contains(window))
    {
        Client client;
        client.id = window;
        client.state = DockState{};
        client.skip_taskbar = true;
        client.skip_pager = true;
        state_.insert(std::move(client));
    }
    if (auto* c = get_client(window))
        publish_lwm_window_class(*c);
    request_workarea_update();
    request_client_list_update();
    conn_.flush();
}

void WindowManager::handle_window_removal(xcb_window_t window)
{
    auto const* client = get_client(window);
    if (!client)
        return;

    // Capture info before unmanage destroys the client record
    std::string unmap_json;
    if (ipc_.has_subscribers(Event_WindowUnmap))
    {
        unmap_json = "{\"event\":\"window_unmap\",\"window\":" + std::to_string(window) + ",\"kind\":\""
            + client_kind_str(client->kind()) + "\"" + ",\"monitor\":" + std::to_string(client->monitor)
            + ",\"workspace\":" + std::to_string(client->workspace) + "}";
    }

    // Release scratchpad slot before unmanage destroys the client

    unmanage_window(window);

    if (!unmap_json.empty())
        queue_event(Event_WindowUnmap, std::move(unmap_json));
}

void WindowManager::handle_enter_notify(xcb_enter_notify_event_t const& e)
{
    LWM_LOG_TRACE(
        "EVENT: EnterNotify event={:#x} root_x={} root_y={} mode={} detail={} time={}",
        e.event,
        e.root_x,
        e.root_y,
        static_cast<int>(e.mode),
        static_cast<int>(e.detail),
        e.time
    );

    if (drag_active())
    {
        LWM_LOG_TRACE("EnterNotify: ignored (drag active)");
        return;
    }

    if (e.event != conn_.screen()->root)
    {
        if (e.mode != XCB_NOTIFY_MODE_NORMAL || e.detail == XCB_NOTIFY_DETAIL_INFERIOR)
        {
            LWM_LOG_TRACE(
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
        if (client && !is_visible(*client))
        {
            LWM_LOG_TRACE("EnterNotify: ignored (window is hidden)");
            return;
        }

        if (client && (client->kind() == Client::Kind::Floating || client->kind() == Client::Kind::Tiled))
        {
            if (e.event != active_window_)
            {
                LWM_LOG_DEBUG("EnterNotify: focusing window {:#x}", e.event);
                focus_any_window(e.event);
            }
            return;
        }
    }

    // Case 2: Entering root or unmanaged window area (gaps/empty space)
    LWM_LOG_TRACE("EnterNotify: updating focused monitor at ({}, {})", e.root_x, e.root_y);
    update_focused_monitor_at_point(e.root_x, e.root_y);
}

void WindowManager::handle_motion_notify(xcb_motion_notify_event_t const& e)
{
    if (drag_active())
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
        if (client && !is_visible(*client))
            return;

        if (client && (client->kind() == Client::Kind::Floating || client->kind() == Client::Kind::Tiled))
        {
            if (window_under_cursor != active_window_)
            {
                LWM_LOG_DEBUG("MotionNotify: focusing window {:#x} (was {:#x})", window_under_cursor, active_window_);
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

MousebindConfig const* WindowManager::resolve_mouse_binding(uint16_t state, uint8_t button) const
{
    uint16_t clean_mod = state & ~(XCB_MOD_MASK_LOCK | XCB_MOD_MASK_2);
    for (auto const& binding : config_.mousebinds)
    {
        if (binding.button == button && binding.modifier == clean_mod)
            return &binding;
    }
    return nullptr;
}

void WindowManager::handle_button_press(xcb_button_press_event_t const& e)
{
    bool from_window_grab = e.event != conn_.screen()->root;
    xcb_window_t target = e.event;
    if (!from_window_grab && e.child != XCB_NONE)
        target = e.child;

    auto const* client = get_client(target);

    auto allow_window_grab = [&](uint8_t mode)
    {
        if (!from_window_grab)
            return;
        xcb_allow_events(conn_.get(), mode, e.time);
        conn_.flush();
    };

    // Ignore button press on hidden (off-screen) windows
    if (client && !is_visible(*client))
    {
        allow_window_grab(XCB_ALLOW_ASYNC_POINTER);
        return;
    }

    bool is_floating = client && client->kind() == Client::Kind::Floating;
    bool is_tiled = client && client->kind() == Client::Kind::Tiled;

    auto handle_binding = [&](MousebindConfig const& binding) -> bool
    {
        if (from_window_grab)
            allow_window_grab(XCB_ALLOW_ASYNC_POINTER);

        if (binding.action == MouseAction::DragWindow && (is_floating || is_tiled))
        {
            begin_window_drag(target, e.root_x, e.root_y, e.detail);
            return true;
        }
        if (binding.action == MouseAction::ResizeFloating)
        {
            // Tiled/root clicks prefer a split; otherwise resize a floating window
            // or convert a tile after successfully acquiring the pointer.
            if (!is_floating)
                if (auto hit = try_hit_split_border(e.root_x, e.root_y))
                {
                    begin_tiled_resize(hit->hit, hit->monitor_idx, e.root_x, e.root_y, e.detail);
                    return true;
                }
            if (is_floating || is_tiled)
            {
                begin_window_drag(
                    target,
                    e.root_x,
                    e.root_y,
                    e.detail,
                    floating::ResizeEdge::Right | floating::ResizeEdge::Bottom
                );
                return true;
            }
        }
        else if (binding.action == MouseAction::ToggleFloat)
        {
            if (is_tiled || is_floating)
            {
                toggle_window_float(target);
                return true;
            }
        }
        return false;
    };

    // Managed windows use a passive SYNC grab for click-to-focus. Regular clicks
    // should still replay to the client, but modifier bindings must be handled
    // here because ReplayPointer skips ancestor passive grabs such as the root
    // grabs installed by grab_buttons().
    if (auto const* binding = resolve_mouse_binding(e.state, e.detail); binding && handle_binding(*binding))
        return;

    if (from_window_grab)
    {
        if (is_floating || is_tiled)
        {
            if (target != active_window_)
                focus_any_window(target);
        }
        allow_window_grab(XCB_ALLOW_REPLAY_POINTER);
        return;
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

                bool is_double_click = !ctrl_held && static_cast<int32_t>(e.time - last_gap_click_time_) > 0
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

                begin_tiled_resize(border_hit->hit, border_hit->monitor_idx, e.root_x, e.root_y, e.detail);
                return;
            }
        }
    }

    // Update focused monitor based on click position (for clicks on empty space)
    update_focused_monitor_at_point(e.root_x, e.root_y);
}

bool WindowManager::is_auto_repeat_toggle(xcb_keysym_t keysym, xcb_timestamp_t time)
{
    LWM_LOG_TRACE(
        "KeyPress: keysym={:#x} time={} last_keysym={:#x} last_release_time={}",
        keysym,
        time,
        last_toggle_keysym_,
        last_toggle_release_time_
    );
    bool same_key = (keysym == last_toggle_keysym_);
    bool same_time = (time == last_toggle_release_time_);
    LWM_LOG_TRACE("check: same_key={} same_time={} would_block={}", same_key, same_time, (same_key && same_time));

    if (same_key && same_time)
    {
        LWM_LOG_TRACE("BLOCKED (auto-repeat detected)");
        return true;
    }

    last_toggle_keysym_ = keysym;
    last_toggle_release_time_ = 0;
    return false;
}

void WindowManager::handle_button_release(xcb_button_release_event_t const& e)
{
    if (!drag_active())
        return;

    if (drag_->button && drag_->button != e.detail)
        return;
    update_drag(e.root_x, e.root_y);
    end_drag();
}

void WindowManager::handle_key_press(xcb_key_press_event_t const& e)
{
    xcb_keysym_t keysym = xcb_key_press_lookup_keysym(conn_.keysyms(), const_cast<xcb_key_press_event_t*>(&e), 0);

    LWM_LOG_KEY(e.state, keysym);

    auto action = keybinds_.resolve(e.state, keysym);
    if (!action)
    {
        LWM_LOG_TRACE("No action for keysym");
        return;
    }

    LWM_LOG_DEBUG("Action: {}", key_action_event_name(*action));

    bool handled = std::visit(
        Overloaded{
            [&](KillAction const&)
            {
                if (active_window_ == XCB_NONE)
                    return false;
                kill_window(active_window_);
                return true;
            },
            [&](ReloadConfigAction const&)
            {
                auto result = reload_config();
                emit_config_reload_result(result, "keybind");
                return true;
            },
            [&](RestartAction const&)
            {
                LWM_LOG_INFO("Restart triggered by keybind");
                initiate_restart();
                return true;
            },
            [&](ToggleWorkspaceAction const&)
            {
                if (!is_auto_repeat_toggle(keysym, e.time))
                    toggle_workspace();
                return true;
            },
            [&](ToggleFullscreenAction const&)
            {
                if (auto* client = get_client(active_window_))
                    set_fullscreen(*client, !client->fullscreen);
                return true;
            },
            [&](ToggleFloatAction const&)
            {
                if (active_window_ != XCB_NONE)
                    toggle_window_float(active_window_);
                return true;
            },
            [&](FocusNextAction const&)
            {
                cycle_focus(true);
                return true;
            },
            [&](FocusPrevAction const&)
            {
                cycle_focus(false);
                return true;
            },
            [&](RatioGrowAction const&)
            {
                adjust_master_ratio(0.05);
                if (ipc_.has_subscribers(Event_LayoutChange))
                {
                    queue_event(Event_LayoutChange, "{\"event\":\"layout_change\",\"action\":\"ratio_grow\"}");
                }
                return true;
            },
            [&](RatioShrinkAction const&)
            {
                adjust_master_ratio(-0.05);
                if (ipc_.has_subscribers(Event_LayoutChange))
                {
                    queue_event(Event_LayoutChange, "{\"event\":\"layout_change\",\"action\":\"ratio_shrink\"}");
                }
                return true;
            },
            [&](SwapNextAction const&)
            {
                swap_focused_tiled(1);
                if (ipc_.has_subscribers(Event_LayoutChange))
                {
                    queue_event(Event_LayoutChange, "{\"event\":\"layout_change\",\"action\":\"swap_next\"}");
                }
                return true;
            },
            [&](SwapPrevAction const&)
            {
                swap_focused_tiled(-1);
                if (ipc_.has_subscribers(Event_LayoutChange))
                {
                    queue_event(Event_LayoutChange, "{\"event\":\"layout_change\",\"action\":\"swap_prev\"}");
                }
                return true;
            },
            [&](ScratchpadStashAction const&)
            {
                if (active_window_ != XCB_NONE)
                    stash_to_scratchpad(active_window_);
                return true;
            },
            [&](ScratchpadCycleAction const&)
            {
                cycle_scratchpad_pool();
                return true;
            },
            [&](SpawnAction const& spawn)
            {
                launch_program(spawn.command);
                return true;
            },
            [&](SwitchWorkspaceAction const& switch_workspace_action)
            {
                switch_workspace(switch_workspace_action.workspace);
                return true;
            },
            [&](MoveToWorkspaceAction const& move_action)
            {
                move_window_to_workspace(move_action.workspace);
                return true;
            },
            [&](FocusMonitorAction const& focus_action)
            {
                focus_monitor(focus_action.direction);
                return true;
            },
            [&](MoveToMonitorAction const& move_action)
            {
                move_window_to_monitor(move_action.direction);
                return true;
            },
            [&](ToggleScratchpadAction const& scratchpad_action)
            {
                toggle_named_scratchpad(scratchpad_action.name);
                return true;
            },
        },
        *action
    );

    if (!handled)
        return;
    effects_.state_changed |= ipc_.has_subscribers(Event_StateChange);

    if (ipc_.has_subscribers(Event_KeyAction))
    {
        std::string event_action = key_action_event_name(*action);
        queue_event(Event_KeyAction, "{\"event\":\"key_action\",\"action\":\"" + json_escape(event_action) + "\"}");
    }
}

void WindowManager::handle_key_release(xcb_key_release_event_t const& e)
{
    xcb_keysym_t keysym = xcb_key_press_lookup_keysym(conn_.keysyms(), const_cast<xcb_key_release_event_t*>(&e), 0);

    LWM_LOG_TRACE("KeyRelease: keysym={:#x} time={} last_toggle_keysym={:#x}", keysym, e.time, last_toggle_keysym_);

    // Record timestamp for auto-repeat detection.
    // X11 auto-repeat sends KeyRelease-KeyPress pairs with identical timestamps.
    if (keysym == last_toggle_keysym_)
    {
        LWM_LOG_TRACE("KeyRelease matches toggle key, recording time={}", e.time);
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
        {                net_close_window_,        &WindowManager::handle_close_window_message },
        {      net_wm_fullscreen_monitors_, &WindowManager::handle_fullscreen_monitors_message },
        {                 wm_change_state_,        &WindowManager::handle_change_state_message },
        {              ewmh->_NET_WM_STATE,             &WindowManager::handle_wm_state_change },
        {       ewmh->_NET_CURRENT_DESKTOP,     &WindowManager::handle_current_desktop_message },
        {         ewmh->_NET_ACTIVE_WINDOW,       &WindowManager::handle_active_window_request },
        {            ewmh->_NET_WM_DESKTOP,              &WindowManager::handle_desktop_change },
        { ewmh->_NET_REQUEST_FRAME_EXTENTS,       &WindowManager::handle_frame_extents_message },
        {     ewmh->_NET_MOVERESIZE_WINDOW,           &WindowManager::handle_moveresize_window },
        {         ewmh->_NET_WM_MOVERESIZE,               &WindowManager::handle_wm_moveresize },
        {       ewmh->_NET_SHOWING_DESKTOP,             &WindowManager::handle_showing_desktop },
        {        ewmh->_NET_RESTACK_WINDOW,             &WindowManager::handle_restack_message },
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

void WindowManager::handle_close_window_message(xcb_client_message_event_t const& e) { kill_window(e.window); }

void WindowManager::handle_fullscreen_monitors_message(xcb_client_message_event_t const& e)
{
    FullscreenMonitors monitors;
    monitors.top = e.data.data32[0];
    monitors.bottom = e.data.data32[1];
    monitors.left = e.data.data32[2];
    monitors.right = e.data.data32[3];
    if (auto* client = get_client(e.window))
        set_fullscreen_monitors(*client, monitors);
}

void WindowManager::handle_change_state_message(xcb_client_message_event_t const& e)
{
    if (e.data.data32[0] == WM_STATE_ICONIC)
        iconify_window(e.window);
}

void WindowManager::handle_current_desktop_message(xcb_client_message_event_t const& e)
{
    uint32_t desktop = e.data.data32[0];
    LWM_LOG_DEBUG("_NET_CURRENT_DESKTOP request: desktop={}", desktop);
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
        client && (client->kind() == Client::Kind::Tiled || client->kind() == Client::Kind::Floating))
    {
        effects_.stacking = true;
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
    // The raw restack of an unmanaged window perturbs X's stacking order
    // outside our funnel; schedule a recompute so apply_stacking re-asserts
    // managed ordering and refreshes _NET_CLIENT_LIST_STACKING.
    effects_.stacking = true;
    conn_.flush();
}

void WindowManager::handle_wm_state_change(xcb_client_message_event_t const& e)
{
    xcb_ewmh_connection_t* ewmh = ewmh_.get();
    uint32_t action = e.data.data32[0];
    if (action > 2)
        return;
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

    auto const* client = get_client(e.window);
    if (!client)
        return;
    auto requested = [&](xcb_atom_t atom) { return first == atom || second == atom; };
    auto value = [&](xcb_atom_t atom, bool current) { return requested(atom) ? compute_enable(current) : current; };
    bool fullscreen = value(ewmh->_NET_WM_STATE_FULLSCREEN, client->fullscreen);
    bool horizontal = value(ewmh->_NET_WM_STATE_MAXIMIZED_HORZ, client->maximized_horz);
    bool vertical = value(ewmh->_NET_WM_STATE_MAXIMIZED_VERT, client->maximized_vert);
    // Read the complete request before changing anything. Fullscreen dominates
    // maximize independently of atom order; repeated atoms are applied once.
    if (!fullscreen && client->fullscreen)
        set_fullscreen(*client, false);
    if (requested(ewmh->_NET_WM_STATE_ABOVE) || requested(ewmh->_NET_WM_STATE_BELOW))
    {
        auto layer = client->preferences.layer.value_or(client->layer_hint);
        bool above = value(ewmh->_NET_WM_STATE_ABOVE, layer == LayerHint::Above);
        bool below = value(ewmh->_NET_WM_STATE_BELOW, layer == LayerHint::Below);
        if (requested(ewmh->_NET_WM_STATE_ABOVE) && above)
            below = false;
        else if (requested(ewmh->_NET_WM_STATE_BELOW) && below)
            above = false;
        state_.layer(e.window, above ? LayerHint::Above : below ? LayerHint::Below : LayerHint::Normal);
    }
    if (requested(ewmh->_NET_WM_STATE_SKIP_TASKBAR))
        state_.skip_taskbar(e.window, compute_enable(client->skip_taskbar));
    if (requested(ewmh->_NET_WM_STATE_SKIP_PAGER))
        state_.skip_pager(e.window, compute_enable(client->skip_pager));
    if (requested(ewmh->_NET_WM_STATE_STICKY))
        state_.sticky(e.window, compute_enable(client->sticky));
    if (requested(ewmh->_NET_WM_STATE_MODAL))
        state_.modal(e.window, compute_enable(client->modal));
    if (requested(ewmh->_NET_WM_STATE_DEMANDS_ATTENTION))
        state_.urgency(e.window, UrgencySource::App, compute_enable(client->urgency.has(UrgencySource::App)));
    if (requested(ewmh->_NET_WM_STATE_HIDDEN))
    {
        if (compute_enable(client->iconic))
            iconify_window(e.window);
        else
            deiconify_window(e.window, false);
    }
    if (requested(ewmh->_NET_WM_STATE_MAXIMIZED_HORZ) || requested(ewmh->_NET_WM_STATE_MAXIMIZED_VERT))
        state_.maximize(e.window, horizontal, vertical);
    if (fullscreen && requested(ewmh->_NET_WM_STATE_FULLSCREEN))
        set_fullscreen(*client, true);
}

void WindowManager::handle_active_window_request(xcb_client_message_event_t const& e)
{
    xcb_window_t window = e.window;
    uint32_t source = e.data.data32[0];
    uint32_t timestamp = e.data.data32[1];
    LWM_LOG_DEBUG("_NET_ACTIVE_WINDOW request: window={:#x} source={}", window, source);

    auto* request_client = get_client(window);
    if (!request_client)
        return;

    auto deny_with_attention = [&]()
    {
        if (source == 1)
            state_.urgency(request_client->id, UrgencySource::WmInitiated, true);
    };

    if (source == 1 && active_window_ != XCB_NONE && active_window_ != window)
    {
        if (timestamp == 0)
        {
            LWM_LOG_DEBUG("Focus stealing prevented, timestamp missing");
            deny_with_attention();
            return;
        }

        auto* active_client = get_client(active_window_);
        if (active_client && active_client->user_time != 0
            && ewmh_policy::timestamp_is_before(timestamp, active_client->user_time))
        {
            LWM_LOG_DEBUG("Focus stealing prevented, setting demands attention");
            deny_with_attention();
            return;
        }
    }

    auto would_be_suppressed = [&]()
    {
        if (!request_client->iconic)
            return is_suppressed_by_fullscreen(*request_client);

        auto candidate = *request_client;
        candidate.iconic = false;
        return is_suppressed_by_fullscreen(candidate);
    };

    if (would_be_suppressed())
    {
        deny_with_attention();
        return;
    }

    if (request_client->iconic)
    {
        deiconify_window(window, false);
        request_client = get_client(window);
        if (!request_client)
            return;
    }
    if (request_client->kind() == Client::Kind::Tiled || request_client->kind() == Client::Kind::Floating)
    {
        focus_any_window(window, true, source == 1 ? timestamp : 0);
    }
}

void WindowManager::handle_desktop_change(xcb_client_message_event_t const& e)
{
    uint32_t desktop = e.data.data32[0];
    LWM_LOG_DEBUG("_NET_WM_DESKTOP request: window={:#x} desktop={}", e.window, desktop);

    auto* client = get_client(e.window);
    if (!client)
        return;

    if (desktop == 0xFFFFFFFF)
    {
        state_.sticky(client->id, true);
        return;
    }

    size_t workspaces_per_monitor = config_.workspaces.count;
    if (workspaces_per_monitor == 0)
        return;
    size_t target_monitor = desktop / workspaces_per_monitor;
    size_t target_workspace = desktop % workspaces_per_monitor;

    if (target_monitor >= monitors_.size() || target_workspace >= monitors_[target_monitor].workspaces.size())
        return;

    if (client->sticky)
    {
        state_.sticky(client->id, false);
        client = get_client(e.window);
        if (!client)
            return;
    }

    bool was_active = (active_window_ == e.window);

    if (!state_.relocate(client->id, target_monitor, target_workspace, RelocationGeometry::CenterOnMonitorChange))
        return;
    bool target_ws_visible = !showing_desktop_ && target_workspace == monitors_[target_monitor].current_workspace;
    if (client->kind() == Client::Kind::Tiled && (!target_ws_visible || was_active))
        state_.remember_focus(target_monitor, target_workspace, e.window);

    state_.pin_desktop(e.window, true);
    if (was_active)
    {
        // Keep focus on the source monitor when the destination workspace is
        // not currently visible — repair_focus_after_visibility_change would
        // otherwise reassign focused_monitor_ to target_monitor and jump the
        // user's focus to a screen they are not interacting with.
        bool target_visible = target_monitor < monitors_.size() && !showing_desktop_
            && target_workspace == monitors_[target_monitor].current_workspace;
        if (target_visible)
            repair_focus_after_visibility_change(target_monitor, false);
        else if (focused_monitor_ < monitors_.size())
            focus_or_fallback(monitors_[focused_monitor_]);
        else
            clear_focus();
    }

    effects_.drain_crossing = true;
}

void WindowManager::handle_moveresize_window(xcb_client_message_event_t const& e)
{
    auto* client = get_client(e.window);
    if (!client || client->kind() != Client::Kind::Floating)
        return;

    uint32_t flags = e.data.data32[0];
    bool has_x = flags & (1 << 8);
    bool has_y = flags & (1 << 9);
    bool has_width = flags & (1 << 10);
    bool has_height = flags & (1 << 11);

    auto geom = floating_geometry(*client);
    if (has_x)
        geom.x = geometry_coordinate(static_cast<int32_t>(e.data.data32[1]));
    if (has_y)
        geom.y = geometry_coordinate(static_cast<int32_t>(e.data.data32[2]));
    if (has_width)
        geom.width = geometry_extent(e.data.data32[3]);
    if (has_height)
        geom.height = geometry_extent(e.data.data32[4]);

    state_.geometry(client->id, geom);
    update_floating_monitor_for_geometry(*client);
    bool visible = is_visible(*client);
    if (visible && active_window_ == e.window)
    {
        state_.focus_monitor(client->monitor);
        request_current_desktop_update();
    }
    if (visible && !client->fullscreen)
    {
        request_geometry(*client);
    }
    conn_.flush();
}

void WindowManager::handle_wm_moveresize(xcb_client_message_event_t const& e)
{
    uint32_t direction = e.data.data32[2];
    if (direction == 11)
    {
        if (drag_)
            if (auto const* move = std::get_if<WindowDrag>(&drag_->operation); move && move->window == e.window)
                end_drag(false);
        return;
    }
    auto const* client = get_client(e.window);
    if (!client || client->kind() != Client::Kind::Floating || direction > 8 || e.data.data32[3] > 255)
        return;
    using Edge = floating::ResizeEdge;
    static constexpr Edge edges[] = { Edge::Top | Edge::Left,
                                      Edge::Top,
                                      Edge::Top | Edge::Right,
                                      Edge::Right,
                                      Edge::Bottom | Edge::Right,
                                      Edge::Bottom,
                                      Edge::Bottom | Edge::Left,
                                      Edge::Left,
                                      Edge::None };
    begin_window_drag(
        e.window,
        geometry_coordinate(static_cast<int32_t>(e.data.data32[0])),
        geometry_coordinate(static_cast<int32_t>(e.data.data32[1])),
        static_cast<uint8_t>(e.data.data32[3]),
        edges[direction]
    );
}

void WindowManager::handle_showing_desktop(xcb_client_message_event_t const& e)
{
    bool show = e.data.data32[0] != 0;
    if (show == showing_desktop_)
        return;

    state_.showing_desktop(show);
    ewmh_.set_showing_desktop(showing_desktop_);

    if (showing_desktop_)
    {
        invalidate_all_monitors();
        clear_focus();
    }
    else
    {
        invalidate_all_monitors();
        if (!monitors_.empty())
        {
            focus_or_fallback(focused_monitor());
        }
    }
    effects_.drain_crossing = true;
}

void WindowManager::handle_configure_request(xcb_configure_request_event_t const& e)
{
    auto* client = get_client(e.window);
    if (client && (client->kind() == Client::Kind::Tiled || client->kind() == Client::Kind::Floating))
        request_configure_notify(*client);
    if (client && client->kind() == Client::Kind::Tiled)
        return;

    if (client && client->fullscreen)
    {
        request_geometry(*client);
        return;
    }

    bool is_floating = client && client->kind() == Client::Kind::Floating;
    uint16_t mask = e.value_mask;
    if (is_floating)
        mask &= ~(XCB_CONFIG_WINDOW_BORDER_WIDTH | XCB_CONFIG_WINDOW_SIBLING | XCB_CONFIG_WINDOW_STACK_MODE);

    if (mask == 0)
        return;

    if (is_floating)
    {
        uint16_t geometry_mask =
            XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y | XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT;
        if (client->suppress_next_configure_request && (mask & geometry_mask) != 0)
        {
            state_.configure_suppression(e.window, false);
            if (is_visible(*client))
                request_geometry(*client);
            return;
        }

        auto geom = floating_geometry(*client);
        if (mask & XCB_CONFIG_WINDOW_X)
            geom.x = e.x;
        if (mask & XCB_CONFIG_WINDOW_Y)
            geom.y = e.y;
        if (mask & XCB_CONFIG_WINDOW_WIDTH)
            geom.width = std::max<uint16_t>(1, e.width);
        if (mask & XCB_CONFIG_WINDOW_HEIGHT)
            geom.height = std::max<uint16_t>(1, e.height);

        geom.width = std::max<uint16_t>(1, geom.width);
        geom.height = std::max<uint16_t>(1, geom.height);

        state_.geometry(client->id, geom);
        update_floating_monitor_for_geometry(*client);
        bool visible = is_visible(*client);
        if (visible && active_window_ == e.window)
        {
            state_.focus_monitor(client->monitor);
            request_current_desktop_update();
        }

        if (visible)
            request_geometry(*client);
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

    conn_.flush();
}

void WindowManager::handle_property_notify(xcb_property_notify_event_t const& e)
{
    if (e.atom == ewmh_.get()->_NET_WM_NAME || e.atom == XCB_ATOM_WM_NAME)
    {
        update_window_title(e.window);
    }
    else if (e.atom == XCB_ATOM_WM_CLASS)
    {
        if (auto* client = get_client(e.window))
        {
            auto [instance, name] = get_wm_class(e.window);
            if (client->wm_class_name != instance || client->wm_class != name)
            {
                auto previous =
                    match_window_rules(config_.rules, window_match_info(*client), monitors_, config_.workspaces.names);
                state_.window_class(e.window, std::move(instance), std::move(name));
                effects_.state_changed |= ipc_.has_subscribers(Event_StateChange);
                reevaluate_metadata(e.window, previous);
            }
        }
    }
    else if (e.atom == ewmh_.get()->_NET_WM_WINDOW_TYPE
             || (wm_transient_for_ != XCB_NONE && e.atom == wm_transient_for_))
    {
        if (auto const* client = get_client(e.window))
        {
            auto previous =
                match_window_rules(config_.rules, window_match_info(*client), monitors_, config_.workspaces.names);
            auto parent = client->transient_for;
            if (e.atom == ewmh_.get()->_NET_WM_WINDOW_TYPE)
                state_.window_type(e.window, ewmh_.get_window_type_enum(e.window));
            else
                state_.transient(e.window, transient_for_window(e.window).value_or(XCB_NONE));
            relocate_to_transient_parent(e.window, parent);
            reevaluate_metadata(e.window, previous);
        }
    }
    else if (wm_normal_hints_ != XCB_NONE && e.atom == wm_normal_hints_)
    {
        if (auto* client = get_client(e.window); client && client->kind() == Client::Kind::Floating)
        {
            auto geom = floating_geometry(*client);
            xcb_size_hints_t hints;
            if (xcb_icccm_get_wm_normal_hints_reply(
                    conn_.get(),
                    xcb_icccm_get_wm_normal_hints(conn_.get(), e.window),
                    &hints,
                    nullptr
                ))
            {
                if (hints.flags & (XCB_ICCCM_SIZE_HINT_US_SIZE | XCB_ICCCM_SIZE_HINT_P_SIZE))
                {
                    uint32_t hinted_width = hints.width > 0 ? static_cast<uint32_t>(hints.width) : geom.width;
                    uint32_t hinted_height = hints.height > 0 ? static_cast<uint32_t>(hints.height) : geom.height;

                    geom.width = geometry_extent(hinted_width);
                    geom.height = geometry_extent(hinted_height);
                }
                bool transient_anchored = client->transient_for != XCB_NONE;
                bool has_position_hint = (hints.flags & XCB_ICCCM_SIZE_HINT_US_POSITION)
                    || ((hints.flags & XCB_ICCCM_SIZE_HINT_P_POSITION) && !transient_anchored);
                if (has_position_hint)
                {
                    int16_t hinted_x = geometry_coordinate(hints.x);
                    int16_t hinted_y = geometry_coordinate(hints.y);
                    bool desktop_pinned = !transient_anchored && client->desktop_pinned;
                    auto target = floating::resolve_position_hint(
                        monitors_,
                        client->monitor,
                        transient_anchored || desktop_pinned,
                        Geometry{ hinted_x, hinted_y, geom.width, geom.height }
                    );

                    if (target.accepted)
                    {
                        geom.x = hinted_x;
                        geom.y = hinted_y;
                    }
                    else
                    {
                        std::optional<Geometry> parent_geometry;
                        if (transient_anchored)
                            parent_geometry = current_window_geometry(client->transient_for);

                        geom = floating::place_floating(
                            monitors_[target.monitor].working_area(),
                            geom.width,
                            geom.height,
                            parent_geometry
                        );
                    }
                }
            }
            state_.geometry(client->id, geom);
            update_floating_monitor_for_geometry(*client, geom);
            bool visible = is_visible(*client);
            if (visible && active_window_ == e.window)
            {
                state_.focus_monitor(client->monitor);
                request_current_desktop_update();
            }
            if (visible)
                request_geometry(*client);
            conn_.flush();
        }
        else if (auto const* client = get_client(e.window))
        {
            if (client->kind() == Client::Kind::Tiled)
                invalidate_monitor(client->monitor);
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
                state_.focus_hints(
                    e.window,
                    !(hints.flags & XCB_ICCCM_WM_HINT_INPUT) || hints.input,
                    client->supports_take_focus
                );

                bool urgent = (hints.flags & XUrgencyHint) != 0;
                if (urgent)
                {
                    if (client->presentation.ignore_next_wm_hints_urgency_echo)
                        state_.presentation(client->id).ignore_next_wm_hints_urgency_echo = false;
                    else if (e.window != active_window_)
                        state_.urgency(client->id, UrgencySource::App, true);
                }
                else
                {
                    if (!client->urgency.active())
                        state_.presentation(client->id).ignore_next_wm_hints_urgency_echo = false;
                    if (e.window != active_window_)
                    {
                        // If App was set, removing it re-syncs (and re-asserts WM_HINTS
                        // if WM still wants urgency). If only WM is set, app's WM_HINTS
                        // clear must be undone via an explicit re-assert.
                        if (client->urgency.has(UrgencySource::App))
                            state_.urgency(client->id, UrgencySource::App, false);
                        else if (client->urgency.has(UrgencySource::WmInitiated))
                            request_urgency_update(*client);
                    }
                }
            }
            else
            {
                state_.focus_hints(e.window, true, client->supports_take_focus); // ICCCM default when WM_HINTS absent
            }

            if (active_window_ == e.window && !is_focus_candidate(*client))
            {
                focus_or_fallback(monitors_[client->monitor], false);
            }
        }
    }
    else if (wm_protocols_ != XCB_NONE && e.atom == wm_protocols_)
    {
        if (auto* client = get_client(e.window))
        {
            state_.focus_hints(e.window, client->accepts_input, supports_protocol(e.window, wm_take_focus_));
            if (active_window_ == e.window && !is_focus_candidate(*client))
                repair_focus_after_visibility_change(client->monitor, false);
        }
    }
    else if (auto const* strut_client = get_client(e.window); strut_client && strut_client->kind() == Client::Kind::Dock
             && (e.atom == ewmh_.get()->_NET_WM_STRUT || e.atom == ewmh_.get()->_NET_WM_STRUT_PARTIAL))
    {
        request_workarea_update();
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
                state_.user_time(id, get_user_time(id), client->user_time_window);
        }
        if (auto* c = get_client(e.window))
        {
            if (e.atom == net_wm_user_time_window_)
                refresh_user_time_tracking(e.window);
            else
                state_.user_time(e.window, get_user_time(e.window), c->user_time_window);
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
    detect_monitors();
    request_workarea_update();
    refresh_workareas();
    state_.fit_floating();

    // Update EWMH for new monitor configuration
    effects_.desktop_metadata = true;
    request_current_desktop_update();

    // Focus a window after reconfiguration
    if (!monitors_.empty())
    {
        focus_or_fallback(monitors_[focused_monitor_]);
    }

    effects_.drain_crossing = true;
}

} // namespace lwm
