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

    // Apply the workspace mutation here — callers no longer pre-mutate.
    monitor.previous_workspace = ctx.old_workspace;
    monitor.current_workspace = ctx.new_workspace;

    // Hide floating windows from old workspace FIRST
    // This prevents visual glitches where old floating windows appear over new workspace content
    for (xcb_window_t fw : floating_windows_)
    {
        auto const* client = get_client(fw);
        if (!client || client->monitor != ctx.monitor_idx)
            continue;
        if (is_client_sticky(fw))
            continue;
        if (client->workspace == ctx.old_workspace)
        {
            LOG_TRACE("perform_workspace_switch: hiding floating {:#x}", fw);
            hide_window(fw);
        }
    }

    // Hide tiled windows from old workspace
    for (xcb_window_t window : monitor.workspaces[ctx.old_workspace].windows)
    {
        if (is_client_sticky(window))
            continue;
        LOG_TRACE("perform_workspace_switch: hiding tiled {:#x}", window);
        hide_window(window);
    }

    // Flush before rearranging to ensure old windows are hidden
    conn_.flush();

    update_ewmh_current_desktop();
    rearrange_monitor(monitor);
    update_floating_visibility(ctx.monitor_idx);

    LOG_TRACE("perform_workspace_switch: DONE");
}

void WindowManager::switch_workspace(int ws)
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
    conn_.flush();

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
    switch_workspace(static_cast<int>(target));
    LOG_TRACE("toggle_workspace: DONE");
}

void WindowManager::move_window_to_workspace(int ws)
{
    auto& monitor = focused_monitor();
    size_t workspace_count = monitor.workspaces.size();
    if (workspace_count == 0)
        return;

    if (ws < 0 || static_cast<size_t>(ws) >= workspace_count || static_cast<size_t>(ws) == monitor.current_workspace)
        return;

    if (active_window_ == XCB_NONE)
        return;

    xcb_window_t window_to_move = active_window_;
    size_t target_ws = static_cast<size_t>(ws);

    if (is_floating_window(window_to_move))
    {
        auto* client = get_client(window_to_move);
        if (!client)
            return;

        size_t monitor_idx = client->monitor;

        client->workspace = target_ws;

        uint32_t desktop = get_ewmh_desktop_index(monitor_idx, target_ws);
        if (is_client_sticky(window_to_move))
            ewmh_.set_window_desktop(window_to_move, 0xFFFFFFFF);
        else
            ewmh_.set_window_desktop(window_to_move, desktop);

        update_floating_visibility(monitor_idx);
        focus_or_fallback(monitors_[monitor_idx]);
        conn_.flush();
        return;
    }

    if (!workspace_policy::move_tiled_window(
            monitor,
            window_to_move,
            target_ws,
            [this](xcb_window_t window) { return is_client_iconic(window); }
        ))
    {
        return;
    }

    if (auto* client = get_client(window_to_move))
        client->workspace = target_ws;

    uint32_t desktop = get_ewmh_desktop_index(focused_monitor_, target_ws);
    if (is_client_sticky(window_to_move))
        ewmh_.set_window_desktop(window_to_move, 0xFFFFFFFF);
    else
        ewmh_.set_window_desktop(window_to_move, desktop);

    if (!is_client_sticky(window_to_move))
    {
        hide_window(window_to_move);
    }
    rearrange_monitor(monitor);
    focus_or_fallback(monitor);

    conn_.flush();
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

    if (is_floating_window(window_to_move))
    {
        auto* client = get_client(window_to_move);
        if (!client)
            return;

        size_t source_idx = client->monitor;
        size_t target_idx = wrap_monitor_index(static_cast<int>(source_idx) + direction);
        if (target_idx == source_idx)
            return;

        size_t target_workspace = monitors_[target_idx].current_workspace;
        client->floating_geometry = floating::place_floating(
            monitors_[target_idx].working_area(),
            client->floating_geometry.width,
            client->floating_geometry.height,
            std::nullopt
        );

        client->monitor = target_idx;
        client->workspace = target_workspace;

        uint32_t desktop = get_ewmh_desktop_index(target_idx, target_workspace);
        if (is_client_sticky(window_to_move))
            ewmh_.set_window_desktop(window_to_move, 0xFFFFFFFF);
        else
            ewmh_.set_window_desktop(window_to_move, desktop);

        update_floating_visibility(source_idx);
        update_floating_visibility(target_idx);

        focused_monitor_ = target_idx;
        update_ewmh_current_desktop();
        focus_any_window(window_to_move);
        if (config_.focus.warp_cursor_on_monitor_change)
        {
            warp_to_monitor(monitors_[target_idx]);
        }

        conn_.flush();
        return;
    }

    auto& source_ws = focused_monitor().current();
    size_t target_idx = wrap_monitor_index(static_cast<int>(focused_monitor_) + direction);
    if (target_idx == focused_monitor_)
        return;

    auto is_iconic = [this](xcb_window_t w) { return is_client_iconic(w); };
    if (!workspace_policy::remove_tiled_window(source_ws, window_to_move, is_iconic))
        return;

    auto& target_monitor = monitors_[target_idx];
    target_monitor.current().windows.push_back(window_to_move);
    target_monitor.current().focused_window = window_to_move;

    if (auto* client = get_client(window_to_move))
    {
        client->monitor = target_idx;
        client->workspace = target_monitor.current_workspace;
    }

    uint32_t desktop = get_ewmh_desktop_index(target_idx, target_monitor.current_workspace);
    if (is_client_sticky(window_to_move))
        ewmh_.set_window_desktop(window_to_move, 0xFFFFFFFF);
    else
        ewmh_.set_window_desktop(window_to_move, desktop);

    rearrange_monitor(focused_monitor());
    rearrange_monitor(target_monitor);

    focused_monitor_ = target_idx;
    update_ewmh_current_desktop();
    focus_any_window(window_to_move);
    if (config_.focus.warp_cursor_on_monitor_change)
    {
        warp_to_monitor(target_monitor);
    }

    conn_.flush();
}

}
