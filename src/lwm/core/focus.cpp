#include "lwm/core/focus.hpp"
#include "lwm/core/log.hpp"

namespace lwm::focus {

std::optional<size_t> monitor_index_at_point(std::span<Monitor const> monitors, int16_t x, int16_t y)
{
    for (size_t i = 0; i < monitors.size(); ++i)
    {
        auto const& monitor = monitors[i];
        if (x >= monitor.x && x < monitor.x + monitor.width && y >= monitor.y && y < monitor.y + monitor.height)
        {
            return i;
        }
    }
    return std::nullopt;
}

PointerFocusResult pointer_move(std::span<Monitor const> monitors, size_t active_monitor, int16_t x, int16_t y)
{
    PointerFocusResult result;
    result.new_monitor = active_monitor;

    auto new_monitor = monitor_index_at_point(monitors, x, y);
    if (!new_monitor)
        return result;

    if (*new_monitor == active_monitor)
        return result;

    result.active_monitor_changed = true;
    result.new_monitor = *new_monitor;
    result.clear_focus = true;
    LOG_TRACE("pointer_move: monitor changed from {} to {}", active_monitor, *new_monitor);
    return result;
}

std::optional<FocusWindowChange> focus_window_state(
    std::vector<Monitor>& monitors,
    size_t& active_monitor,
    xcb_window_t& active_window,
    xcb_window_t window,
    bool is_sticky
)
{
    LOG_TRACE(
        "focus_window_state: window={:#x} active_monitor={} active_window={:#x} is_sticky={}",
        window,
        active_monitor,
        active_window,
        is_sticky
    );

    for (size_t m = 0; m < monitors.size(); ++m)
    {
        for (size_t w = 0; w < monitors[m].workspaces.size(); ++w)
        {
            if (monitors[m].workspaces[w].find_window(window) != monitors[m].workspaces[w].windows.end())
            {
                auto& target_monitor = monitors[m];
                size_t old_workspace = target_monitor.current_workspace;
                size_t new_workspace = w;

                LOG_TRACE(
                    "focus_window_state: found window on monitor {} workspace {} (current={})",
                    m,
                    w,
                    old_workspace
                );

                bool actually_changed = false;
                if (!is_sticky && old_workspace != new_workspace)
                {
                    LOG_DEBUG(
                        "focus_window_state: WORKSPACE WILL CHANGE from {} to {} on monitor {}",
                        old_workspace,
                        new_workspace,
                        m
                    );
                    target_monitor.previous_workspace = old_workspace;
                    target_monitor.current_workspace = new_workspace;
                    actually_changed = true;
                }

                target_monitor.current().focused_window = window;
                active_monitor = m;
                active_window = window;

                FocusWindowChange change;
                change.target_monitor = m;
                change.old_workspace = old_workspace;
                change.new_workspace = is_sticky ? old_workspace : new_workspace;
                change.workspace_changed = actually_changed;

                LOG_TRACE(
                    "focus_window_state: returning change target_monitor={} old_ws={} new_ws={} changed={}",
                    change.target_monitor,
                    change.old_workspace,
                    change.new_workspace,
                    change.workspace_changed
                );
                return change;
            }
        }
    }
    LOG_TRACE("focus_window_state: window {:#x} not found in any workspace", window);
    return std::nullopt;
}

} // namespace lwm::focus
