#include "lwm/core/xproperty.hpp"
#include "lwm/core/focus.hpp"
#include "lwm/core/classification.hpp"
#include "lwm/core/log.hpp"
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

    switch (type)
    {
        case XCB_CONFIGURE_NOTIFY:
        {
            // A mismatch is usually LWM's own superseded configure, but may be an
            // external change: forget the written geometry so the next publication
            // rewrites it, without forcing one now.
            auto const& e = reinterpret_cast<xcb_configure_notify_event_t const&>(event);
            State::Presentation observed{ { e.x, e.y, e.width, e.height }, e.border_width };
            if (auto it = outputs_.find(e.window); it != outputs_.end() && it->second.presentation != observed)
                it->second.presentation.reset();
            break;
        }
        case XCB_MAP_REQUEST:
            handle_map_request(reinterpret_cast<xcb_map_request_event_t const&>(event));
            break;
        // LWM never unmaps managed windows, so any UnmapNotify is a client withdrawal.
        case XCB_UNMAP_NOTIFY:
        {
            auto window = reinterpret_cast<xcb_unmap_notify_event_t const&>(event).window;
            if (auto const* client = state_.find(window))
                write_geometry(window, outputs_[window], state_.presentation(*client));
            state_.erase(window);
            break;
        }
        case XCB_DESTROY_NOTIFY:
            state_.erase(reinterpret_cast<xcb_destroy_notify_event_t const&>(event).window);
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
        case XCB_CLIENT_MESSAGE:
            handle_client_message(reinterpret_cast<xcb_client_message_event_t const&>(event));
            break;
        case XCB_CONFIGURE_REQUEST:
            handle_configure_request(reinterpret_cast<xcb_configure_request_event_t const&>(event));
            break;
        case XCB_PROPERTY_NOTIFY:
            handle_property_notify(reinterpret_cast<xcb_property_notify_event_t const&>(event));
            break;
        // A changed keyboard mapping moves keysyms to other keycodes; refreshing
        // reports whether it was one.
        case XCB_MAPPING_NOTIFY:
        {
            auto mapping = reinterpret_cast<xcb_mapping_notify_event_t const&>(event);
            if (xcb_refresh_keyboard_mapping(conn_.keysyms(), &mapping))
                grab_keys();
            break;
        }
        case XCB_SELECTION_CLEAR:
            if (reinterpret_cast<xcb_selection_clear_event_t const&>(event).selection == atoms_.wm_s0)
            {
                LWM_LOG_INFO("Stopping: WM_S0 ownership transferred to another manager");
                stop_ = RunResult::Exit;
            }
            break;
    }
}

// Window lifetime

void WindowManager::handle_map_request(xcb_map_request_event_t const& e)
{
    if (state_.find(e.window))
    {
        state_.restore(e.window);
        return;
    }
    if (state_.find_fixture(e.window))
        return;
    xcb_window_t window = e.window;
    auto observed = observe({ &window, 1 }, false);
    if (observed.empty())
        return;
    state_.admit(observed.front());
    manage(observed.front(), false);
}


// Pointer and keyboard

void WindowManager::handle_enter_notify(xcb_enter_notify_event_t const& e)
{
    bool window_crossing = e.event != conn_.screen()->root;
    if (window_crossing && (e.mode != XCB_NOTIFY_MODE_NORMAL || e.detail == XCB_NOTIFY_DETAIL_INFERIOR))
        return;
    state_.hover(e.event, e.root_x, e.root_y);
}

void WindowManager::handle_motion_notify(xcb_motion_notify_event_t const& e)
{
    if (state_.drag())
    {
        state_.drag_to(e.root_x, e.root_y);
        return;
    }
    // Root motion names the child under the pointer. Moving within a managed
    // window re-establishes focus after another window took it.
    xcb_window_t under = e.event == conn_.screen()->root && e.child != XCB_NONE ? e.child : e.event;
    if (!state_.find(under))
    {
        auto hit = state_.split_at(e.root_x, e.root_y);
        set_root_cursor(hit ? split_cursor(*hit) : cursor_default_);
    }
    state_.hover(under, e.root_x, e.root_y);
}

// Managed windows use a passive SYNC grab for click-to-focus, which a consumed
// press releases and an ordinary click replays. Modifier bindings are
// interpreted here because ReplayPointer skips ancestor grabs such as the
// root grabs installed by grab_buttons().
void WindowManager::handle_button_press(xcb_button_press_event_t const& e)
{
    xcb_window_t root = conn_.screen()->root;
    bool from_window_grab = e.event != root;
    xcb_window_t target = from_window_grab ? e.event : e.child;
    auto [consumed, interaction, action] = state_.press(target, e.root_x, e.root_y, e.detail, e.state, e.time);
    if (from_window_grab)
        xcb_allow_events(conn_.get(), consumed ? XCB_ALLOW_ASYNC_POINTER : XCB_ALLOW_REPLAY_POINTER, e.time);
    if (interaction)
        begin_interaction(*interaction, e.root_x, e.root_y, e.detail);
    if (action)
        if (auto result = execute(*action, "mousebind"); !result)
            LWM_LOG_DEBUG("Mouse action failed: button={} error={}", e.detail, result.error());
}

