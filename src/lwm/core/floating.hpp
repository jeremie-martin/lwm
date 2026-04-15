#pragma once

#include "types.hpp"
#include <optional>

namespace lwm::floating {

Geometry place_floating(Geometry area, uint16_t width, uint16_t height, std::optional<Geometry> parent);
Geometry clamp_to_area(Geometry area, Geometry geometry);
Geometry translate_to_area(Geometry geometry, Geometry source_area, Geometry target_area);

} // namespace lwm::floating
