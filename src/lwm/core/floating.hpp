#pragma once

#include "types.hpp"
#include <optional>

namespace lwm::floating {

Geometry place_floating(Geometry area, uint16_t width, uint16_t height, std::optional<Geometry> parent);

} // namespace lwm::floating
