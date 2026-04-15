#pragma once

#include "lwm/core/types.hpp"
#include <cstdint>
#include <memory>
#include <optional>
#include <variant>
#include <vector>

namespace lwm {

// ── Tree types ──────────────────────────────────────────────────────────

enum class SplitDirection
{
    Horizontal,
    Vertical
};

struct SplitNode;
struct LeafNode
{
    size_t window_index; // index into the visible_windows vector
};

using TreeNode = std::variant<std::unique_ptr<SplitNode>, LeafNode>;

struct SplitNode
{
    SplitDirection direction;
    double ratio; // fraction allocated to left/top child (0.0–1.0)
    TreeNode left;
    TreeNode right;
};

// SplitAddress and SplitRatioMap are defined in types.hpp

// ── Hit-test result ─────────────────────────────────────────────────────

struct SplitHitResult
{
    SplitAddress address;
    SplitDirection direction;
    double ratio;
    int32_t split_pixel_pos; // center of the gap, in the split axis
    int32_t available_extent; // total available space along the split axis (for ratio ↔ pixel conversion)
    int32_t cross_min; // perpendicular extent: start of the border line
    int32_t cross_max; // perpendicular extent: end of the border line
};

// ── Tree construction helpers ───────────────────────────────────────────

TreeNode make_leaf(size_t window_index);
TreeNode make_split(SplitDirection direction, double ratio, TreeNode left, TreeNode right);

// ── Tree algorithms ─────────────────────────────────────────────────────

/// Overlay saved ratios onto the tree's split nodes (modifies in place).
void overlay_ratios(TreeNode& node, SplitRatioMap const& ratios, uint8_t depth = 0, uint32_t path = 0);

/// Compute window geometries by recursive subdivision.
/// `content_rect` is the area available for windows (working area inset by outer padding+border).
/// Returns one Geometry per leaf, indexed by leaf's window_index.
std::vector<Geometry> compute_geometries(
    TreeNode const& node,
    Geometry const& content_rect,
    uint32_t padding,
    uint32_t border_width);

/// Collect all split border positions in the tree.
/// Used for hit-testing (finding which split the pointer is near).
void collect_split_borders(
    TreeNode const& node,
    Geometry const& content_rect,
    uint32_t padding,
    uint32_t border_width,
    uint8_t depth,
    uint32_t path,
    std::vector<SplitHitResult>& out);

/// Find the nearest split border to (x, y) within threshold pixels.
std::optional<SplitHitResult> hit_test_split(
    TreeNode const& node,
    Geometry const& content_rect,
    uint32_t padding,
    uint32_t border_width,
    int16_t x,
    int16_t y,
    uint32_t threshold);

/// Compute the content rect from a working area (inset by padding + border on all sides).
Geometry working_area_to_content_rect(Geometry const& working_area, uint32_t padding, uint32_t border_width);

} // namespace lwm
