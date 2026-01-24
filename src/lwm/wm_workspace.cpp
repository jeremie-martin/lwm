#include "wm.hpp"
#include "lwm/core/floating.hpp"
#include "lwm/core/log.hpp"
#include "lwm/core/policy.hpp"

namespace lwm {

void WindowManager::switch_workspace(int ws)
{
    auto& monitor = focused_monitor();
    LOG_TRACE("switch_workspace({}) called, current={} previous={}",
              ws, monitor.current_workspace, monitor.previous_workspace);

    auto switch_result = workspace_policy::apply_workspace_switch(monitor, ws);
    if (!switch_result)
    {
        LOG_TRACE("switch_workspace: policy rejected switch");
        return;
    }

    LOG_DEBUG("switch_workspace: policy approved, old_ws={} new_ws={}",
              switch_result->old_workspace, switch_result->new_workspace);

    // Unmap floating windows from old workspace FIRST
    // This prevents visual glitches where old floating windows appear over new workspace content
    for (auto& fw : floating_windows_)
    {
        auto const* client = get_client(fw.id);
        if (!client || client->monitor != focused_monitor_)
            continue;
        if (is_client_sticky(fw.id))
            continue;
        if (client->workspace == switch_result->old_workspace)
        {
            LOG_DEBUG("switch_workspace: pre-unmapping floating {:#x}", fw.id);
            wm_unmap_window(fw.id);
        }
    }

    // Now unmap tiled windows from old workspace
    auto& old_workspace = monitor.workspaces[switch_result->old_workspace];
    LOG_DEBUG("switch_workspace: old_workspace has {} tiled windows", old_workspace.windows.size());
    for (xcb_window_t window : old_workspace.windows)
    {
        LOG_DEBUG("switch_workspace: considering unmapping tiled {:#x}", window);
        if (is_client_sticky(window))
        {
            LOG_DEBUG("switch_workspace: {:#x} is sticky, skipping", window);
            continue;
        }
        LOG_DEBUG("switch_workspace: unmapping tiled {:#x}", window);
        wm_unmap_window(window);
    }
    LOG_DEBUG("switch_workspace: done unmapping old workspace windows");
    // Flush unmaps before rearranging to ensure old windows are hidden
    conn_.flush();

    LOG_TRACE("switch_workspace: unmapped old windows, now updating EWMH");
    update_ewmh_current_desktop();
    LOG_TRACE("switch_workspace: rearranging monitor");
    rearrange_monitor(monitor);
    LOG_TRACE("switch_workspace: updating floating visibility");
    update_floating_visibility(focused_monitor_);
    LOG_TRACE("switch_workspace: focus_or_fallback");
    focus_or_fallback(monitor);
    LOG_TRACE("switch_workspace: update_all_bars");
    update_all_bars();
    LOG_TRACE("switch_workspace: flushing");
    conn_.flush();
    LOG_TRACE("switch_workspace: DONE, now current={} previous={}",
              monitor.current_workspace, monitor.previous_workspace);
}

void WindowManager::toggle_workspace()
{
    auto& monitor = focused_monitor();
    size_t workspace_count = monitor.workspaces.size();
    LOG_TRACE("toggle_workspace() called, workspace_count={} current={} previous={}",
              workspace_count, monitor.current_workspace, monitor.previous_workspace);

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

    if (find_floating_window(window_to_move))
    {
        auto* client = get_client(window_to_move);
        if (!client)
            return;

        size_t monitor_idx = client->monitor;

        // Update Client workspace (authoritative)
        client->workspace = target_ws;

        uint32_t desktop = get_ewmh_desktop_index(monitor_idx, target_ws);
        if (is_client_sticky(window_to_move))
            ewmh_.set_window_desktop(window_to_move, 0xFFFFFFFF);
        else
            ewmh_.set_window_desktop(window_to_move, desktop);

        update_floating_visibility(monitor_idx);
        focus_or_fallback(monitors_[monitor_idx]);
        update_all_bars();
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

    // Update Client workspace for O(1) lookup
    if (auto* client = get_client(window_to_move))
        client->workspace = target_ws;

    // Update EWMH desktop for moved window
    uint32_t desktop = get_ewmh_desktop_index(focused_monitor_, target_ws);
    if (is_client_sticky(window_to_move))
        ewmh_.set_window_desktop(window_to_move, 0xFFFFFFFF);
    else
        ewmh_.set_window_desktop(window_to_move, desktop);

    if (!is_client_sticky(window_to_move))
    {
        wm_unmap_window(window_to_move);
    }
    rearrange_monitor(monitor);
    focus_or_fallback(monitor);

    update_all_bars();
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

    update_all_bars();
    conn_.flush();
}

void WindowManager::move_window_to_monitor(int direction)
{
    if (monitors_.size() <= 1)
        return;

    if (active_window_ == XCB_NONE)
        return;

    xcb_window_t window_to_move = active_window_;

    if (auto* floating_window = find_floating_window(window_to_move))
    {
        auto* client = get_client(window_to_move);
        if (!client)
            return;

        size_t source_idx = client->monitor;
        size_t target_idx = wrap_monitor_index(static_cast<int>(source_idx) + direction);
        if (target_idx == source_idx)
            return;

        size_t target_workspace = monitors_[target_idx].current_workspace;
        floating_window->geometry = floating::place_floating(
            monitors_[target_idx].working_area(),
            floating_window->geometry.width,
            floating_window->geometry.height,
            std::nullopt
        );

        // Update Client (authoritative for monitor, workspace, geometry)
        client->monitor = target_idx;
        client->workspace = target_workspace;
        client->floating_geometry = floating_window->geometry;

        uint32_t desktop = get_ewmh_desktop_index(target_idx, target_workspace);
        if (is_client_sticky(window_to_move))
            ewmh_.set_window_desktop(window_to_move, 0xFFFFFFFF);
        else
            ewmh_.set_window_desktop(window_to_move, desktop);

        update_floating_visibility(source_idx);
        update_floating_visibility(target_idx);

        focused_monitor_ = target_idx;
        update_ewmh_current_desktop();
        focus_floating_window(window_to_move);
        if (config_.focus.warp_cursor_on_monitor_change)
        {
            warp_to_monitor(monitors_[target_idx]);
        }

        update_all_bars();
        conn_.flush();
        return;
    }

    auto& source_ws = focused_monitor().current();
    size_t target_idx = wrap_monitor_index(static_cast<int>(focused_monitor_) + direction);
    if (target_idx == focused_monitor_)
        return;

    auto it = source_ws.find_window(window_to_move);
    if (it == source_ws.windows.end())
        return;

    xcb_window_t moved_window = *it;
    source_ws.windows.erase(it);

    // Update source workspace's focused_window to another window if any remain
    if (!source_ws.windows.empty())
    {
        source_ws.focused_window = source_ws.windows.back();
    }
    else
    {
        source_ws.focused_window = XCB_NONE;
    }

    auto& target_monitor = monitors_[target_idx];
    target_monitor.current().windows.push_back(moved_window);
    target_monitor.current().focused_window = window_to_move;

    // Update Client for O(1) lookup
    if (auto* client = get_client(window_to_move))
    {
        client->monitor = target_idx;
        client->workspace = target_monitor.current_workspace;
    }

    // Update EWMH desktop for moved window
    uint32_t desktop = get_ewmh_desktop_index(target_idx, target_monitor.current_workspace);
    if (is_client_sticky(window_to_move))
        ewmh_.set_window_desktop(window_to_move, 0xFFFFFFFF);
    else
        ewmh_.set_window_desktop(window_to_move, desktop);

    rearrange_monitor(focused_monitor());
    rearrange_monitor(target_monitor);

    focused_monitor_ = target_idx;
    update_ewmh_current_desktop();
    focus_window(window_to_move);
    if (config_.focus.warp_cursor_on_monitor_change)
    {
        warp_to_monitor(target_monitor);
    }

    update_all_bars();
    conn_.flush();
}

} // namespace lwm