void WindowManager::handle_button_release(xcb_button_release_event_t const& e)
{
    auto const& drag = state_.drag();
    if (!drag || (drag->button && drag->button != e.detail))
        return;
    state_.drag_to(e.root_x, e.root_y);
    state_.end_drag(true);
}

bool WindowManager::grab_pointer(xcb_cursor_t cursor)
{
    // A drag that ended earlier in this operation may still hold the grab.
    release_pointer();
    auto grab = reply(xcb_grab_pointer_reply(
        conn_.get(),
        xcb_grab_pointer(
            conn_.get(),
            0,
            conn_.screen()->root,
            XCB_EVENT_MASK_POINTER_MOTION | XCB_EVENT_MASK_BUTTON_RELEASE,
            XCB_GRAB_MODE_ASYNC,
            XCB_GRAB_MODE_ASYNC,
            XCB_NONE,
            cursor,
            XCB_CURRENT_TIME
        ),
        nullptr
    ));
    pointer_grabbed_ = grab && grab->status == XCB_GRAB_STATUS_SUCCESS;
    return pointer_grabbed_;
}

void WindowManager::release_pointer()
{
    if (!std::exchange(pointer_grabbed_, false))
        return;
    xcb_ungrab_pointer(conn_.get(), XCB_CURRENT_TIME);
    set_root_cursor(cursor_default_);
    drain_requested_ = true;
}

// Domain changes start only after the grab succeeds; completion releases it
// once State no longer has a drag.
void WindowManager::begin_interaction(State::Interaction const& interaction, int16_t x, int16_t y, uint8_t button)
{
    auto const* split = std::get_if<State::SplitHit>(&interaction);
    if (grab_pointer(split ? split_cursor(*split) : XCB_NONE))
        state_.begin_drag(interaction, x, y, button);
}

xcb_cursor_t WindowManager::split_cursor(State::SplitHit const& split) const
{
    return split.hit.direction == SplitDirection::Horizontal ? cursor_resize_h_ : cursor_resize_v_;
}

void WindowManager::handle_key_press(xcb_key_press_event_t const& e)
{
    xcb_keysym_t keysym = xcb_key_press_lookup_keysym(conn_.keysyms(), const_cast<xcb_key_press_event_t*>(&e), 0);
    auto const& keybinds = config().keybinds;
    auto binding = keybinds.find({ binding_modifiers(e.state), keysym });
    if (binding == keybinds.end())
    {
        LWM_LOG_TRACE("Key unbound: keysym={:#x} modifiers={:#x}", keysym, e.state);
        return;
    }
    // Executing may reload and replace the configuration that owns the binding.
    Action action = binding->second;
    LWM_LOG_TRACE("Key action: keysym={:#x} modifiers={:#x}", keysym, e.state);
    if (auto result = execute(action, "keybind"); !result)
        LWM_LOG_DEBUG("Key action failed: keysym={:#x} error={}", keysym, result.error());
}

// Client messages

void WindowManager::handle_client_message(xcb_client_message_event_t const& e)
{
    auto* ewmh = ewmh_.get();
    auto const* client = state_.find(e.window);
    if (e.type == atoms_.lwm_command && e.window == wm_window_)
        handle_command(e.data.data32[0]);
    // Only managed windows can be closed; LWM's own windows are never targets.
    else if (e.type == ewmh->_NET_CLOSE_WINDOW && (client || state_.find_fixture(e.window)))
        close_window(e.window);
    else if (e.type == ewmh->_NET_WM_FULLSCREEN_MONITORS && client)
        state_.fullscreen_monitors(
            e.window,
            FullscreenMonitors{ e.data.data32[0], e.data.data32[1], e.data.data32[2], e.data.data32[3] }
        );
    else if (e.type == ewmh->_NET_WM_STATE)
        handle_wm_state_change(e);
    else if (e.type == ewmh->_NET_CURRENT_DESKTOP)
    {
        LWM_LOG_DEBUG("_NET_CURRENT_DESKTOP request: desktop={}", e.data.data32[0]);
        state_.switch_desktop(e.data.data32[0]);
    }
    else if (e.type == ewmh->_NET_ACTIVE_WINDOW)
    {
        LWM_LOG_DEBUG("_NET_ACTIVE_WINDOW request: window={:#x} source={}", e.window, e.data.data32[0]);
        state_.request_activation(e.window, e.data.data32[0] == XCB_EWMH_CLIENT_SOURCE_TYPE_NORMAL, e.data.data32[1]);
    }
    else if (e.type == ewmh->_NET_WM_DESKTOP)
    {
        LWM_LOG_DEBUG("_NET_WM_DESKTOP request: window={:#x} desktop={}", e.window, e.data.data32[0]);
        state_.request_desktop(e.window, e.data.data32[0]);
    }
    else if (e.type == ewmh->_NET_REQUEST_FRAME_EXTENTS)
        xcb_ewmh_set_frame_extents(ewmh_.get(), e.window, 0, 0, 0, 0);
    else if (e.type == ewmh->_NET_MOVERESIZE_WINDOW)
        handle_moveresize_window(e);
    else if (e.type == ewmh->_NET_WM_MOVERESIZE)
        handle_wm_moveresize(e);
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
    xcb_configure_window_value_list_t values{ };
    values.sibling = e.data.data32[1];
    values.stack_mode = e.data.data32[2];
    uint16_t mask = XCB_CONFIG_WINDOW_STACK_MODE | (values.sibling != XCB_NONE ? XCB_CONFIG_WINDOW_SIBLING : 0);
    xcb_configure_window_aux(conn_.get(), e.window, mask, &values);
}

