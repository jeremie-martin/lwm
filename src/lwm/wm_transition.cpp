#include "lwm/core/floating.hpp"
#include "wm.hpp"
#include <algorithm>
#include <utility>
#include <xcb/xcb_icccm.h>

namespace lwm {

void WindowManager::invalidate_monitor(size_t monitor, xcb_window_t preferred)
{
    state_.invalidate(monitor, preferred);
}

void WindowManager::request_configure_notify(Client const& client) { effects_.configure_replies.insert(client.id); }

void WindowManager::request_geometry(Client const& client)
{
    if (client.kind() == Client::Kind::Tiled || client.kind() == Client::Kind::Floating)
        effects_.geometry.push_back(client.id);
}

Geometry WindowManager::presentation_geometry(Client const& client) const
{
    if (client.fullscreen)
        return fullscreen_geometry_for_client(client);
    if (client.kind() == Client::Kind::Floating)
        return floating::presentation_geometry(
            floating_geometry(client),
            monitors_[client.monitor].working_area(),
            client.maximized_horz,
            client.maximized_vert
        );
    if (drag_)
        if (auto const* move = std::get_if<WindowDrag>(&drag_->operation); move && move->window == client.id)
            return floating::drag_geometry(
                move->start_geometry,
                static_cast<int32_t>(drag_->last_x) - drag_->start_x,
                static_cast<int32_t>(drag_->last_y) - drag_->start_y,
                move->edges
            );
    return client.tiled_geometry;
}

void WindowManager::dispatch_event(
    xcb_generic_event_t const& event,
    size_t& remaining,
    std::chrono::steady_clock::time_point deadline
)
{
    auto current = event;
    if ((event.response_type & ~0x80) == XCB_MOTION_NOTIFY && drag_active())
    {
        while (remaining && std::chrono::steady_clock::now() < deadline)
        {
            if (deferred_events_.empty())
            {
                auto* next = xcb_poll_for_queued_event(conn_.get());
                if (!next)
                    break;
                deferred_events_.push_back(*next);
                free(next);
            }
            auto const& next = deferred_events_.front();
            if ((next.response_type & ~0x80) != XCB_MOTION_NOTIFY)
                break;
            --remaining;
            current = next;
            deferred_events_.pop_front();
        }
    }
    handle_event(current);
    complete_transition();
}

void WindowManager::commit_focus(TransitionEffects const& publication)
{
    if (!publication.previous_focus)
        return;
    auto previous = *publication.previous_focus;
    if (previous != XCB_NONE && previous != active_window_)
        ewmh_.set_window_state(previous, net_wm_state_focused_, false);
    if (auto* old = get_client(previous); old && previous != active_window_)
    {
        uint32_t color = border_color_for_client(*old);
        xcb_change_window_attributes(conn_.get(), previous, XCB_CW_BORDER_PIXEL, &color);
    }
    auto* client = get_client(active_window_);
    if (client)
    {
        uint32_t color = border_color_for_client(*client);
        xcb_change_window_attributes(conn_.get(), client->id, XCB_CW_BORDER_PIXEL, &color);
        send_wm_take_focus(*client, publication.focus_time ? publication.focus_time : last_event_time_);
        xcb_set_input_focus(conn_.get(), XCB_INPUT_FOCUS_POINTER_ROOT, client->id, publication.focus_time);
        ewmh_.set_window_state(client->id, net_wm_state_focused_, true);
    }
    else
        xcb_set_input_focus(conn_.get(), XCB_INPUT_FOCUS_POINTER_ROOT, conn_.screen()->root, XCB_CURRENT_TIME);
    ewmh_.set_active_window(active_window_);
}

void WindowManager::request_urgency_update(Client const& client) { effects_.urgency.insert(client.id); }
void WindowManager::request_allowed_actions(Client const& client) { effects_.allowed_actions.insert(client.id); }
void WindowManager::request_client_list_update() { effects_.client_list = true; }
void WindowManager::request_current_desktop_update() { effects_.current_desktop = true; }

void WindowManager::complete_transition()
{
    if (effects_ == TransitionEffects{ .state_changed = true } && !ewmh_.has_pending_window_states())
    {
        ipc_.emit(Event_StateChange, "{\"event\":\"state_change\"}");
        effects_ = { };
        LWM_ASSERT_INVARIANTS(clients_, monitors_, active_window_);
        return;
    }
    refresh_workareas();
    // Consume pending ownership preferences once. Later phases use resolved owners.
    auto affected_monitors = std::exchange(effects_.monitors, {});
    auto layout_monitors = std::exchange(effects_.layouts, { });
    effects_.drain_crossing |= (!affected_monitors.empty() || !layout_monitors.empty()) && !drag_active();
    for (auto [monitor, preferred] : affected_monitors)
        if (monitor < monitors_.size())
            realize_visibility(monitor, preferred);
    validate_drag(!affected_monitors.empty() || !layout_monitors.empty());
    if (auto const* active = get_client(active_window_); active && !is_focus_candidate(*active))
        repair_focus_after_visibility_change(focused_monitor_, false);
    if (active_window_ == XCB_NONE && effects_.repair_focus && !effects_.previous_focus && !showing_desktop_)
        focus_or_fallback(focused_monitor(), false);
    if (affected_monitors.empty() && layout_monitors.empty() && effects_ == TransitionEffects{ }
        && !ewmh_.has_pending_window_states())
    {
        LWM_ASSERT_INVARIANTS(clients_, monitors_, active_window_);
        return;
    }
    for (auto [monitor, preferred] : affected_monitors) layout_monitors.insert(monitor);
    for (auto monitor : layout_monitors)
        if (monitor < monitors_.size())
            arrange_monitor(monitors_[monitor]);
    if (!affected_monitors.empty())
    {
        for (auto const& [window, client] : clients_)
            if ((client.kind() == Client::Kind::Floating || client.fullscreen)
                && affected_monitors.contains(client.monitor))
                request_geometry(client);
    }
    if (effects_.previous_focus && get_client(active_window_))
        state_.clear_urgency(active_window_);
    if (effects_.previous_focus)
    {
        effects_.current_desktop = true;
        effects_.stacking = true;
        if (auto const* client = get_client(active_window_))
        {
            if (ipc_.has_subscribers(Event_FocusChange))
            {
                queue_event(
                    Event_FocusChange,
                    "{\"event\":\"focus_change\",\"window\":" + std::to_string(client->id) + ",\"class\":\""
                        + json_escape(client->wm_class) + "\",\"title\":\"" + json_escape(client->name) + "\"}"
                );
            }
        }
    }
    // Panels use client-list changes to refresh urgency. This dependency is
    // resolved before publication, never requested by a publisher.
    effects_.client_list |= !effects_.urgency.empty();
    effects_.stacking |= effects_.client_list;
    for (auto const& [monitor, change] : effects_.workspace_events)
        if (change.first != change.second && ipc_.has_subscribers(Event_WorkspaceSwitch))
        {
            queue_event(
                Event_WorkspaceSwitch,
                "{\"event\":\"workspace_switch\",\"monitor\":" + std::to_string(monitor)
                    + ",\"from\":" + std::to_string(change.first) + ",\"to\":" + std::to_string(change.second) + "}"
            );
        }
    // Freeze completion work. Publishers may acknowledge X bookkeeping, but
    // cannot schedule an earlier phase or mutate authoritative domain state.
    auto publication = state_.begin_publication();
    std::set<xcb_window_t> seen;
    std::erase_if(publication.geometry, [&](auto id) { return !seen.insert(id).second; });
    for (auto [id, visible] : publication.visibility)
        if (auto const* client = get_client(id))
        {
            auto& output = state_.presentation(id);
            output.hidden = !visible;
            if (!visible)
            {
                output.applied_geometry.reset();
                uint32_t x = static_cast<uint32_t>(OFF_SCREEN_X);
                xcb_configure_window(conn_.get(), id, XCB_CONFIG_WINDOW_X, &x);
            }
        }
    if (publication.appearance)
        publish_appearance();
    // Keep a split resize's configure requests together on the server.
    bool resizing_tiles =
        (drag_ && std::holds_alternative<TiledResize>(drag_->operation)) && !publication.geometry.empty();
    if (resizing_tiles)
        xcb_grab_server(conn_.get());
    for (auto window : publication.geometry)
        if (auto* client = get_client(window); client && is_visible(*client))
            write_geometry(*client, presentation_geometry(*client), border_width_for_client(*client));
    if (resizing_tiles)
        xcb_ungrab_server(conn_.get());
    for (auto window : publication.configure_replies)
        if (auto const* client = get_client(window))
            publish_configure_notify(*client);
    for (auto window : publication.maps)
        if (is_managed(window))
            xcb_map_window(conn_.get(), window);
    commit_focus(publication);
    for (auto window : publication.urgency)
        if (auto* client = get_client(window))
            publish_urgency(*client);
    for (auto window : publication.desktops)
        if (auto* client = get_client(window))
            ewmh_.set_window_desktop(
                window,
                client->sticky ? 0xFFFFFFFF : get_ewmh_desktop_index(client->monitor, client->workspace)
            );
    for (auto window : publication.iconic)
        if (auto* client = get_client(window); client && wm_state_ != XCB_NONE)
        {
            uint32_t data[] = { client->iconic ? XCB_ICCCM_WM_STATE_ICONIC : XCB_ICCCM_WM_STATE_NORMAL, 0 };
            xcb_change_property(conn_.get(), XCB_PROP_MODE_REPLACE, window, wm_state_, wm_state_, 32, 2, data);
        }
    for (auto window : publication.allowed_actions)
        if (auto* client = get_client(window))
            publish_allowed_actions(*client);
    if (publication.desktop_metadata)
        update_ewmh_desktops();
    else if (publication.workarea_property)
        update_ewmh_workarea();
    if (publication.showing_desktop)
        ewmh_.set_showing_desktop(showing_desktop_);
    for (auto id : publication.fullscreen_properties)
        if (auto const* client = get_client(id);
            client && client->fullscreen_monitors && net_wm_fullscreen_monitors_ != XCB_NONE)
        {
            auto const& m = *client->fullscreen_monitors;
            xcb_ewmh_set_wm_fullscreen_monitors(ewmh_.get(), id, m.top, m.bottom, m.left, m.right);
        }
    if (publication.client_list)
        publish_client_list();
    if (publication.current_desktop)
        publish_current_desktop();
    for (auto id : publication.states)
        if (auto const* c = get_client(id))
        {
            auto* atoms = ewmh_.get();
            for (auto [atom, enabled] : {
                     std::pair{     atoms->_NET_WM_STATE_FULLSCREEN,                     c->fullscreen },
                     {          atoms->_NET_WM_STATE_ABOVE, c->layer_hint == LayerHint::Above },
                     {          atoms->_NET_WM_STATE_BELOW, c->layer_hint == LayerHint::Below },
                     {         atoms->_NET_WM_STATE_STICKY,                         c->sticky },
                     {          atoms->_NET_WM_STATE_MODAL,                          c->modal },
                     {   atoms->_NET_WM_STATE_SKIP_TASKBAR,                   c->skip_taskbar },
                     {     atoms->_NET_WM_STATE_SKIP_PAGER,                     c->skip_pager },
                     { atoms->_NET_WM_STATE_MAXIMIZED_HORZ,                 c->maximized_horz },
                     { atoms->_NET_WM_STATE_MAXIMIZED_VERT,                 c->maximized_vert },
                     {         atoms->_NET_WM_STATE_HIDDEN,                         c->iconic }
            })
                ewmh_.set_window_state(id, atom, enabled);
        }
    ewmh_.flush_window_states();
    if (publication.stacking)
        apply_stacking();
    if (publication.drain_crossing)
        flush_and_drain_crossing();
    conn_.flush();
    auto priority = [](EventType type)
    {
        if (type == Event_WorkspaceSwitch)
            return 0;
        if (type == Event_FocusChange)
            return 1;
        if (type == Event_WindowMap || type == Event_WindowUnmap)
            return 2;
        return 3;
    };
    std::stable_sort(
        publication.events.begin(),
        publication.events.end(),
        [&](auto const& a, auto const& b) { return priority(a.first) < priority(b.first); }
    );
    for (auto const& [type, json] : publication.events) ipc_.emit(type, json);
    ipc_.emit(Event_StateChange, "{\"event\":\"state_change\"}");
    state_.end_publication();
    LWM_ASSERT_INVARIANTS(clients_, monitors_, active_window_);
}

} // namespace lwm
