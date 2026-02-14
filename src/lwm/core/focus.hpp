#pragma once

#include "lwm/core/types.hpp"
#include <optional>
#include <span>

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
 * @brief Determine the focus change needed when focusing a tiled window.
 *
 * Pure decision function: does NOT mutate any state. The caller is responsible
 * for applying the returned FocusWindowChange (updating monitor workspace,
 * active_monitor, active_window, focused_window, etc.).
 *
 * @param monitors The list of monitors (read-only).
 * @param active_monitor The currently active monitor index.
 * @param window The window to focus.
 * @param is_sticky If true, do NOT switch workspaces (sticky windows are visible on all workspaces).
 * @return The focus change details, or nullopt if window not found.
 */
std::optional<FocusWindowChange> focus_window_state(
    std::span<Monitor const> monitors,
    size_t active_monitor,
    xcb_window_t window,
    bool is_sticky = false
);

} // namespace lwm::focus
