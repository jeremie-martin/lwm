#include "lwm/core/floating.hpp"
#include "lwm/core/log.hpp"
#include "lwm/core/policy.hpp"
#include "wm.hpp"

namespace lwm {

void WindowManager::perform_workspace_switch(WorkspaceSwitchContext const& ctx)
{
    auto& monitor = monitors_[ctx.monitor_idx];
    LOG_DEBUG(
        "perform_workspace_switch: monitor={} old_ws={} new_ws={}",
        ctx.monitor_idx,
        ctx.old_workspace,
        ctx.new_workspace
    );

    // 1. Mutate workspace state. Must happen first; subsequent steps read it.
    monitor.previous_workspace = ctx.old_workspace;
    monitor.current_workspace = ctx.new_workspace;

    // 2. EWMH _NET_CURRENT_DESKTOP — reads monitor.current_workspace set above.
    update_ewmh_current_desktop();

    // 3. Recompute fullscreen ownership, sync per-window visibility against the
    //    new workspace, then re-tile. Order is load-bearing: see CONTRIBUTING.md
    //    "Design Principles" — sync_visibility_for_monitor maintains the
    //    authoritative `hidden` flag that rearrange_monitor reads.
    finalize_visibility_on_monitor(ctx.monitor_idx);

    emit_event(Event_WorkspaceSwitch,
        "{\"event\":\"workspace_switch\",\"monitor\":" + std::to_string(ctx.monitor_idx)
        + ",\"from\":" + std::to_string(ctx.old_workspace)
        + ",\"to\":" + std::to_string(ctx.new_workspace) + "}");

    // Re-sync WM_HINTS urgency for windows that still have active urgency.
    // Apps may have cleared their own WM_HINTS urgency on focus (standard ICCCM
    // behavior), but the WM considers them still urgent (set via notify-attention).
    // Re-assert so panels checking WM_HINTS (e.g. polybar) see the urgency.
    for (auto& [wid, client] : clients_)
    {
        if (!client.urgency.active() || wid == active_window_)
            continue;
        sync_client_urgency_state(client);
    }

    LWM_ASSERT_INVARIANTS(clients_, monitors_);
    LOG_TRACE("perform_workspace_switch: DONE");
}

void WindowManager::switch_workspace(size_t ws)
{
    auto& monitor = focused_monitor();
    LOG_TRACE(
        "switch_workspace({}) called, current={} previous={}",
        ws,
        monitor.current_workspace,
        monitor.previous_workspace
    );

    auto switch_result = workspace_policy::validate_workspace_switch(monitor, ws);
    if (!switch_result)
    {
        LOG_TRACE("switch_workspace: policy rejected switch");
        return;
    }

    LOG_DEBUG(
        "switch_workspace: policy approved, old_ws={} new_ws={}",
        switch_result->old_workspace,
        switch_result->new_workspace
    );

    perform_workspace_switch({ focused_monitor_, switch_result->old_workspace, switch_result->new_workspace });
    focus_or_fallback(monitor);
    flush_and_drain_crossing();

    LOG_TRACE(
        "switch_workspace: DONE, now current={} previous={}",
        monitor.current_workspace,
        monitor.previous_workspace
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
        if (client->monitor >= monitors_.size())
            return;

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
        if (client->monitor >= monitors_.size())
            return;

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
