#pragma once

#include "lwm/core/types.hpp"
#include <optional>
#include <span>
#include <vector>

namespace lwm::focus {

enum class PointerTransition
{
    None,
    MonitorChangedClearFocus,
};

struct PointerFocusResult
{
    PointerTransition transition = PointerTransition::None;
    size_t new_monitor = 0;

    bool monitor_changed() const { return transition != PointerTransition::None; }
    bool clears_focus() const { return transition == PointerTransition::MonitorChangedClearFocus; }
};

std::optional<size_t> monitor_index_at_point(std::span<Monitor const> monitors, int16_t x, int16_t y);

PointerFocusResult pointer_move(std::span<Monitor const> monitors, size_t active_monitor, int16_t x, int16_t y);

struct FocusWindowChange
{
    size_t target_monitor = 0;
    size_t old_workspace = 0;
    size_t new_workspace = 0;
    bool workspace_changed = false;
};

/**
 * @brief Update internal state when focusing a tiled window.
 *
 * @param monitors The list of monitors.
 * @param active_monitor The currently active monitor index (updated).
 * @param active_window The currently active window (updated).
 * @param window The window to focus.
 * @param is_sticky If true, do NOT switch workspaces (sticky windows are visible on all workspaces).
 * @return The focus change details, or nullopt if window not found.
 */
std::optional<FocusWindowChange> focus_window_state(
    std::vector<Monitor>& monitors,
    size_t& active_monitor,
    xcb_window_t& active_window,
    xcb_window_t window,
    bool is_sticky = false
);

} // namespace lwm::focus
