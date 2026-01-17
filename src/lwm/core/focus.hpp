#pragma once

#include "lwm/core/types.hpp"
#include <optional>
#include <span>
#include <vector>

namespace lwm::focus {

struct PointerFocusResult
{
    bool active_monitor_changed = false;
    size_t new_monitor = 0;
    bool clear_focus = false;
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

std::optional<FocusWindowChange> focus_window_state(
    std::vector<Monitor>& monitors,
    size_t& active_monitor,
    xcb_window_t& active_window,
    xcb_window_t window
);

} // namespace lwm::focus
