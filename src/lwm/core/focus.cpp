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

    result.transition = PointerTransition::MonitorChangedClearFocus;
    result.new_monitor = *new_monitor;
    LOG_TRACE("pointer_move: monitor changed from {} to {}", active_monitor, *new_monitor);
    return result;
}

} // namespace lwm::focus
