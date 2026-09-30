#pragma once
#include "types.hpp"

namespace lwm {
struct EdgeReservation
{
    uint32_t depth = 0;
    uint32_t start = 0;
    uint32_t end = UINT32_MAX; // Inclusive root-coordinate range; legacy struts span the edge.
};
struct DockStrut
{
    EdgeReservation left, right, top, bottom;
    bool empty() const { return !left.depth && !right.depth && !top.depth && !bottom.depth; }
};

// Project root-relative reservations onto one monitor's rectangular workarea.
Strut monitor_strut(DockStrut const& dock, Geometry root, Geometry monitor);
} // namespace lwm
