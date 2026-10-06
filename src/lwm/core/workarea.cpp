#include "workarea.hpp"
#include <algorithm>

namespace lwm {
namespace {
uint32_t inset(
    EdgeReservation edge,
    int64_t root_start,
    int64_t root_end,
    int64_t monitor_start,
    int64_t monitor_end,
    int64_t cross_start,
    int64_t cross_end,
    bool far_edge
)
{
    if (!edge.depth || edge.start > edge.end || cross_end <= edge.start || cross_start > edge.end
        || monitor_end <= root_start || monitor_start >= root_end)
        return 0;
    // Widen before arithmetic: CARDINAL permits values through UINT32_MAX.
    int64_t boundary =
        far_edge ? std::max(root_start, root_end - edge.depth) : std::min(root_end, root_start + edge.depth);
    int64_t amount = far_edge ? monitor_end - boundary : boundary - monitor_start;
    return std::clamp<int64_t>(amount, 0, monitor_end - monitor_start);
}
}
Strut monitor_strut(DockStrut const& dock, Geometry root, Geometry monitor)
{
    int64_t x = monitor.x, y = monitor.y;
    int64_t right = x + monitor.width, bottom = y + monitor.height;
    int64_t root_right = int64_t(root.x) + root.width, root_bottom = int64_t(root.y) + root.height;
    return {
        inset(dock.left, root.x, root_right, x, right, y, bottom, false),
        inset(dock.right, root.x, root_right, x, right, y, bottom, true),
        inset(dock.top, root.y, root_bottom, y, bottom, x, right, false),
        inset(dock.bottom, root.y, root_bottom, y, bottom, x, right, true),
    };
}
Geometry working_area(Geometry monitor, Strut strut)
{
    auto const& [x, y, width, height] = monitor;
    uint64_t horizontal = static_cast<uint64_t>(strut.left) + strut.right;
    uint64_t vertical = static_cast<uint64_t>(strut.top) + strut.bottom;
    return { geometry_coordinate(static_cast<int64_t>(x) + (horizontal >= width ? 0 : strut.left)),
             geometry_coordinate(static_cast<int64_t>(y) + (vertical >= height ? 0 : strut.top)),
             geometry_extent(static_cast<int64_t>(width) - std::min<uint64_t>(width, horizontal)),
             geometry_extent(static_cast<int64_t>(height) - std::min<uint64_t>(height, vertical)) };
}

} // namespace lwm
