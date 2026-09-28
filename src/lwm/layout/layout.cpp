#include "layout.hpp"
#include <algorithm>
#include <cmath>
#include <limits>

namespace lwm {

Geometry working_area_to_content_rect(Geometry const& area, uint32_t padding, uint32_t border_width)
{
    int32_t inset = static_cast<int32_t>(padding + border_width);
    return { static_cast<int16_t>(area.x + inset),
             static_cast<int16_t>(area.y + inset),
             static_cast<uint16_t>(std::max<int32_t>(1, area.width - 2 * inset)),
             static_cast<uint16_t>(std::max<int32_t>(1, area.height - 2 * inset)) };
}

namespace {

// Master-stack is a sequence of cuts, not an arbitrary tree. Visiting cuts
// directly avoids node allocations, recursion and depth-limited path encoding.
template <class Slot, class Split>
void visit_layout(
    size_t count,
    Geometry area,
    LayoutStrategy strategy,
    SplitRatioMap const& ratios,
    AppearanceConfig const& appearance,
    LayoutConfig const& config,
    Slot slot,
    Split split
)
{
    area = working_area_to_content_rect(area, appearance.padding, appearance.border_width);
    int32_t gap = static_cast<int32_t>(appearance.padding + 2 * appearance.border_width);
    for (size_t i = 0; i < count; ++i)
    {
        if (strategy == LayoutStrategy::Monocle || i + 1 == count)
        {
            slot(i, area);
            continue;
        }
        bool horizontal = i == 0;
        SplitAddress address{ static_cast<uint32_t>(i) };
        double ratio = horizontal ? config.default_ratio : 1.0 / static_cast<double>(count - i);
        if (auto it = ratios.find(address); it != ratios.end())
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
            area.x = static_cast<int16_t>(area.x + first + gap);
            area.width = static_cast<uint16_t>(std::max(1, available - first));
        }
        else
        {
            allocated.height = static_cast<uint16_t>(std::max(1, first));
            area.y = static_cast<int16_t>(area.y + first + gap);
            area.height = static_cast<uint16_t>(std::max(1, available - first));
        }
        slot(i, allocated);
    }
}

} // namespace

std::vector<Geometry>
Layout::arrange(size_t count, Geometry const& area, LayoutStrategy strategy, SplitRatioMap const& ratios) const
{
    std::vector<Geometry> slots(count);
    visit_layout(
        count,
        area,
        strategy,
        ratios,
        appearance_,
        config_,
        [&](size_t i, Geometry geometry) { slots[i] = geometry; },
        [](SplitHitResult const&) {}
    );
    return slots;
}

size_t Layout::drop_target_index(
    size_t count,
    Geometry const& area,
    LayoutStrategy strategy,
    SplitRatioMap const& ratios,
    int16_t x,
    int16_t y
) const
{
    if (count == 0 || strategy == LayoutStrategy::Monocle)
        return 0;
    size_t best = 0;
    int64_t distance = std::numeric_limits<int64_t>::max();
    visit_layout(
        count,
        area,
        strategy,
        ratios,
        appearance_,
        config_,
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

std::optional<SplitHitResult> Layout::hit_test(
    size_t count,
    Geometry const& area,
    LayoutStrategy strategy,
    SplitRatioMap const& ratios,
    int16_t x,
    int16_t y
) const
{
    if (count < 2 || strategy == LayoutStrategy::Monocle)
        return std::nullopt;
    std::optional<SplitHitResult> best;
    int32_t distance = std::numeric_limits<int32_t>::max();
    visit_layout(
        count,
        area,
        strategy,
        ratios,
        appearance_,
        config_,
        [](size_t, Geometry) {},
        [&](SplitHitResult const& split)
        {
            bool horizontal = split.direction == SplitDirection::Horizontal;
            int32_t cross = horizontal ? y : x;
            int32_t d = std::abs((horizontal ? x : y) - split.split_pixel_pos);
            if (cross >= split.cross_min && cross <= split.cross_max
                && d <= static_cast<int32_t>(config_.resize_grab_threshold) && d < distance)
            {
                distance = d;
                best = split;
            }
        }
    );
    return best;
}

} // namespace lwm
