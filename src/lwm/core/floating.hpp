#pragma once

#include "types.hpp"
#include <optional>

namespace lwm::floating {

enum class ResizeEdge : uint8_t
{
    None = 0,
    Left = 1,
    Right = 2,
    Top = 4,
    Bottom = 8
};
constexpr ResizeEdge operator|(ResizeEdge a, ResizeEdge b)
{
    return static_cast<ResizeEdge>(static_cast<uint8_t>(a) | static_cast<uint8_t>(b));
}

// No edges means move. Resizing keeps the opposite edge fixed, within X11 limits.
Geometry drag_geometry(Geometry start, int32_t dx, int32_t dy, ResizeEdge edges);

Geometry place_floating(Geometry area, uint16_t width, uint16_t height, std::optional<Geometry> parent);

// Derive maximized presentation without overwriting normal placement.
Geometry presentation_geometry(Geometry normal, Geometry area, bool horizontal, bool vertical);

struct PositionHintResolution
{
    bool accepted = false;
    size_t monitor = 0;
};

// Resolve a hinted center to a monitor. Transient and client-desktop-pinned
// windows are constrained to their assigned monitor; ordinary windows may move.
PositionHintResolution resolve_position_hint(
    std::vector<Monitor> const& monitors,
    size_t assigned_monitor,
    bool constrained_to_assigned_monitor,
    Geometry hinted_geometry
);

// True if a window of (width,height) placed at (x,y) belongs to `monitor` —
// i.e. its center point lies within the monitor's full geometry rectangle.
bool hint_targets_monitor(Geometry monitor, int16_t x, int16_t y, uint16_t width, uint16_t height);

Geometry clamp_to_area(Geometry area, Geometry geometry);
Geometry translate_to_area(Geometry geometry, Geometry source_area, Geometry target_area);

} // namespace lwm::floating
