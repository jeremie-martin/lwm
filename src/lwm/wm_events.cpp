#include "lwm/core/focus.hpp"
#include "lwm/core/log.hpp"
#include "lwm/core/policy.hpp"
#include "wm.hpp"
#include <xcb/xcb_icccm.h>

namespace lwm {

namespace {

uint32_t event_time(uint8_t type, xcb_generic_event_t const& event)
{
    switch (type)
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
    uint8_t type = event.response_type & ~0x80;
    if (type == 0)
    {
        auto const& error = reinterpret_cast<xcb_generic_error_t const&>(event);
        // Unchecked requests can race with client destruction. Keep those visible
        // at DEBUG, without letting a broken client flood the diagnostic queue.
        if (error.error_code == XCB_WINDOW || error.error_code == XCB_DRAWABLE)
            LWM_LOG_DEBUG_LIMIT(
                std::chrono::seconds(5),
                "X resource error: code={} opcode={}:{} resource={:#x} sequence={}",
                error.error_code,
                error.major_code,
                error.minor_code,
                error.resource_id,
                error.full_sequence
            );
        else
            LWM_LOG_WARN_LIMIT(
                std::chrono::seconds(5),
                "X protocol error: code={} opcode={}:{} resource={:#x} sequence={}",
                error.error_code,
                error.major_code,
                error.minor_code,
                error.resource_id,
                error.full_sequence
            );
        return;
    }
    if (uint32_t time = event_time(type, event))
    {
        last_event_time_ = time;
        if (type != XCB_PROPERTY_NOTIFY)
            last_input_time_ = time;
    }
    if (conn_.has_randr())
    {
        if (type == conn_.randr_event_base() + XCB_RANDR_SCREEN_CHANGE_NOTIFY)
        {
            monitors_dirty_ = true;
            return;
        }
        if (type == conn_.randr_event_base() + XCB_RANDR_NOTIFY)
        {
            auto const& notify = reinterpret_cast<xcb_randr_notify_event_t const&>(event);
            if (notify.subCode == XCB_RANDR_NOTIFY_CRTC_CHANGE || notify.subCode == XCB_RANDR_NOTIFY_OUTPUT_CHANGE)
                monitors_dirty_ = true;
            return;
        }
    }

    switch (type)
    {
        case XCB_CONFIGURE_NOTIFY:
        {
            // A mismatch is usually LWM's own superseded configure, but may be an
            // external change: forget the written geometry so the next publication
            // rewrites it, without forcing one now.
            auto const& e = reinterpret_cast<xcb_configure_notify_event_t const&>(event);
            if (auto it = outputs_.find(e.window); it != outputs_.end() && it->second.geometry
                && (*it->second.geometry != Geometry{ e.x, e.y, e.width, e.height }
                    || it->second.border_width != e.border_width))
                it->second.geometry.reset();
            break;
        }
        case XCB_MAP_REQUEST:
            handle_map_request(reinterpret_cast<xcb_map_request_event_t const&>(event));
            break;
        // LWM never unmaps managed windows, so any UnmapNotify is a client withdrawal.
        case XCB_UNMAP_NOTIFY:
            handle_window_removal(reinterpret_cast<xcb_unmap_notify_event_t const&>(event).window);
            break;
        case XCB_DESTROY_NOTIFY:
            handle_window_removal(reinterpret_cast<xcb_destroy_notify_event_t const&>(event).window);
            break;
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
        case XCB_SELECTION_CLEAR:
            if (reinterpret_cast<xcb_selection_clear_event_t const&>(event).selection == atoms_.wm_s0)
            {
                LWM_LOG_INFO("Stopping: WM_S0 ownership transferred to another manager");
                running_ = false;
            }
            break;
    }
}

// ---------------------------------------------------------------------------
// Window lifetime
// ---------------------------------------------------------------------------

void WindowManager::handle_map_request(xcb_map_request_event_t const& e)
{
    if (state_.find(e.window))
    {
        state_.restore(e.window, true);
        return;
    }
    if (state_.find_fixture(e.window) || is_override_redirect(e.window))
        return;
    manage_window(e.window, classify_window(e.window), false);
    if (!ipc_.has_subscribers(Event_WindowMap))
        return;
    if (auto const* client = state_.find(e.window))
        queue_event(event::WindowMap{ e.window,
                                      client->wm_class,
                                      client_kind_str(client->kind()),
                                      Placement{ client->monitor, client->workspace } });
    else if (auto const* fixture = state_.find_fixture(e.window))
        queue_event(event::WindowMap{ e.window, "", fixture_role_str(fixture->role), std::nullopt });
    else
        queue_event(event::WindowMap{ e.window, "", "popup", std::nullopt });
}

void WindowManager::handle_window_removal(xcb_window_t window)
{
    if (auto const* client = state_.find(window))
        queue_event(event::WindowUnmap{ window, client_kind_str(client->kind()), Placement{ client->monitor, client->workspace } });
    else if (auto const* fixture = state_.find_fixture(window))
    {
        queue_event(event::WindowUnmap{ window, fixture_role_str(fixture->role), std::nullopt });
        workareas_dirty_ |= fixture->role == Fixture::Role::Dock;
    }
    else
        return;
    pending_kills_.erase(window);
    state_.erase(window);
}

// ---------------------------------------------------------------------------
// Pointer and keyboard
// ---------------------------------------------------------------------------

void WindowManager::handle_enter_notify(xcb_enter_notify_event_t const& e)
{
    if (drag_active())
        return;
    xcb_window_t root = conn_.screen()->root;
    if (e.event != root)
    {
        if (e.mode != XCB_NOTIFY_MODE_NORMAL || e.detail == XCB_NOTIFY_DETAIL_INFERIOR)
            return;
        if (auto const* client = state_.find(e.event))
        {
            if (state_.visible(*client) && e.event != state_.active_window())
                state_.focus(e.event);
            return;
        }
    }
    // Root, fixtures and unmanaged windows select the focused monitor.
    focus_monitor_at_point(e.root_x, e.root_y);
}

void WindowManager::handle_motion_notify(xcb_motion_notify_event_t const& e)
{
    if (drag_active())
    {
        update_drag(e.root_x, e.root_y);
        return;
    }
    // Root motion names the child under the pointer. Moving within a managed
    // window re-establishes focus after another window took it.
    xcb_window_t root = conn_.screen()->root;
    xcb_window_t under = e.event == root && e.child != XCB_NONE ? e.child : e.event;
    if (auto const* client = state_.find(under))
    {
        if (state_.visible(*client) && under != state_.active_window())
            state_.focus(under);
        return;
    }
    if (auto hit = hit_split_border(e.root_x, e.root_y))
        set_root_cursor(hit->hit.direction == SplitDirection::Horizontal ? cursor_resize_h_ : cursor_resize_v_);
    else
        set_root_cursor(cursor_default_);
    focus_monitor_at_point(e.root_x, e.root_y);
}

MousebindConfig const* WindowManager::resolve_mouse_binding(uint16_t state, uint8_t button) const
{
    auto modifiers = binding_modifiers(state);
    for (auto const& binding : config_.mousebinds)
        if (binding.button == button && binding.modifier == modifiers)
            return &binding;
    return nullptr;
}

void WindowManager::handle_button_press(xcb_button_press_event_t const& e)
{
    xcb_window_t root = conn_.screen()->root;
    bool from_window_grab = e.event != root;
    xcb_window_t target = !from_window_grab && e.child != XCB_NONE ? e.child : e.event;
    auto const* client = state_.find(target);
    // Managed windows use a passive SYNC grab for click-to-focus; release it.
    auto allow = [&](uint8_t mode)
    {
        if (from_window_grab)
            xcb_allow_events(conn_.get(), mode, e.time);
    };
    if (client && !state_.visible(*client))
    {
        allow(XCB_ALLOW_ASYNC_POINTER);
        return;
    }

    // Modifier bindings are handled here because ReplayPointer skips ancestor
    // passive grabs such as the root grabs installed by grab_buttons().
    if (auto const* binding = resolve_mouse_binding(e.state, e.detail))
    {
        bool handled = false;
        switch (binding->action)
        {
            case MouseAction::DragWindow:
                if (client)
                {
                    allow(XCB_ALLOW_ASYNC_POINTER);
                    begin_window_drag(target, e.root_x, e.root_y, e.detail);
                    handled = true;
                }
                break;
            case MouseAction::ResizeFloating:
                // Tiled and root clicks prefer a split; otherwise resize a
                // floating window or convert a tile after acquiring the pointer.
                if (!client || client->kind() == Client::Kind::Tiled)
                    if (auto hit = hit_split_border(e.root_x, e.root_y))
                    {
                        allow(XCB_ALLOW_ASYNC_POINTER);
                        begin_tiled_resize(hit->hit, hit->monitor, e.root_x, e.root_y, e.detail);
                        handled = true;
                        break;
                    }
                if (client)
                {
                    allow(XCB_ALLOW_ASYNC_POINTER);
                    using Edge = floating::ResizeEdge;
                    begin_window_drag(target, e.root_x, e.root_y, e.detail, Edge::Right | Edge::Bottom);
                    handled = true;
                }
                break;
            case MouseAction::ToggleFloat:
                if (client)
                {
                    allow(XCB_ALLOW_ASYNC_POINTER);
                    toggle_float(target);
                    handled = true;
                }
                break;
        }
        if (handled)
            return;
    }

    // Ordinary clicks focus the client and still reach it.
    if (client && target != state_.active_window())
        state_.focus(target);
    if (from_window_grab)
        allow(XCB_ALLOW_REPLAY_POINTER);
    if (client || from_window_grab)
        return;

    // A plain (or Ctrl) left click on an empty gap resizes the split under it;
    // a double or Ctrl click resets that split.
    uint16_t modifiers = binding_modifiers(e.state) & ~XCB_MOD_MASK_CONTROL;
    if (target == root && e.child == XCB_NONE && e.detail == 1 && modifiers == 0)
        if (auto hit = hit_split_border(e.root_x, e.root_y))
        {
            bool ctrl = (e.state & XCB_MOD_MASK_CONTROL) != 0;
            auto elapsed = static_cast<int32_t>(e.time - last_gap_click_time_);
            bool double_click = !ctrl && elapsed > 0 && elapsed < 400 && last_gap_click_address_ == hit->hit.address
                && last_gap_click_monitor_ == hit->monitor;
            last_gap_click_time_ = e.time;
            last_gap_click_address_ = hit->hit.address;
            last_gap_click_monitor_ = hit->monitor;
            if (double_click || ctrl)
            {
                state_.erase_ratio(hit->monitor, hit->hit.address);
                last_gap_click_time_ = 0;
                return;
            }
            begin_tiled_resize(hit->hit, hit->monitor, e.root_x, e.root_y, e.detail);
            return;
        }
    focus_monitor_at_point(e.root_x, e.root_y);
}

void WindowManager::handle_button_release(xcb_button_release_event_t const& e)
{
    if (!drag_active() || (drag_->button && drag_->button != e.detail))
        return;
    update_drag(e.root_x, e.root_y);
    end_drag();
}

// X11 auto-repeat sends KeyRelease/KeyPress pairs with identical timestamps.
bool WindowManager::is_auto_repeat_toggle(xcb_keysym_t keysym, xcb_timestamp_t time)
{
    if (keysym == last_toggle_keysym_ && time == last_toggle_release_time_)
    {
        LWM_LOG_TRACE("Key repeat suppressed: keysym={:#x} time={}", keysym, time);
        return true;
    }
    last_toggle_keysym_ = keysym;
    last_toggle_release_time_ = 0;
    return false;
}

void WindowManager::handle_key_press(xcb_key_press_event_t const& e)
{
    xcb_keysym_t keysym = xcb_key_press_lookup_keysym(conn_.keysyms(), const_cast<xcb_key_press_event_t*>(&e), 0);
    auto binding = config_.keybinds.find({ binding_modifiers(e.state), keysym });
    if (binding == config_.keybinds.end())
    {
        LWM_LOG_TRACE("Key unbound: keysym={:#x} modifiers={:#x}", keysym, e.state);
        return;
    }
    // Executing may reload and replace the configuration that owns the binding.
    Action action = binding->second;
    LWM_LOG_TRACE("Key action: action={} keysym={:#x} modifiers={:#x}", action_name(action), keysym, e.state);
    if (std::holds_alternative<action::ToggleWorkspace>(action) && is_auto_repeat_toggle(keysym, e.time))
        return;
    queue_event(event::KeyAction{ action_name(action) });
    if (auto result = execute(action, "keybind"); !result)
        LWM_LOG_DEBUG("Key action {} failed: {}", action_name(action), result.error());
}

void WindowManager::handle_key_release(xcb_key_release_event_t const& e)
{
    xcb_keysym_t keysym = xcb_key_press_lookup_keysym(conn_.keysyms(), const_cast<xcb_key_release_event_t*>(&e), 0);
    if (keysym == last_toggle_keysym_)
        last_toggle_release_time_ = e.time;
}

// ---------------------------------------------------------------------------
// Client messages
// ---------------------------------------------------------------------------

void WindowManager::handle_client_message(xcb_client_message_event_t const& e)
{
    auto* ewmh = ewmh_.get();
    auto const* client = state_.find(e.window);
    if (e.type == ewmh->WM_PROTOCOLS && e.data.data32[0] == ewmh->_NET_WM_PING)
    {
        xcb_window_t window = e.data.data32[2] != XCB_NONE ? e.data.data32[2] : e.window;
        pending_kills_.erase(window);
    }
    else if (e.type == ewmh->_NET_CLOSE_WINDOW)
        kill_window(e.window);
    else if (e.type == ewmh->_NET_WM_FULLSCREEN_MONITORS && client)
        state_.fullscreen_monitors(
            e.window,
            FullscreenMonitors{ e.data.data32[0], e.data.data32[1], e.data.data32[2], e.data.data32[3] }
        );
    else if (e.type == atoms_.wm_change_state && e.data.data32[0] == WM_STATE_ICONIC && client)
        state_.iconic(e.window, true);
    else if (e.type == ewmh->_NET_WM_STATE)
        handle_wm_state_change(e);
    else if (e.type == ewmh->_NET_CURRENT_DESKTOP)
        switch_to_desktop(e.data.data32[0]);
    else if (e.type == ewmh->_NET_ACTIVE_WINDOW)
        handle_active_window_request(e);
    else if (e.type == ewmh->_NET_WM_DESKTOP)
        handle_desktop_change(e);
    else if (e.type == ewmh->_NET_REQUEST_FRAME_EXTENTS)
        xcb_ewmh_set_frame_extents(ewmh_.get(), e.window, 0, 0, 0, 0);
    else if (e.type == ewmh->_NET_MOVERESIZE_WINDOW)
        handle_moveresize_window(e);
    else if (e.type == ewmh->_NET_WM_MOVERESIZE)
        handle_wm_moveresize(e);
    else if (e.type == ewmh->_NET_SHOWING_DESKTOP)
        handle_showing_desktop(e);
    else if (e.type == ewmh->_NET_RESTACK_WINDOW)
        handle_restack_message(e);
}

// Managed clients keep LWM's global order. Other windows are restacked as
// requested, after which LWM reasserts managed ordering.
void WindowManager::handle_restack_message(xcb_client_message_event_t const& e)
{
    restack_requested_ = true;
    if (state_.find(e.window))
        return;
    xcb_window_t sibling = e.data.data32[1];
    uint32_t values[2] = { e.data.data32[2], 0 };
    uint16_t mask = XCB_CONFIG_WINDOW_STACK_MODE;
    if (sibling != XCB_NONE)
    {
        mask |= XCB_CONFIG_WINDOW_SIBLING;
        values[0] = sibling;
        values[1] = e.data.data32[2];
    }
    xcb_configure_window(conn_.get(), e.window, mask, values);
}

void WindowManager::handle_wm_state_change(xcb_client_message_event_t const& e)
{
    auto* ewmh = ewmh_.get();
    uint32_t action = e.data.data32[0];
    auto const* client = state_.find(e.window);
    if (action > 2 || !client)
        return;
    xcb_atom_t first = e.data.data32[1];
    xcb_atom_t second = e.data.data32[2];
    auto requested = [&](xcb_atom_t atom) { return first == atom || second == atom; };
    auto enable = [action](bool current) { return action == 2 ? !current : action == 1; };
    auto value = [&](xcb_atom_t atom, bool current) { return requested(atom) ? enable(current) : current; };
    xcb_window_t id = client->id;
    // Read the complete request before changing anything. Fullscreen dominates
    // maximize independently of atom order; repeated atoms are applied once.
    bool fullscreen = value(ewmh->_NET_WM_STATE_FULLSCREEN, client->fullscreen);
    bool horizontal = value(ewmh->_NET_WM_STATE_MAXIMIZED_HORZ, client->maximized_horz);
    bool vertical = value(ewmh->_NET_WM_STATE_MAXIMIZED_VERT, client->maximized_vert);
    if (!fullscreen && client->fullscreen)
        state_.fullscreen(id, false);
    if (requested(ewmh->_NET_WM_STATE_ABOVE) || requested(ewmh->_NET_WM_STATE_BELOW))
    {
        auto layer = client->preferences.layer.value_or(effective_layer(*client));
        bool above = value(ewmh->_NET_WM_STATE_ABOVE, layer == LayerHint::Above);
        bool below = value(ewmh->_NET_WM_STATE_BELOW, layer == LayerHint::Below);
        if (requested(ewmh->_NET_WM_STATE_ABOVE) && above)
            below = false;
        else if (requested(ewmh->_NET_WM_STATE_BELOW) && below)
            above = false;
        state_.layer(id, above ? LayerHint::Above : below ? LayerHint::Below : LayerHint::Normal);
    }
    if (requested(ewmh->_NET_WM_STATE_SKIP_TASKBAR))
        state_.skip_taskbar(id, enable(skips_taskbar(*client)));
    if (requested(ewmh->_NET_WM_STATE_SKIP_PAGER))
        state_.skip_pager(id, enable(skips_pager(*client)));
    if (requested(ewmh->_NET_WM_STATE_STICKY))
        state_.sticky(id, enable(client->sticky));
    if (requested(ewmh->_NET_WM_STATE_MODAL))
        state_.modal(id, enable(client->modal));
    if (requested(ewmh->_NET_WM_STATE_DEMANDS_ATTENTION))
        state_.urgency(id, UrgencySource::App, enable(client->urgency.has(UrgencySource::App)));
    if (requested(ewmh->_NET_WM_STATE_HIDDEN))
    {
        if (enable(client->iconic))
            state_.iconic(id, true);
        else
            state_.restore(id, false);
    }
    if (requested(ewmh->_NET_WM_STATE_MAXIMIZED_HORZ) || requested(ewmh->_NET_WM_STATE_MAXIMIZED_VERT))
        state_.maximize(id, horizontal, vertical);
    if (fullscreen && requested(ewmh->_NET_WM_STATE_FULLSCREEN))
        state_.request_fullscreen(id);
}

// Application requests (source 1) against another active client need a
// timestamp no older than its user time; rejected requests demand attention.
// No request surfaces a window suppressed by a current fullscreen owner.
void WindowManager::handle_active_window_request(xcb_client_message_event_t const& e)
{
    xcb_window_t window = e.window;
    uint32_t source = e.data.data32[0];
    uint32_t timestamp = e.data.data32[1];
    LWM_LOG_DEBUG("_NET_ACTIVE_WINDOW request: window={:#x} source={}", window, source);
    auto const* client = state_.find(window);
    if (!client)
        return;
    auto deny = [&](char const* reason)
    {
        LWM_LOG_DEBUG("Activation rejected: window={:#x} reason={}", window, reason);
        if (source == 1)
            state_.urgency(window, UrgencySource::WmInitiated, true);
    };
    xcb_window_t active = state_.active_window();
    if (source == 1 && active != XCB_NONE && active != window)
    {
        if (timestamp == 0)
            return deny("missing-timestamp");
        auto const* current = state_.find(active);
        if (current && current->user_time != 0 && ewmh_policy::timestamp_is_before(timestamp, current->user_time))
            return deny("stale-timestamp");
    }
    // Activation never surfaces a window suppressed by the owner of the placement it is shown on.
    bool shown = client->sticky || state_.shows(client->monitor, client->workspace);
    if (shown && state_.suppressed(*client))
        return deny("fullscreen-suppressed");
    state_.focus(window, source == 1 ? timestamp : 0);
}

void WindowManager::handle_desktop_change(xcb_client_message_event_t const& e)
{
    uint32_t desktop = e.data.data32[0];
    LWM_LOG_DEBUG("_NET_WM_DESKTOP request: window={:#x} desktop={}", e.window, desktop);
    auto const* client = state_.find(e.window);
    if (!client)
        return;
    if (desktop == 0xFFFFFFFF)
    {
        state_.sticky(e.window, true);
        return;
    }
    auto placement = ewmh_policy::desktop_placement(desktop, config_.workspaces.count, state_.monitors().size());
    if (!placement)
        return;
    auto [monitor, workspace] = *placement;
    state_.sticky(e.window, false);
    if (!state_.relocate(e.window, monitor, workspace, State::RelocationGeometry::Center))
        return;
    state_.pin_desktop(e.window, true);
    drain_requested_ = true;
}

void WindowManager::handle_moveresize_window(xcb_client_message_event_t const& e)
{
    auto const* client = state_.find(e.window);
    if (!client || client->kind() != Client::Kind::Floating)
        return;
    uint32_t flags = e.data.data32[0];
    auto geometry = floating_mode(*client)->geometry;
    if (flags & (1 << 8))
        geometry.x = geometry_coordinate(static_cast<int32_t>(e.data.data32[1]));
    if (flags & (1 << 9))
        geometry.y = geometry_coordinate(static_cast<int32_t>(e.data.data32[2]));
    if (flags & (1 << 10))
        geometry.width = geometry_extent(e.data.data32[3]);
    if (flags & (1 << 11))
        geometry.height = geometry_extent(e.data.data32[4]);
    update_floating_geometry(*client, geometry);
}

void WindowManager::handle_wm_moveresize(xcb_client_message_event_t const& e)
{
    uint32_t direction = e.data.data32[2];
    if (direction == 11) // _NET_WM_MOVERESIZE_CANCEL
    {
        if (drag_)
            if (auto const* move = std::get_if<WindowDrag>(&drag_->operation); move && move->window == e.window)
                end_drag(false);
        return;
    }
    auto const* client = state_.find(e.window);
    if (!client || client->kind() != Client::Kind::Floating || direction > 8 || e.data.data32[3] > 255)
        return;
    using Edge = floating::ResizeEdge;
    static constexpr Edge edges[] = { Edge::Top | Edge::Left,     Edge::Top,    Edge::Top | Edge::Right,
                                      Edge::Right,                Edge::Bottom | Edge::Right,
                                      Edge::Bottom,               Edge::Bottom | Edge::Left,
                                      Edge::Left,                 Edge::None };
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
    if (show == state_.showing_desktop())
        return;
    state_.show_desktop(show);
    drain_requested_ = true;
}

// ---------------------------------------------------------------------------
// Configure requests and properties
// ---------------------------------------------------------------------------

void WindowManager::handle_configure_request(xcb_configure_request_event_t const& e)
{
    if (auto const* client = state_.find(e.window))
    {
        // Every processed request of a managed client gets one acknowledgement.
        configure_replies_.insert(e.window);
        if (client->kind() == Client::Kind::Tiled || client->fullscreen)
            return;
        uint16_t geometry_mask =
            XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y | XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT;
        if (!(e.value_mask & geometry_mask))
            return;
        auto geometry = floating_mode(*client)->geometry;
        if (e.value_mask & XCB_CONFIG_WINDOW_X)
            geometry.x = e.x;
        if (e.value_mask & XCB_CONFIG_WINDOW_Y)
            geometry.y = e.y;
        if (e.value_mask & XCB_CONFIG_WINDOW_WIDTH)
            geometry.width = std::max<uint16_t>(1, e.width);
        if (e.value_mask & XCB_CONFIG_WINDOW_HEIGHT)
            geometry.height = std::max<uint16_t>(1, e.height);
        update_floating_geometry(*client, geometry);
        return;
    }

    // Unmanaged windows and fixtures are configured as requested.
    uint32_t values[7];
    size_t index = 0;
    for (auto [bit, value] : { std::pair<uint16_t, uint32_t>{ XCB_CONFIG_WINDOW_X, static_cast<uint32_t>(e.x) },
                               { XCB_CONFIG_WINDOW_Y, static_cast<uint32_t>(e.y) },
                               { XCB_CONFIG_WINDOW_WIDTH, e.width },
                               { XCB_CONFIG_WINDOW_HEIGHT, e.height },
                               { XCB_CONFIG_WINDOW_BORDER_WIDTH, e.border_width },
                               { XCB_CONFIG_WINDOW_SIBLING, e.sibling },
                               { XCB_CONFIG_WINDOW_STACK_MODE, e.stack_mode } })
        if (e.value_mask & bit)
            values[index++] = value;
    xcb_configure_window(conn_.get(), e.window, e.value_mask, values);
    if (e.value_mask & XCB_CONFIG_WINDOW_STACK_MODE)
        restack_requested_ = true;
}

// Floating geometry requests may move the client to the monitor under its center.
void WindowManager::update_floating_geometry(Client const& client, Geometry geometry)
{
    xcb_window_t id = client.id;
    state_.geometry(id, geometry);
    follow_floating_geometry(id);
    if (id == state_.active_window() && state_.visible(state_.require(id)))
        state_.focus_monitor(state_.require(id).monitor);
}

void WindowManager::handle_property_notify(xcb_property_notify_event_t const& e)
{
    auto* ewmh = ewmh_.get();
    auto const* client = state_.find(e.window);
    if (client && (e.atom == ewmh->_NET_WM_NAME || e.atom == XCB_ATOM_WM_NAME))
    {
        if (auto name = read_window_name(e.window); name != client->name)
        {
            state_.title(e.window, std::move(name));
            reevaluate_metadata(e.window);
        }
    }
    else if (client && e.atom == XCB_ATOM_WM_CLASS)
    {
        auto [instance, name] = read_wm_class(e.window);
        if (instance != client->wm_class_name || name != client->wm_class)
        {
            state_.window_class(e.window, std::move(instance), std::move(name));
            reevaluate_metadata(e.window);
        }
    }
    else if (client && (e.atom == ewmh->_NET_WM_WINDOW_TYPE || e.atom == XCB_ATOM_WM_TRANSIENT_FOR))
    {
        auto previous_parent = client->transient_for;
        if (e.atom == ewmh->_NET_WM_WINDOW_TYPE)
            state_.window_type(e.window, ewmh_.get_window_type_enum(e.window));
        else
            state_.transient(e.window, read_transient_for(e.window).value_or(XCB_NONE));
        relocate_to_transient_parent(e.window, previous_parent);
        reevaluate_metadata(e.window);
    }
    else if (client && e.atom == XCB_ATOM_WM_NORMAL_HINTS)
        handle_normal_hints(*client);
    else if (client && e.atom == XCB_ATOM_WM_HINTS)
        handle_wm_hints(*client);
    else if (client && e.atom == ewmh->WM_PROTOCOLS)
        state_.focus_hints(e.window, client->accepts_input, supports_protocol(e.window, atoms_.wm_take_focus));
    else if (auto const* fixture = state_.find_fixture(e.window); fixture && fixture->role == Fixture::Role::Dock
             && (e.atom == ewmh->_NET_WM_STRUT || e.atom == ewmh->_NET_WM_STRUT_PARTIAL))
        workareas_dirty_ = true;

    if (e.atom == ewmh->_NET_WM_USER_TIME || e.atom == ewmh->_NET_WM_USER_TIME_WINDOW)
    {
        std::vector<xcb_window_t> updated;
        for (auto const& [id, c] : state_.clients())
            if (c.user_time_window == e.window)
                updated.push_back(id);
        for (auto id : updated)
            state_.user_time(id, read_user_time(id, e.window), e.window);
        if (auto const* c = state_.find(e.window))
        {
            auto time_window = e.atom == ewmh->_NET_WM_USER_TIME_WINDOW ? read_user_time_window(e.window) : c->user_time_window;
            state_.user_time(e.window, read_user_time(e.window, time_window), time_window);
        }
    }
}

// Position and size hints update the normal floating rectangle, even while
// maximized or fullscreen. See X11.md for anchoring rules.
void WindowManager::handle_normal_hints(Client const& client)
{
    if (client.kind() != Client::Kind::Floating)
        return;
    bool anchored = client.transient_for != XCB_NONE;
    auto hints = read_size_hints(client.id, anchored);
    auto geometry = floating_mode(client)->geometry;
    geometry.width = hints.width.value_or(geometry.width);
    geometry.height = hints.height.value_or(geometry.height);
    if (hints.position)
    {
        Geometry hinted{ hints.position->first, hints.position->second, geometry.width, geometry.height };
        auto target = floating::resolve_position_hint(
            state_.monitors(),
            client.monitor,
            anchored || client.desktop_pinned,
            hinted
        );
        geometry = target.accepted ? hinted
                                   : floating::place_floating(
                                         state_.monitors()[target.monitor].working_area(),
                                         geometry.width,
                                         geometry.height,
                                         anchored ? placement_parent_geometry(client.transient_for) : std::nullopt
                                     );
    }
    update_floating_geometry(client, geometry);
}

void WindowManager::handle_wm_hints(Client const& client)
{
    xcb_window_t id = client.id;
    bool active = id == state_.active_window();
    auto& output = outputs_[id];
    xcb_icccm_wm_hints_t hints{ };
    if (!xcb_icccm_get_wm_hints_reply(conn_.get(), xcb_icccm_get_wm_hints(conn_.get(), id), &hints, nullptr))
        hints = { }; // Missing hints mean input is accepted and no app urgency is requested.
    state_.focus_hints(id, !(hints.flags & XCB_ICCCM_WM_HINT_INPUT) || hints.input, client.supports_take_focus);
    bool hinted_urgent = (hints.flags & XUrgencyHint) != 0;
    // WM_HINTS is shared with the application. A changed hint invalidates our
    // publication cache regardless of which urgency sources remain in State.
    if (output.urgent != hinted_urgent)
    {
        output.urgent.reset();
        presentation_dirty_ = true;
    }
    if (hinted_urgent)
    {
        if (std::exchange(output.ignore_urgency_echo, false))
            return;
        if (!active)
            state_.urgency(id, UrgencySource::App, true);
        return;
    }
    output.ignore_urgency_echo = false;
    // Clearing or deleting the hint withdraws only the application's request.
    // Publication reasserts any remaining WM-initiated urgency.
    if (!active)
        state_.urgency(id, UrgencySource::App, false);
}

void WindowManager::focus_monitor_at_point(int16_t x, int16_t y)
{
    auto monitor = focus::monitor_index_at_point(state_.monitors(), x, y);
    if (!monitor || *monitor == state_.focused_monitor())
        return;
    LWM_LOG_TRACE("Pointer changed monitor: {} -> {}", state_.focused_monitor(), *monitor);
    state_.focus_monitor(*monitor);
    state_.focus(XCB_NONE);
}

} // namespace lwm
