#pragma once

#include "types.hpp"
#include <optional>

namespace lwm::floating {

Geometry place_floating(Geometry area, uint16_t width, uint16_t height, std::optional<Geometry> parent);

// True if a window of (width,height) placed at (x,y) belongs to `monitor` —
// i.e. its center point lies within the monitor's full geometry rectangle.
bool hint_targets_monitor(Geometry monitor, int16_t x, int16_t y, uint16_t width, uint16_t height);

Geometry clamp_to_area(Geometry area, Geometry geometry);
Geometry translate_to_area(Geometry geometry, Geometry source_area, Geometry target_area);

} // namespace lwm::floating
