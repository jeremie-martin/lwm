#include "lwm/core/log.hpp"
#include "lwm/core/policy.hpp"
#include "wm.hpp"

namespace lwm {

bool WindowManager::apply_workspace_switch(size_t monitor_idx, size_t target_workspace)
{
    if (monitor_idx >= monitors_.size())
    {
        LOG_TRACE("workspace switch rejected: invalid-monitor");
        return false;
    }

    auto& monitor = monitors_[monitor_idx];
    auto result = workspace_policy::validate_workspace_switch(monitor, target_workspace);
    if (!result)
    {
        LOG_TRACE("workspace switch rejected");
        return false;
    }

    monitor.previous_workspace = result->old_workspace;
    monitor.current_workspace = result->new_workspace;

    update_ewmh_current_desktop();
    finalize_visibility_on_monitor(monitor_idx);
    emit_event(Event_WorkspaceSwitch,
        "{\"event\":\"workspace_switch\",\"monitor\":" + std::to_string(monitor_idx)
        + ",\"from\":" + std::to_string(result->old_workspace)
        + ",\"to\":" + std::to_string(result->new_workspace) + "}");

    for (auto& [wid, client] : clients_)
    {
        if (!client.urgency.active() || wid == active_window_)
            continue;
        sync_client_urgency_state(client);
    }

    LWM_ASSERT_INVARIANTS(clients_, monitors_);
    return true;
}

void WindowManager::switch_workspace(size_t ws)
{
    if (focused_monitor_ >= monitors_.size())
    {
        LOG_TRACE("switch_workspace({}) called with invalid focused_monitor={}", ws, focused_monitor_);
        return;
    }

    auto const& monitor = focused_monitor();
    LOG_TRACE(
        "switch_workspace({}) called, current={} previous={}",
        ws,
        monitor.current_workspace,
        monitor.previous_workspace
    );

    if (!apply_workspace_switch(focused_monitor_, ws))
    {
        LOG_TRACE("switch_workspace: transition rejected switch");
        return;
    }

    focus_or_fallback(focused_monitor());
    flush_and_drain_crossing();

    LOG_TRACE(
        "switch_workspace: DONE, now current={} previous={}",
        focused_monitor().current_workspace,
        focused_monitor().previous_workspace
    );
}

void WindowManager::toggle_workspace()
{
    auto& monitor = focused_monitor();
    size_t workspace_count = monitor.workspaces.size();
    LOG_TRACE(
        "toggle_workspace() called, workspace_count={} current={} previous={}",
        workspace_count,
        monitor.current_workspace,
        monitor.previous_workspace
    );

    if (workspace_count <= 1)
    {
        LOG_TRACE("toggle_workspace: only 1 workspace, returning");
        return;
    }

    size_t target = monitor.previous_workspace;
    if (target >= workspace_count || target == monitor.current_workspace)
    {
        LOG_TRACE("toggle_workspace: invalid target={}, returning", target);
        return;
    }

    LOG_TRACE("toggle_workspace: switching to workspace {}", target);
    switch_workspace(target);
    LOG_TRACE("toggle_workspace: DONE");
}

void WindowManager::move_window_to_workspace(size_t ws)
{
    auto& monitor = focused_monitor();
    size_t workspace_count = monitor.workspaces.size();
    if (workspace_count == 0)
        return;

    if (ws >= workspace_count || ws == monitor.current_workspace)
        return;

    if (active_window_ == XCB_NONE)
        return;

    xcb_window_t window_to_move = active_window_;
    size_t target_ws = ws;
    auto* client = get_client(window_to_move);
    if (!client)
        return;

    if (client->kind == Client::Kind::Floating)
    {
        size_t monitor_idx = client->monitor;
        if (!move_floating_client_to_workspace(*client, monitor_idx, target_ws, false))
            return;
        focus_or_fallback(monitors_[monitor_idx]);
        flush_and_drain_crossing();
        return;
    }

    if (!move_tiled_client_to_workspace(*client, focused_monitor_, target_ws))
        return;

    workspace_policy::set_workspace_focus(monitor.workspaces[target_ws], window_to_move);
    focus_or_fallback(monitor);

    flush_and_drain_crossing();
}

size_t WindowManager::wrap_monitor_index(int idx) const
{
    int size = static_cast<int>(monitors_.size());
    return static_cast<size_t>(((idx % size) + size) % size);
}

void WindowManager::warp_to_monitor(Monitor const& monitor)
{
    xcb_warp_pointer(
        conn_.get(),
        XCB_NONE,
        conn_.screen()->root,
        0,
        0,
        0,
        0,
        monitor.x + monitor.width / 2,
        monitor.y + monitor.height / 2
    );
}

void WindowManager::focus_monitor(int direction)
{
    if (monitors_.size() <= 1)
        return;

    focused_monitor_ = wrap_monitor_index(static_cast<int>(focused_monitor_) + direction);
    update_ewmh_current_desktop();

    auto& monitor = focused_monitor();
    focus_or_fallback(monitor);
    if (config_.focus.warp_cursor_on_monitor_change)
    {
        warp_to_monitor(monitor);
    }

    conn_.flush();
}

void WindowManager::move_window_to_monitor(int direction)
{
    if (monitors_.size() <= 1)
        return;

    if (active_window_ == XCB_NONE)
        return;

    xcb_window_t window_to_move = active_window_;
    auto* client = get_client(window_to_move);
    if (!client)
        return;

    if (client->kind == Client::Kind::Floating)
    {
        size_t source_idx = client->monitor;
        size_t target_idx = wrap_monitor_index(static_cast<int>(source_idx) + direction);
        if (target_idx == source_idx)
            return;

        size_t target_workspace = monitors_[target_idx].current_workspace;
        if (!move_floating_client_to_workspace(*client, target_idx, target_workspace, true))
            return;

        focused_monitor_ = target_idx;
        update_ewmh_current_desktop();
        if (is_suppressed_by_fullscreen(*client))
            focus_or_fallback(monitors_[target_idx]);
        else
            focus_any_window(window_to_move);
        if (config_.focus.warp_cursor_on_monitor_change)
        {
            warp_to_monitor(monitors_[target_idx]);
        }

        flush_and_drain_crossing();
        return;
    }

    size_t target_idx = wrap_monitor_index(static_cast<int>(focused_monitor_) + direction);
    if (target_idx == focused_monitor_)
        return;

    auto& target_monitor = monitors_[target_idx];
    if (!move_tiled_client_to_workspace(*client, target_idx, target_monitor.current_workspace))
        return;
    workspace_policy::set_workspace_focus(target_monitor.current(), window_to_move);

    focused_monitor_ = target_idx;
    update_ewmh_current_desktop();
    if (is_suppressed_by_fullscreen(*client))
        focus_or_fallback(target_monitor);
    else
        focus_any_window(window_to_move);
    if (config_.focus.warp_cursor_on_monitor_change)
    {
        warp_to_monitor(target_monitor);
    }

    flush_and_drain_crossing();
}

}
