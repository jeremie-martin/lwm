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

// Geometry only: arranging, dropping and resizing share one subdivision of the
// workarea into frames separated by padding.
struct Layout
{
    uint32_t padding = 0;
    LayoutConfig config;

    std::vector<Geometry> arrange(size_t count, Geometry area, Workspace const& workspace) const;
    size_t drop_target_index(size_t count, Geometry area, Workspace const& workspace, int16_t x, int16_t y) const;
    std::optional<SplitHitResult>
    hit_test(size_t count, Geometry area, Workspace const& workspace, int16_t x, int16_t y) const;
};

} // namespace lwm
