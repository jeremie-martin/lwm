#include "lwm/core/log.hpp"
#include "lwm/core/policy.hpp"
#include "wm.hpp"

namespace lwm {

void WindowManager::switch_workspace(size_t ws)
{
    if (focused_monitor_ >= monitors_.size())
    {
        LWM_LOG_TRACE("switch_workspace({}): invalid focused_monitor={}", ws, focused_monitor_);
        return;
    }

    if (!state_.switch_workspace(focused_monitor_, ws))
    {
        LWM_LOG_TRACE("switch_workspace: transition rejected switch");
        return;
    }

    focus_or_fallback(focused_monitor());

    LWM_LOG_TRACE(
        "Workspace switched: current={} previous={}",
        focused_monitor().current_workspace,
        focused_monitor().previous_workspace
    );
}

void WindowManager::toggle_workspace()
{
    auto& monitor = focused_monitor();
    size_t workspace_count = monitor.workspaces.size();
    if (workspace_count <= 1)
    {
        LWM_LOG_TRACE("toggle_workspace: only 1 workspace, returning");
        return;
    }

    size_t target = monitor.previous_workspace;
    if (target >= workspace_count || target == monitor.current_workspace)
    {
        LWM_LOG_TRACE("toggle_workspace: invalid target={}, returning", target);
        return;
    }

    LWM_LOG_TRACE("toggle_workspace: switching to workspace {}", target);
    switch_workspace(target);
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

    auto* client = get_client(active_window_);
    if (!client)
        return;

    auto source_monitor = client->monitor;
    if (!state_.relocate(client->id, source_monitor, ws))
        return;
    if (client->kind() == Client::Kind::Tiled)
        state_.remember_focus(source_monitor, ws, client->id);
    focus_or_fallback(monitors_[source_monitor]);
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

    state_.focus_monitor(wrap_monitor_index(static_cast<int>(focused_monitor_) + direction));
    request_current_desktop_update();

    auto& monitor = focused_monitor();
    focus_or_fallback(monitor);
    if (config_.focus.warp_cursor_on_monitor_change)
    {
        warp_to_monitor(monitor);
    }
}

void WindowManager::move_window_to_monitor(int direction)
{
    if (monitors_.size() <= 1)
        return;

    if (active_window_ == XCB_NONE)
        return;

    auto* client = get_client(active_window_);
    if (!client)
        return;

    size_t target_idx = wrap_monitor_index(static_cast<int>(client->monitor) + direction);
    if (target_idx == client->monitor)
        return;

    auto& target_monitor = monitors_[target_idx];
    if (!state_.relocate(
            client->id,
            target_idx,
            target_monitor.current_workspace,
            RelocationGeometry::CenterOnMonitorChange
        ))
        return;
    if (client->kind() == Client::Kind::Tiled)
        state_.remember_focus(target_idx, target_monitor.current_workspace, client->id);

    state_.focus_monitor(target_idx);
    request_current_desktop_update();
    if (is_suppressed_by_fullscreen(*client))
        focus_or_fallback(target_monitor);
    else
        focus_any_window(client->id);
    if (config_.focus.warp_cursor_on_monitor_change)
        warp_to_monitor(target_monitor);
}

}
