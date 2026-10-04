#include "layout.hpp"
#include <algorithm>
#include <cmath>
#include <limits>

namespace lwm {

namespace {
// Split indices follow successive cuts: master/stack, then each stack slot.
// Padding surrounds the frames and separates neighbours.
template <class Slot, class Split>
void visit_layout(size_t count, Geometry area, Workspace const& workspace, Layout const& layout, Slot slot, Split split)
{
    int32_t gap = static_cast<int32_t>(layout.padding);
    area = { geometry_coordinate(area.x + gap),
             geometry_coordinate(area.y + gap),
             geometry_extent(area.width - 2 * int64_t{ gap }),
             geometry_extent(area.height - 2 * int64_t{ gap }) };
    for (size_t i = 0; i < count; ++i)
    {
        if (workspace.layout_strategy == LayoutStrategy::Monocle || i + 1 == count)
        {
            slot(i, area);
            continue;
        }
        bool horizontal = i == 0;
        SplitAddress address{ static_cast<uint32_t>(i) };
        double ratio = horizontal ? layout.config.default_ratio : 1.0 / static_cast<double>(count - i);
        if (auto it = workspace.split_ratios.find(address); it != workspace.split_ratios.end())
            ratio = it->second;
        int32_t available = std::max<int32_t>(0, (horizontal ? area.width : area.height) - gap);
        int32_t first = static_cast<int32_t>(std::floor(available * ratio));
        Geometry allocated = area;
        split(SplitHitResult{ address,
                              horizontal ? SplitDirection::Horizontal : SplitDirection::Vertical,
                              ratio,
                              (horizontal ? area.x : area.y) + first + gap / 2,
                              available,
                              horizontal ? area.y : area.x,
                              horizontal ? area.y + static_cast<int32_t>(area.height)
                                         : area.x + static_cast<int32_t>(area.width) });
        if (horizontal)
        {
            allocated.width = static_cast<uint16_t>(std::max(1, first));
            area.x = geometry_coordinate(area.x + first + gap);
            area.width = static_cast<uint16_t>(std::max(1, available - first));
        }
        else
        {
            allocated.height = static_cast<uint16_t>(std::max(1, first));
            area.y = geometry_coordinate(area.y + first + gap);
            area.height = static_cast<uint16_t>(std::max(1, available - first));
        }
        slot(i, allocated);
    }
}

} // namespace

std::vector<Geometry> Layout::arrange(size_t count, Geometry area, Workspace const& workspace) const
{
    std::vector<Geometry> slots(count);
    visit_layout(
        count,
        area,
        workspace,
        *this,
        [&](size_t i, Geometry geometry) { slots[i] = geometry; },
        [](SplitHitResult const&) {}
    );
    return slots;
}

size_t Layout::drop_target_index(size_t count, Geometry area, Workspace const& workspace, int16_t x, int16_t y) const
{
    if (count == 0 || workspace.layout_strategy == LayoutStrategy::Monocle)
        return 0;
    size_t best = 0;
    int64_t distance = std::numeric_limits<int64_t>::max();
    visit_layout(
        count,
        area,
        workspace,
        *this,
        [&](size_t i, Geometry slot)
        {
            int32_t dx = std::max({ static_cast<int32_t>(slot.x) - x, x - (slot.x + slot.width), 0 });
            int32_t dy = std::max({ static_cast<int32_t>(slot.y) - y, y - (slot.y + slot.height), 0 });
            int64_t d = static_cast<int64_t>(dx) * dx + static_cast<int64_t>(dy) * dy;
            if (d < distance)
            {
                distance = d;
                best = i;
            }
        },
        [](SplitHitResult const&) {}
    );
    return best;
}

std::optional<SplitHitResult>
Layout::hit_test(size_t count, Geometry area, Workspace const& workspace, int16_t x, int16_t y) const
{
    if (count < 2 || workspace.layout_strategy == LayoutStrategy::Monocle)
        return std::nullopt;
    std::optional<SplitHitResult> best;
    int32_t distance = std::numeric_limits<int32_t>::max();
    visit_layout(
        count,
        area,
        workspace,
        *this,
        [](size_t, Geometry) {},
        [&](SplitHitResult const& split)
        {
            bool horizontal = split.direction == SplitDirection::Horizontal;
            int32_t cross = horizontal ? y : x;
            int32_t d = std::abs((horizontal ? x : y) - split.split_pixel_pos);
            if (cross >= split.cross_min && cross <= split.cross_max
                && d <= static_cast<int32_t>(config.resize_grab_threshold) && d < distance)
            {
                distance = d;
                best = split;
            }
        }
    );
    return best;
}

} // namespace lwm
