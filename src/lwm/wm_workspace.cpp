#include "wm.hpp"
#include "lwm/core/floating.hpp"
#include "lwm/core/policy.hpp"

namespace lwm {

void WindowManager::switch_workspace(int ws)
{
    auto& monitor = focused_monitor();
    auto switch_result = workspace_policy::apply_workspace_switch(monitor, ws);
    if (!switch_result)
        return;

    auto& old_workspace = monitor.workspaces[switch_result->old_workspace];
    for (xcb_window_t window : old_workspace.windows)
    {
        if (is_client_sticky(window))
            continue;
        wm_unmap_window(window);
    }

    update_ewmh_current_desktop();
    rearrange_monitor(monitor);
    update_floating_visibility(focused_monitor_);
    focus_or_fallback(monitor);
    update_all_bars();
    conn_.flush();
}

void WindowManager::toggle_workspace()
{
    auto& monitor = focused_monitor();
    size_t workspace_count = monitor.workspaces.size();
    if (workspace_count <= 1)
        return;

    size_t target = monitor.previous_workspace;
    if (target >= workspace_count || target == monitor.current_workspace)
        return;

    constexpr auto debounce = std::chrono::milliseconds(150);
    auto now = std::chrono::steady_clock::now();
    // Temporary debounce to avoid rapid re-entry from event storms; remove if root cause is fixed.
    if (now - last_toggle_workspace_time_ < debounce)
        return;
    last_toggle_workspace_time_ = now;

    switch_workspace(static_cast<int>(target));
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
