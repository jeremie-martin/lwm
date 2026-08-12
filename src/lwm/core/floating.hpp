#pragma once

#include "types.hpp"
#include <optional>

namespace lwm::floating {

Geometry place_floating(Geometry area, uint16_t width, uint16_t height, std::optional<Geometry> parent);

// Runtime normal hints update the geometry restored after fullscreen/maximize,
// not the currently realized state geometry.
Geometry& runtime_hints_geometry(Client& client);

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
