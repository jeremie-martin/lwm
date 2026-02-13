#include "lwm/core/focus.hpp"
#include "lwm/core/log.hpp"
#include "lwm/core/policy.hpp"
#include "wm.hpp"
#include <xcb/xcb_icccm.h>

namespace lwm {

void WindowManager::focus_window(xcb_window_t window)
{
    LOG_TRACE(
        "focus_window({:#x}) called, active_window_={:#x}, showing_desktop_={}",
        window,
        active_window_,
        showing_desktop_
    );

    if (showing_desktop_)
    {
        LOG_TRACE("focus_window: rejected (showing_desktop_)");
        return;
    }

    if (!is_focus_eligible(window))
    {
        LOG_TRACE("focus_window: rejected (not focus eligible)");
        return;
    }

    if (is_client_iconic(window))
    {
        LOG_DEBUG("focus_window: deiconifying window {:#x}", window);
        deiconify_window(window, false);
    }

    xcb_window_t previous_active = active_window_;

    bool is_sticky = is_client_sticky(window);
    LOG_TRACE("focus_window: calling focus_window_state, is_sticky={}", is_sticky);
    auto change = focus::focus_window_state(monitors_, focused_monitor_, active_window_, window, is_sticky);
    if (!change)
    {
        LOG_TRACE("focus_window: focus_window_state returned nullopt, returning");
        return;
    }

    LOG_DEBUG(
        "focus_window({:#x}): target_monitor={} workspace_changed={} "
        "old_ws={} new_ws={} prev_active={:#x}",
        window,
        change->target_monitor,
        change->workspace_changed,
        change->old_workspace,
        change->new_workspace,
        previous_active
    );

    auto& target_monitor = monitors_[change->target_monitor];
    if (change->workspace_changed)
    {
        LOG_DEBUG(
            "focus_window: WORKSPACE SWITCH TRIGGERED by focus! old_ws={} new_ws={}",
            change->old_workspace,
            change->new_workspace
        );
        for (xcb_window_t w : target_monitor.workspaces[change->old_workspace].windows)
        {
            if (is_client_sticky(w))
                continue;
            LOG_TRACE("focus_window: unmapping window {:#x} from old workspace", w);
            hide_window(w);
        }
        LOG_TRACE("focus_window: rearranging monitor after workspace switch");
        rearrange_monitor(target_monitor);
        LOG_TRACE("focus_window: updating floating visibility after workspace switch");
        update_floating_visibility(change->target_monitor);
    }

    LOG_TRACE("focus_window: updating EWMH current desktop");
    update_ewmh_current_desktop();

    clear_all_borders();

    xcb_change_window_attributes(conn_.get(), window, XCB_CW_BORDER_PIXEL, &config_.appearance.border_color);
    xcb_configure_window(conn_.get(), window, XCB_CONFIG_WINDOW_BORDER_WIDTH, &config_.appearance.border_width);
    xcb_timestamp_t focus_time = last_event_time_ ? last_event_time_ : XCB_CURRENT_TIME;
    send_wm_take_focus(window, last_event_time_);
    if (should_set_input_focus(window))
    {
        xcb_set_input_focus(conn_.get(), XCB_INPUT_FOCUS_POINTER_ROOT, window, focus_time);
    }
    else
    {
        xcb_set_input_focus(conn_.get(), XCB_INPUT_FOCUS_POINTER_ROOT, conn_.screen()->root, focus_time);
    }

    set_client_demands_attention(window, false);
    ewmh_.set_active_window(window);
    if (net_wm_state_focused_ != XCB_NONE)
    {
        if (previous_active != XCB_NONE && previous_active != window)
        {
            ewmh_.set_window_state(previous_active, net_wm_state_focused_, false);
        }
        ewmh_.set_window_state(window, net_wm_state_focused_, true);
    }

    if (auto* client = get_client(window))
        client->user_time = last_event_time_;

    restack_transients(window);
    update_ewmh_client_list();

    conn_.flush();
    LOG_TRACE("focus_window({:#x}): DONE", window);
}

void WindowManager::focus_floating_window(xcb_window_t window)
{
    LOG_TRACE(
        "focus_floating_window({:#x}) called, active_window_={:#x}, showing_desktop_={}",
        window,
        active_window_,
        showing_desktop_
    );

    if (showing_desktop_)
    {
        LOG_TRACE("focus_floating_window: rejected (showing_desktop_)");
        return;
    }

    auto* floating_window = find_floating_window(window);
    if (!floating_window)
    {
        LOG_TRACE("focus_floating_window: rejected (not a floating window)");
        return;
    }

    auto* client = get_client(window);
    if (!client)
    {
        LOG_TRACE("focus_floating_window: rejected (no client)");
        return;
    }

    if (!is_focus_eligible(window))
    {
        LOG_TRACE("focus_floating_window: rejected (not focus eligible)");
        return;
    }

    if (is_client_iconic(window))
    {
        LOG_DEBUG("focus_floating_window: deiconifying window {:#x}", window);
        deiconify_window(window, false);
    }

    if (client->monitor >= monitors_.size())
    {
        LOG_TRACE("focus_floating_window: rejected (invalid monitor index)");
        return;
    }

    xcb_window_t previous_active = active_window_;

    bool is_sticky = is_client_sticky(window);

    LOG_TRACE(
        "focus_floating_window: client->monitor={} client->workspace={} "
        "current_workspace={} is_sticky={}",
        client->monitor,
        client->workspace,
        monitors_[client->monitor].current_workspace,
        is_sticky
    );

    focused_monitor_ = client->monitor;
    auto& monitor = monitors_[client->monitor];
    if (!is_sticky && monitor.current_workspace != client->workspace)
    {
        LOG_DEBUG(
            "focus_floating_window({:#x}): WORKSPACE SWITCH TRIGGERED by focus! "
            "old_ws={} new_ws={}",
            window,
            monitor.current_workspace,
            client->workspace
        );
        for (xcb_window_t w : monitor.current().windows)
        {
            if (is_client_sticky(w))
                continue;
            LOG_TRACE("focus_floating_window: unmapping window {:#x} from old workspace", w);
            hide_window(w);
        }
        monitor.previous_workspace = monitor.current_workspace;
        monitor.current_workspace = client->workspace;
        LOG_TRACE("focus_floating_window: rearranging monitor after workspace switch");
        rearrange_monitor(monitor);
        LOG_TRACE("focus_floating_window: updating floating visibility after workspace switch");
        update_floating_visibility(client->monitor);
    }

    LOG_TRACE("focus_floating_window: updating EWMH current desktop");
    update_ewmh_current_desktop();

    if (focus_policy::promote_mru(floating_windows_, window, [](FloatingWindow const& fw) { return fw.id; }))
    {
        floating_window = &floating_windows_.back();
    }

    active_window_ = window;
    clear_all_borders();
    xcb_change_window_attributes(conn_.get(), window, XCB_CW_BORDER_PIXEL, &config_.appearance.border_color);
    xcb_configure_window(conn_.get(), window, XCB_CONFIG_WINDOW_BORDER_WIDTH, &config_.appearance.border_width);
    xcb_timestamp_t focus_time = last_event_time_ ? last_event_time_ : XCB_CURRENT_TIME;
    send_wm_take_focus(window, last_event_time_);
    if (should_set_input_focus(window))
    {
        xcb_set_input_focus(conn_.get(), XCB_INPUT_FOCUS_POINTER_ROOT, window, focus_time);
    }
    else
    {
        xcb_set_input_focus(conn_.get(), XCB_INPUT_FOCUS_POINTER_ROOT, conn_.screen()->root, focus_time);
    }

    uint32_t stack_mode = XCB_STACK_MODE_ABOVE;
    xcb_configure_window(conn_.get(), window, XCB_CONFIG_WINDOW_STACK_MODE, &stack_mode);

    set_client_demands_attention(window, false);
    ewmh_.set_active_window(window);
    if (net_wm_state_focused_ != XCB_NONE)
    {
        if (previous_active != XCB_NONE && previous_active != window)
        {
            ewmh_.set_window_state(previous_active, net_wm_state_focused_, false);
        }
        ewmh_.set_window_state(window, net_wm_state_focused_, true);
    }

    if (auto* client = get_client(window))
        client->user_time = last_event_time_;

    restack_transients(window);

    update_ewmh_client_list();
    conn_.flush();
}

void WindowManager::clear_focus()
{
    active_window_ = XCB_NONE;
    ewmh_.set_active_window(XCB_NONE);
    xcb_set_input_focus(conn_.get(), XCB_INPUT_FOCUS_POINTER_ROOT, conn_.screen()->root, XCB_CURRENT_TIME);

    clear_all_borders();

    conn_.flush();
}

void WindowManager::focus_or_fallback(Monitor& monitor)
{
    auto& ws = monitor.current();

    size_t monitor_idx = 0;
    for (; monitor_idx < monitors_.size(); ++monitor_idx)
    {
        if (&monitors_[monitor_idx] == &monitor)
            break;
    }

    LOG_DEBUG(
        "focus_or_fallback: monitor_idx={} current_ws={} ws.focused_window={:#x} "
        "ws.windows.size={} active_window_={:#x}",
        monitor_idx,
        monitor.current_workspace,
        ws.focused_window,
        ws.windows.size(),
        active_window_
    );

    if (monitor_idx >= monitors_.size())
    {
        LOG_DEBUG("focus_or_fallback: invalid monitor, clearing focus");
        clear_focus();
        return;
    }

    auto eligible = [this](xcb_window_t window) { return !is_client_iconic(window) && is_focus_eligible(window); };

    std::vector<focus_policy::FloatingCandidate> floating_candidates;
    floating_candidates.reserve(floating_windows_.size());
    for (auto const& fw : floating_windows_)
    {
        auto const* c = get_client(fw.id);
        if (!c)
            continue;
        floating_candidates.push_back({ fw.id, c->monitor, c->workspace, c->sticky });
    }

    std::vector<xcb_window_t> sticky_tiled_candidates;
    sticky_tiled_candidates.reserve(ws.windows.size());
    for (size_t w = 0; w < monitor.workspaces.size(); ++w)
    {
        if (w == monitor.current_workspace)
            continue;
        for (xcb_window_t window : monitor.workspaces[w].windows)
        {
            if (is_client_sticky(window))
                sticky_tiled_candidates.push_back(window);
        }
    }

    LOG_TRACE(
        "focus_or_fallback: {} floating candidates, {} sticky tiled candidates",
        floating_candidates.size(),
        sticky_tiled_candidates.size()
    );

    auto selection = focus_policy::select_focus_candidate(
        ws,
        monitor_idx,
        monitor.current_workspace,
        sticky_tiled_candidates,
        floating_candidates,
        eligible
    );

    if (!selection)
    {
        LOG_DEBUG("focus_or_fallback: no candidate found, clearing focus");
        clear_focus();
        return;
    }

    LOG_DEBUG("focus_or_fallback: selected window={:#x} is_floating={}", selection->window, selection->is_floating);

    if (selection->is_floating)
        focus_floating_window(selection->window);
    else
        focus_window(selection->window);

    LOG_TRACE("focus_or_fallback: DONE");
}

bool WindowManager::is_focus_eligible(xcb_window_t window) const
{
    Client::Kind kind = Client::Kind::Tiled;
    if (auto* client = get_client(window))
    {
        kind = client->kind;
        if (kind == Client::Kind::Dock || kind == Client::Kind::Desktop)
            return false;
    }

    bool accepts_input_focus = should_set_input_focus(window);
    bool supports_take_focus = false;
    if (!accepts_input_focus)
    {
        supports_take_focus = supports_protocol(window, wm_take_focus_);
    }
    return focus_policy::is_focus_eligible(kind, accepts_input_focus, supports_take_focus);
}

bool WindowManager::should_set_input_focus(xcb_window_t window) const
{
    xcb_icccm_wm_hints_t hints;
    if (!xcb_icccm_get_wm_hints_reply(conn_.get(), xcb_icccm_get_wm_hints(conn_.get(), window), &hints, nullptr))
        return true;

    if (!(hints.flags & XCB_ICCCM_WM_HINT_INPUT))
        return true;

    if (hints.input)
        return true;

    return false;
}

void WindowManager::send_wm_take_focus(xcb_window_t window, uint32_t timestamp)
{
    if (wm_protocols_ == XCB_NONE || wm_take_focus_ == XCB_NONE)
        return;

    if (!supports_protocol(window, wm_take_focus_))
        return;

    xcb_client_message_event_t ev = {};
    ev.response_type = XCB_CLIENT_MESSAGE;
    ev.window = window;
    ev.type = wm_protocols_;
    ev.format = 32;
    ev.data.data32[0] = wm_take_focus_;
    ev.data.data32[1] = timestamp ? timestamp : XCB_CURRENT_TIME;

    xcb_send_event(conn_.get(), 0, window, XCB_EVENT_MASK_NO_EVENT, reinterpret_cast<char*>(&ev));
}

void WindowManager::focus_next()
{
    if (monitors_.empty())
        return;

    auto& monitor = focused_monitor();
    auto& ws = monitor.current();

    auto eligible = [this](xcb_window_t window) { return !is_client_iconic(window) && is_focus_eligible(window); };

    std::vector<focus_policy::FloatingCandidate> floating_candidates;
    for (auto const& fw : floating_windows_)
    {
        auto const* c = get_client(fw.id);
        if (c)
            floating_candidates.push_back({ fw.id, c->monitor, c->workspace, c->sticky });
    }

    auto candidates = focus_policy::build_cycle_candidates(
        ws.windows,
        floating_candidates,
        focused_monitor_,
        monitor.current_workspace,
        eligible
    );

    auto target = focus_policy::cycle_focus_next(candidates, active_window_);
    if (!target)
        return;

    if (target->is_floating)
        focus_floating_window(target->id);
    else
        focus_window(target->id);
}

void WindowManager::focus_prev()
{
    if (monitors_.empty())
        return;

    auto& monitor = focused_monitor();
    auto& ws = monitor.current();

    auto eligible = [this](xcb_window_t window) { return !is_client_iconic(window) && is_focus_eligible(window); };

    std::vector<focus_policy::FloatingCandidate> floating_candidates;
    for (auto const& fw : floating_windows_)
    {
        auto const* c = get_client(fw.id);
        if (c)
            floating_candidates.push_back({ fw.id, c->monitor, c->workspace, c->sticky });
    }

    auto candidates = focus_policy::build_cycle_candidates(
        ws.windows,
        floating_candidates,
        focused_monitor_,
        monitor.current_workspace,
        eligible
    );

    auto target = focus_policy::cycle_focus_prev(candidates, active_window_);
    if (!target)
        return;

    if (target->is_floating)
        focus_floating_window(target->id);
    else
        focus_window(target->id);
}

} // namespace lwm
