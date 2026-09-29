#include "lwm/core/floating.hpp"
#include "wm.hpp"
#include <algorithm>
#include <utility>
#include <xcb/xcb_icccm.h>

namespace lwm {

void WindowManager::invalidate_monitor(size_t monitor, xcb_window_t preferred)
{
    if (monitor >= monitors_.size())
        return;
    auto [entry, inserted] = effects_.monitors.try_emplace(monitor, preferred);
    if (preferred != XCB_NONE)
        entry->second = preferred;
    effects_.drain_crossing |= !drag_active();
    effects_.stacking = true;
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
    return client.tiled_geometry;
}

void WindowManager::dispatch_event(xcb_generic_event_t const& event)
{
    auto current = event;
    if ((event.response_type & ~0x80) == XCB_MOTION_NOTIFY && std::holds_alternative<TiledResize>(drag_state_))
    {
        for (;;)
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
            current = next;
            deferred_events_.pop_front();
        }
    }
    handle_event(current);
    complete_transition();
}

void WindowManager::commit_focus()
{
    if (!effects_.previous_focus)
        return;
    auto previous = *effects_.previous_focus;
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
        clear_client_urgency(*client);
        uint32_t color = border_color_for_client(*client);
        xcb_change_window_attributes(conn_.get(), client->id, XCB_CW_BORDER_PIXEL, &color);
        send_wm_take_focus(*client, effects_.focus_time ? effects_.focus_time : last_event_time_);
        xcb_set_input_focus(conn_.get(), XCB_INPUT_FOCUS_POINTER_ROOT, client->id, effects_.focus_time);
        ewmh_.set_window_state(client->id, net_wm_state_focused_, true);
        if (ipc_.has_subscribers(Event_FocusChange))
        {
            queue_event(
                Event_FocusChange,
                "{\"event\":\"focus_change\",\"window\":" + std::to_string(client->id) + ",\"class\":\""
                    + json_escape(client->wm_class) + "\",\"title\":\"" + json_escape(client->name) + "\"}"
            );
        }
    }
    else
        xcb_set_input_focus(conn_.get(), XCB_INPUT_FOCUS_POINTER_ROOT, conn_.screen()->root, XCB_CURRENT_TIME);
    ewmh_.set_active_window(active_window_);
    request_current_desktop_update();
    effects_.stacking = true;
}

void WindowManager::request_urgency_update(Client& client) { effects_.urgency.insert(client.id); }
void WindowManager::request_allowed_actions(Client const& client) { effects_.allowed_actions.insert(client.id); }
void WindowManager::request_client_list_update() { effects_.client_list = true; }
void WindowManager::request_current_desktop_update() { effects_.current_desktop = true; }

void WindowManager::complete_transition()
{
    refresh_workareas();
    // Consume pending ownership preferences once. Later phases use resolved owners.
    auto affected_monitors = std::exchange(effects_.monitors, {});
    for (auto [monitor, preferred] : affected_monitors)
        if (monitor < monitors_.size())
            realize_visibility(monitor, preferred);
    if (auto const* active = get_client(active_window_);
        active && (!is_focus_eligible(*active) || !is_visible(*active)))
        repair_focus_after_visibility_change(focused_monitor_, false);
    if (active_window_ == XCB_NONE && effects_.repair_focus && !effects_.previous_focus && !showing_desktop_)
        focus_or_fallback(focused_monitor(), false);
    if (affected_monitors.empty() && effects_ == TransitionEffects{} && !ewmh_.has_pending_window_states())
    {
        LWM_ASSERT_INVARIANTS(clients_, monitors_, active_window_);
        return;
    }
    for (auto [monitor, preferred] : affected_monitors)
        if (monitor < monitors_.size())
            arrange_monitor(monitors_[monitor]);
    if (!affected_monitors.empty())
    {
        for (auto const& [window, client] : clients_)
            if (client.kind() == Client::Kind::Floating && affected_monitors.contains(client.monitor))
                request_geometry(client);
    }
    // The output cache coalesces duplicates without reordering geometry writes.
    // Keep a split resize's configure requests together on the server.
    bool resizing_tiles = std::holds_alternative<TiledResize>(drag_state_) && !effects_.geometry.empty();
    if (resizing_tiles)
        xcb_grab_server(conn_.get());
    for (auto window : effects_.geometry)
        if (auto* client = get_client(window); client && is_visible(*client))
            write_geometry(*client, presentation_geometry(*client), border_width_for_client(*client));
    if (resizing_tiles)
        xcb_ungrab_server(conn_.get());
    for (auto window : effects_.configure_replies)
        if (auto const* client = get_client(window))
            publish_configure_notify(*client);
    for (auto window : effects_.maps)
        if (is_managed(window))
            xcb_map_window(conn_.get(), window);
    commit_focus();
    for (auto window : effects_.urgency)
        if (auto* client = get_client(window))
            publish_urgency(*client);
    for (auto window : effects_.desktops)
        if (auto* client = get_client(window))
            ewmh_.set_window_desktop(
                window,
                client->sticky ? 0xFFFFFFFF : get_ewmh_desktop_index(client->monitor, client->workspace)
            );
    for (auto window : effects_.iconic)
        if (auto* client = get_client(window); client && wm_state_ != XCB_NONE)
        {
            uint32_t data[] = { client->iconic ? XCB_ICCCM_WM_STATE_ICONIC : XCB_ICCCM_WM_STATE_NORMAL, 0 };
            xcb_change_property(conn_.get(), XCB_PROP_MODE_REPLACE, window, wm_state_, wm_state_, 32, 2, data);
        }
    for (auto window : effects_.allowed_actions)
        if (auto* client = get_client(window))
            publish_allowed_actions(*client);
    if (effects_.client_list)
        publish_client_list();
    if (effects_.current_desktop)
        publish_current_desktop();
    ewmh_.flush_window_states();
    if (effects_.stacking)
        apply_stacking();
    if (effects_.drain_crossing)
        flush_and_drain_crossing();
    conn_.flush();
    for (auto const& [monitor, change] : effects_.workspace_events)
        if (change.first != change.second && ipc_.has_subscribers(Event_WorkspaceSwitch))
        {
            queue_event(
                Event_WorkspaceSwitch,
                "{\"event\":\"workspace_switch\",\"monitor\":" + std::to_string(monitor)
                    + ",\"from\":" + std::to_string(change.first) + ",\"to\":" + std::to_string(change.second) + "}"
            );
        }
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
        effects_.events.begin(),
        effects_.events.end(),
        [&](auto const& a, auto const& b) { return priority(a.first) < priority(b.first); }
    );
    for (auto const& [type, json] : effects_.events) ipc_.emit(type, json);
    effects_ = {};
    LWM_ASSERT_INVARIANTS(clients_, monitors_, active_window_);
}

} // namespace lwm
