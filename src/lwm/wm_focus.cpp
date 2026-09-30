#include "lwm/core/focus.hpp"
#include "lwm/core/log.hpp"
#include "lwm/core/policy.hpp"
#include "wm.hpp"
#include <xcb/xcb_icccm.h>

namespace lwm {

void WindowManager::focus_any_window(xcb_window_t window, bool record_user_time, uint32_t focus_timestamp)
{
    auto* client = get_client(window);
    if (showing_desktop_ || !client || client->monitor >= monitors_.size()
        || client->workspace >= monitors_[client->monitor].workspaces.size() || !focus::accepts_focus(*client))
        return;
    if (client->iconic)
        deiconify_window(window, false);
    state_.focus_monitor(client->monitor);
    if (!client->sticky)
        state_.switch_workspace(client->monitor, client->workspace);
    if (is_suppressed_by_fullscreen(*client))
    {
        focus_or_fallback(monitors_[client->monitor], false);
        return;
    }
    state_.focus(window, focus_timestamp);
    if (record_user_time)
    {
        uint32_t time = focus_timestamp ? focus_timestamp : last_input_time_;
        if (time && (!client->user_time || !ewmh_policy::timestamp_is_before(time, client->user_time)))
            state_.user_time(window, time, client->user_time_window);
    }
}

void WindowManager::clear_focus() { state_.focus(XCB_NONE); }

void WindowManager::focus_or_fallback(Monitor const& monitor, bool record_user_time)
{
    auto context = focus_context(monitor_index(monitor));
    auto target = focus::fallback(clients_, monitor, context);
    if (target != XCB_NONE)
        focus_any_window(target, record_user_time);
    else
        clear_focus();
}

void WindowManager::repair_focus_after_visibility_change(size_t preferred_monitor, bool record_user_time)
{
    if (monitors_.empty())
    {
        clear_focus();
        return;
    }

    if (auto* active = get_client(active_window_); active && is_focus_candidate(*active))
    {
        state_.focus_monitor(active->monitor);
        if (active->kind() == Client::Kind::Tiled && active->workspace < monitors_[active->monitor].workspaces.size())
            state_.remember_focus(active->monitor, active->workspace, active->id);
        request_current_desktop_update();
        return;
    }

    size_t target_monitor = preferred_monitor;
    if (target_monitor >= monitors_.size())
        target_monitor = focused_monitor_ < monitors_.size() ? focused_monitor_ : 0;
    focus_or_fallback(monitors_[target_monitor], record_user_time);
}

focus::Context WindowManager::focus_context(size_t monitor) const
{
    return { monitor, monitors_[monitor].current_workspace, effective_fullscreen_owner(monitor), showing_desktop_ };
}

bool WindowManager::is_focus_eligible(Client const& client) const
{
    // Explicit activation may deiconify or switch workspace before selecting focus.
    return focus::accepts_focus(client) && !is_suppressed_by_fullscreen(client);
}

bool WindowManager::is_focus_candidate(Client const& client) const
{
    return client.monitor < monitors_.size() && focus::eligible(client, focus_context(client.monitor));
}

void WindowManager::send_wm_take_focus(Client const& client, uint32_t timestamp)
{
    if (wm_protocols_ == XCB_NONE || wm_take_focus_ == XCB_NONE)
        return;

    if (!client.supports_take_focus)
        return;

    xcb_client_message_event_t ev = {};
    ev.response_type = XCB_CLIENT_MESSAGE;
    ev.window = client.id;
    ev.type = wm_protocols_;
    ev.format = 32;
    ev.data.data32[0] = wm_take_focus_;
    ev.data.data32[1] = timestamp;

    xcb_send_event(conn_.get(), 0, client.id, XCB_EVENT_MASK_NO_EVENT, reinterpret_cast<char*>(&ev));
}

bool WindowManager::cycle_focus(bool forward)
{
    if (focused_monitor_ >= monitors_.size())
        return false;
    auto context = focus_context(focused_monitor_);
    if (!focus_cycle_ || focus_cycle_->monitor != context.monitor || focus_cycle_->workspace != context.workspace
        || focus_cycle_->next_recency != state_.next_recency()
        || focus_cycle_->next_registration != state_.next_registration() || focus_cycle_->current != active_window_)
    {
        focus_cycle_ = FocusCycle{ context.monitor,       context.workspace,
                                   state_.next_recency(), state_.next_registration(),
                                   active_window_,        focus::recent_order(clients_) };
    }
    auto target = focus::cycle_target(focus_cycle_->order, clients_, context, active_window_, forward);
    if (target == XCB_NONE)
    {
        focus_cycle_.reset();
        return false;
    }
    focus_any_window(target);
    // Ordinary activation advances recency and thereby invalidates this traversal.
    // A cycling step acknowledges its own recency update without reordering IDs.
    focus_cycle_->next_recency = state_.next_recency();
    focus_cycle_->current = active_window_;
    return true;
}

} // namespace lwm
