#include "lwm/core/floating.hpp"
#include <algorithm>

namespace lwm::floating {

namespace {

Geometry clamp_geometry(Geometry area, Geometry geometry)
{
    int32_t target_x = geometry.x;
    int32_t target_y = geometry.y;

    int32_t min_x = area.x;
    int32_t max_x = static_cast<int32_t>(area.x) + static_cast<int32_t>(area.width)
        - static_cast<int32_t>(geometry.width);
    if (max_x < min_x)
        max_x = min_x;

    int32_t min_y = area.y;
    int32_t max_y = static_cast<int32_t>(area.y) + static_cast<int32_t>(area.height)
        - static_cast<int32_t>(geometry.height);
    if (max_y < min_y)
        max_y = min_y;

    target_x = std::clamp(target_x, min_x, max_x);
    target_y = std::clamp(target_y, min_y, max_y);

    geometry.x = static_cast<int16_t>(target_x);
    geometry.y = static_cast<int16_t>(target_y);
    return geometry;
}

} // namespace

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

Geometry& runtime_hints_geometry(Client& client)
{
    if (client.fullscreen && client.fullscreen_restore)
        return *client.fullscreen_restore;
    if ((client.maximized_horz || client.maximized_vert) && client.maximize_restore)
        return *client.maximize_restore;
    return floating_geometry(client);
}

PositionHintResolution resolve_position_hint(
    std::vector<Monitor> const& monitors,
    size_t assigned_monitor,
    bool constrained_to_assigned_monitor,
    Geometry hinted_geometry)
{
    size_t fallback_monitor = assigned_monitor < monitors.size() ? assigned_monitor : 0;
    if (monitors.empty())
        return { false, fallback_monitor };

    if (constrained_to_assigned_monitor)
    {
        return {
            hint_targets_monitor(
                monitors[fallback_monitor].geometry(),
                hinted_geometry.x,
                hinted_geometry.y,
                hinted_geometry.width,
                hinted_geometry.height
            ),
            fallback_monitor,
        };
    }

    for (size_t monitor = 0; monitor < monitors.size(); ++monitor)
    {
        if (hint_targets_monitor(
                monitors[monitor].geometry(),
                hinted_geometry.x,
                hinted_geometry.y,
                hinted_geometry.width,
                hinted_geometry.height
            ))
        {
            return { true, monitor };
        }
    }

    return { false, fallback_monitor };
}

bool hint_targets_monitor(Geometry monitor, int16_t x, int16_t y, uint16_t width, uint16_t height)
{
    int32_t center_x = static_cast<int32_t>(x) + static_cast<int32_t>(width) / 2;
    int32_t center_y = static_cast<int32_t>(y) + static_cast<int32_t>(height) / 2;

    int32_t left = monitor.x;
    int32_t right = static_cast<int32_t>(monitor.x) + static_cast<int32_t>(monitor.width);
    int32_t top = monitor.y;
    int32_t bottom = static_cast<int32_t>(monitor.y) + static_cast<int32_t>(monitor.height);

    return center_x >= left && center_x < right && center_y >= top && center_y < bottom;
}

Geometry clamp_to_area(Geometry area, Geometry geometry)
{
    return clamp_geometry(area, geometry);
}

Geometry translate_to_area(Geometry geometry, Geometry source_area, Geometry target_area)
{
    Geometry translated = geometry;
    translated.x = static_cast<int16_t>(
        static_cast<int32_t>(target_area.x) + (static_cast<int32_t>(geometry.x) - static_cast<int32_t>(source_area.x))
    );
    translated.y = static_cast<int16_t>(
        static_cast<int32_t>(target_area.y) + (static_cast<int32_t>(geometry.y) - static_cast<int32_t>(source_area.y))
    );
    return clamp_geometry(target_area, translated);
}

} // namespace lwm::floating
