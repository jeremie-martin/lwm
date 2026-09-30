#pragma once

#include "lwm/config/config.hpp"
#include "lwm/core/types.hpp"
#include <optional>
#include <vector>

namespace lwm {

enum class SplitDirection
{
    Horizontal,
    Vertical
};

struct SplitHitResult
{
    SplitAddress address;
    SplitDirection direction;
    double ratio;
    int32_t split_pixel_pos;
    int32_t available_extent;
    int32_t cross_min;
    int32_t cross_max;
};

// Geometry only: arranging, dropping and resizing use the same subdivision.
class Layout
{
public:
    Layout(AppearanceConfig const& appearance, LayoutConfig const& config)
        : appearance_(appearance)
        , config_(config)
    { }

    std::vector<Geometry>
    arrange(size_t count, Geometry const& area, LayoutStrategy strategy, SplitRatioMap const& ratios) const;
    size_t drop_target_index(
        size_t count,
        Geometry const& area,
        LayoutStrategy strategy,
        SplitRatioMap const& ratios,
        int16_t x,
        int16_t y
    ) const;
    std::optional<SplitHitResult> hit_test(
        size_t count,
        Geometry const& area,
        LayoutStrategy strategy,
        SplitRatioMap const& ratios,
        int16_t x,
        int16_t y
    ) const;

private:
    AppearanceConfig const& appearance_;
    LayoutConfig const& config_;
};

} // namespace lwm
