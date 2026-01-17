#include "lwm/core/floating.hpp"
#include <algorithm>

namespace lwm::floating {

Geometry place_floating(Geometry area, uint16_t width, uint16_t height, std::optional<Geometry> parent)
{
    int32_t target_x = 0;
    int32_t target_y = 0;

    if (parent)
    {
        target_x =
            static_cast<int32_t>(parent->x) + (static_cast<int32_t>(parent->width) - static_cast<int32_t>(width)) / 2;
        target_y =
            static_cast<int32_t>(parent->y) + (static_cast<int32_t>(parent->height) - static_cast<int32_t>(height)) / 2;
    }
    else
    {
        target_x = static_cast<int32_t>(area.x) + (static_cast<int32_t>(area.width) - static_cast<int32_t>(width)) / 2;
        target_y =
            static_cast<int32_t>(area.y) + (static_cast<int32_t>(area.height) - static_cast<int32_t>(height)) / 2;
    }

    int32_t min_x = area.x;
    int32_t max_x = static_cast<int32_t>(area.x) + static_cast<int32_t>(area.width) - static_cast<int32_t>(width);
    if (max_x < min_x)
        max_x = min_x;

    int32_t min_y = area.y;
    int32_t max_y = static_cast<int32_t>(area.y) + static_cast<int32_t>(area.height) - static_cast<int32_t>(height);
    if (max_y < min_y)
        max_y = min_y;

    target_x = std::clamp(target_x, min_x, max_x);
    target_y = std::clamp(target_y, min_y, max_y);

    Geometry result;
    result.x = static_cast<int16_t>(target_x);
    result.y = static_cast<int16_t>(target_y);
    result.width = width;
    result.height = height;
    return result;
}

} // namespace lwm::floating
