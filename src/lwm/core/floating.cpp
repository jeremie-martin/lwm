#include "lwm/core/floating.hpp"
#include <algorithm>

namespace lwm::floating {

namespace {

Geometry clamp_geometry(Geometry area, Geometry geometry, int32_t target_x, int32_t target_y)
{

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

    geometry.x = geometry_coordinate(target_x);
    geometry.y = geometry_coordinate(target_y);
    return geometry;
}

} // namespace

Geometry drag_geometry(Geometry start, int32_t dx, int32_t dy, ResizeEdge edges)
{
    if (edges == ResizeEdge::None)
        return { geometry_coordinate(static_cast<int64_t>(start.x) + dx),
                 geometry_coordinate(static_cast<int64_t>(start.y) + dy),
                 start.width,
                 start.height };
    auto has = [edges](ResizeEdge edge) { return (static_cast<uint8_t>(edges) & static_cast<uint8_t>(edge)) != 0; };
    auto resize_axis = [](int16_t& origin, uint16_t& extent, int32_t delta, bool leading, bool trailing)
    {
        if (leading)
        {
            int64_t far_edge = static_cast<int64_t>(origin) + extent;
            // Bound the moving edge by both wire ranges and the fixed opposite edge.
            int64_t lower = std::max<int64_t>(-32768, far_edge - 65535);
            int64_t upper = std::min<int64_t>(32767, far_edge - 1);
            origin = static_cast<int16_t>(std::clamp<int64_t>(static_cast<int64_t>(origin) + delta, lower, upper));
            extent = static_cast<uint16_t>(far_edge - origin);
        }
        else if (trailing)
            extent = geometry_extent(static_cast<int64_t>(extent) + delta);
    };
    resize_axis(start.x, start.width, dx, has(ResizeEdge::Left), has(ResizeEdge::Right));
    resize_axis(start.y, start.height, dy, has(ResizeEdge::Top), has(ResizeEdge::Bottom));
    return start;
}

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

    return clamp_geometry(area, Geometry{ 0, 0, width, height }, target_x, target_y);
}

Geometry presentation_geometry(Geometry normal, Geometry area, bool horizontal, bool vertical)
{
    if (horizontal)
    {
        normal.x = area.x;
        normal.width = area.width;
    }
    if (vertical)
    {
        normal.y = area.y;
        normal.height = area.height;
    }
    return normal;
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
    return clamp_geometry(area, geometry, geometry.x, geometry.y);
}

Geometry recover_to_area(Geometry area, Geometry geometry)
{
    auto overlaps = [](int32_t start, int32_t extent, int32_t area_start, int32_t area_extent)
    { return start < area_start + area_extent && area_start < start + extent; };
    if (overlaps(geometry.x, geometry.width, area.x, area.width)
        && overlaps(geometry.y, geometry.height, area.y, area.height))
        return geometry;
    return place_floating(area, geometry.width, geometry.height, std::nullopt);
}

Geometry translate_to_area(Geometry geometry, Geometry source_area, Geometry target_area)
{
    int32_t x = static_cast<int32_t>(target_area.x) + geometry.x - source_area.x;
    int32_t y = static_cast<int32_t>(target_area.y) + geometry.y - source_area.y;
    return clamp_geometry(target_area, geometry, x, y);
}

} // namespace lwm::floating