void WindowManager::handle_wm_state_change(xcb_client_message_event_t const& e)
{
    if (e.data.data32[0] > XCB_EWMH_WM_STATE_TOGGLE)
        return;
    xcb_atom_t const atoms[] = { e.data.data32[1], e.data.data32[2] };
    state_.request_states(e.window, static_cast<StateChange>(e.data.data32[0]), ewmh_.states(atoms));
}

void WindowManager::handle_moveresize_window(xcb_client_message_event_t const& e)
{
    uint32_t flags = e.data.data32[0];
    GeometryRequest request;
    if (flags & XCB_EWMH_MOVERESIZE_WINDOW_X)
        request.x = geometry_coordinate(static_cast<int32_t>(e.data.data32[1]));
    if (flags & XCB_EWMH_MOVERESIZE_WINDOW_Y)
        request.y = geometry_coordinate(static_cast<int32_t>(e.data.data32[2]));
    if (flags & XCB_EWMH_MOVERESIZE_WINDOW_WIDTH)
        request.width = geometry_extent(e.data.data32[3]);
    if (flags & XCB_EWMH_MOVERESIZE_WINDOW_HEIGHT)
        request.height = geometry_extent(e.data.data32[4]);
    state_.moveresize_request(e.window, request);
}

void WindowManager::handle_wm_moveresize(xcb_client_message_event_t const& e)
{
    uint32_t direction = e.data.data32[2];
    if (direction == XCB_EWMH_WM_MOVERESIZE_CANCEL)
        return state_.cancel_moveresize(e.window);
    if (direction > XCB_EWMH_WM_MOVERESIZE_MOVE || e.data.data32[3] > 255)
        return;
    using Edge = floating::ResizeEdge;
    static constexpr Edge edges[] = { Edge::Top | Edge::Left,     Edge::Top,    Edge::Top | Edge::Right,
                                      Edge::Right,                Edge::Bottom | Edge::Right,
                                      Edge::Bottom,               Edge::Bottom | Edge::Left,
                                      Edge::Left,                 Edge::None };
    if (auto interaction = state_.moveresize(e.window, edges[direction]))
        begin_interaction(
            *interaction,
            geometry_coordinate(static_cast<int32_t>(e.data.data32[0])),
            geometry_coordinate(static_cast<int32_t>(e.data.data32[1])),
            static_cast<uint8_t>(e.data.data32[3])
        );
}

// Configure requests and properties

void WindowManager::handle_configure_request(xcb_configure_request_event_t const& e)
{
    if (state_.find(e.window))
    {
        // Every processed request of a managed client gets one acknowledgement.
        configure_replies_.insert(e.window);
        GeometryRequest request;
        if (e.value_mask & XCB_CONFIG_WINDOW_X)
            request.x = e.x;
        if (e.value_mask & XCB_CONFIG_WINDOW_Y)
            request.y = e.y;
        if (e.value_mask & XCB_CONFIG_WINDOW_WIDTH)
            request.width = geometry_extent(e.width);
        if (e.value_mask & XCB_CONFIG_WINDOW_HEIGHT)
            request.height = geometry_extent(e.height);
        state_.configure_request(e.window, request);
        return;
    }

    // Unmanaged windows and fixtures are configured as requested.
    xcb_configure_window_value_list_t values{ e.x, e.y, e.width, e.height, e.border_width, e.sibling, e.stack_mode };
    xcb_configure_window_aux(conn_.get(), e.window, e.value_mask, &values);
    if (e.value_mask & XCB_CONFIG_WINDOW_STACK_MODE)
        restack_requested_ = true;
}

} // namespace lwm
