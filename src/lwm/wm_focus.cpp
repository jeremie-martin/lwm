#include "lwm/core/focus.hpp"
#include "lwm/core/log.hpp"
#include "lwm/core/policy.hpp"
#include "wm.hpp"

namespace lwm {

// Records focus intent; completion publishes the final choice once. Explicit
// activation may deiconify the client and switch to its workspace first.
void WindowManager::focus_window(xcb_window_t window, bool record_user_time, uint32_t timestamp)
{
    auto const* client = state_.find(window);
    if (state_.showing_desktop() || !client || !State::accepts_focus(*client))
        return;
    if (client->iconic)
        deiconify_window(window, false);
    state_.focus_monitor(client->monitor);
    if (!client->sticky)
        state_.switch_workspace(client->monitor, client->workspace);
    if (!state_.visible(*client))
    {
        focus_fallback(client->monitor, false);
        return;
    }
    focus_cycle_.reset();
    state_.focus(window, timestamp);
    uint32_t time = timestamp ? timestamp : last_input_time_;
    if (record_user_time && time
        && (!client->user_time || !ewmh_policy::timestamp_is_before(time, client->user_time)))
        state_.user_time(window, time, client->user_time_window);
}

void WindowManager::focus_fallback(size_t monitor, bool record_user_time)
{
    auto target = focus::fallback(state_, monitor);
    if (target != XCB_NONE)
        focus_window(target, record_user_time);
    else
        clear_focus();
}

void WindowManager::clear_focus()
{
    focus_cycle_.reset();
    state_.focus(XCB_NONE);
}

// Consecutive steps keep one recency order; eligibility is read per step.
// Ordinary activation, a new registration, or a changed monitor, workspace
// or active window starts a fresh traversal.
bool WindowManager::cycle_focus(bool forward)
{
    size_t monitor = state_.focused_monitor();
    size_t workspace = state_.monitors()[monitor].current_workspace;
    if (!focus_cycle_ || focus_cycle_->monitor != monitor || focus_cycle_->workspace != workspace
        || focus_cycle_->next_order != state_.next_order()
        || focus_cycle_->current != state_.active_window())
        focus_cycle_ = FocusTraversal{
            monitor, workspace, state_.next_order(), state_.active_window(), focus::recent_order(state_)
        };
    auto target = focus::cycle_target(focus_cycle_->order, state_, monitor, state_.active_window(), forward);
    if (target == XCB_NONE)
    {
        focus_cycle_.reset();
        return false;
    }
    auto traversal = std::move(*focus_cycle_);
    focus_window(target);
    focus_cycle_ = std::move(traversal);
    focus_cycle_->current = state_.active_window();
    return true;
}

} // namespace lwm
