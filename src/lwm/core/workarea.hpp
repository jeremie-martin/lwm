#pragma once
#include "types.hpp"

namespace lwm {
// Project root-relative reservations onto one monitor's rectangular workarea.
Strut monitor_strut(DockStrut const& dock, Geometry root, Geometry monitor);
} // namespace lwm
