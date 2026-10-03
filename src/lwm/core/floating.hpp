#pragma once

#include "types.hpp"
#include <optional>
#include <span>

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

Geometry clamp_to_area(Geometry area, Geometry geometry);
// Keep geometry that overlaps area; center geometry lying entirely outside it,
// such as an off-screen or other-monitor rectangle that was never laid out here.
Geometry recover_to_area(Geometry area, Geometry geometry);
Geometry translate_to_area(Geometry geometry, Geometry source_area, Geometry target_area);

} // namespace lwm::floating
