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

} // namespace lwm::focus
