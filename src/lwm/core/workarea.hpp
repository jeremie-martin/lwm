#pragma once
#include "types.hpp"

namespace lwm {
// Project root-relative reservations onto one monitor's rectangular workarea.
Strut monitor_strut(DockStrut const& dock, Geometry root, Geometry monitor);
// The monitor rectangle less a reservation. Oversized reservations consume the
// extent without shifting the origin.
Geometry working_area(Geometry monitor, Strut strut);
} // namespace lwm
