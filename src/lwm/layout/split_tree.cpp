#include "split_tree.hpp"
#include <algorithm>
#include <cmath>
#include <limits>

namespace lwm {

// ── Construction helpers ────────────────────────────────────────────────

TreeNode make_leaf(size_t window_index)
{
    return LeafNode { window_index };
}

TreeNode make_split(SplitDirection direction, double ratio, TreeNode left, TreeNode right)
{
    auto node = std::make_unique<SplitNode>();
    node->direction = direction;
    node->ratio = ratio;
    node->left = std::move(left);
    node->right = std::move(right);
    return node;
}

// ── Ratio overlay ───────────────────────────────────────────────────────

void overlay_ratios(TreeNode& node, SplitRatioMap const& ratios, uint8_t depth, uint32_t path)
{
    auto* split = std::get_if<std::unique_ptr<SplitNode>>(&node);
    if (!split)
        return;

    SplitAddress addr { depth, path };
    if (auto it = ratios.find(addr); it != ratios.end())
        (*split)->ratio = it->second;

    overlay_ratios((*split)->left, ratios, static_cast<uint8_t>(depth + 1), path);
    overlay_ratios(
        (*split)->right, ratios, static_cast<uint8_t>(depth + 1),
        static_cast<uint32_t>(path | (1u << depth)));
}

// ── Geometry helpers (internal) ─────────────────────────────────────────

namespace {

struct SplitResult
{
    Geometry left_rect;
    Geometry right_rect;
    int32_t split_pixel_pos; // center of the gap
    int32_t available;       // total available extent along split axis
};

/// Divide a content rect along the given direction using the given ratio.
/// The gap between children is `padding + 2 * border_width`.
SplitResult subdivide(
    Geometry const& rect,
    SplitDirection direction,
    double ratio,
    uint32_t padding,
    uint32_t border_width)
{
    int32_t gap = static_cast<int32_t>(padding + 2 * border_width);

    if (direction == SplitDirection::Horizontal)
    {
        int32_t avail = std::max<int32_t>(0, static_cast<int32_t>(rect.width) - gap);
        int32_t left_w = static_cast<int32_t>(std::floor(avail * ratio));
        int32_t right_w = avail - left_w;

        // Split pixel position = center of the gap
        int32_t split_pos = rect.x + left_w + gap / 2;

        Geometry left_rect {
            rect.x,
            rect.y,
            static_cast<uint16_t>(std::max<int32_t>(1, left_w)),
            rect.height
        };
        Geometry right_rect {
            static_cast<int16_t>(rect.x + left_w + gap),
            rect.y,
            static_cast<uint16_t>(std::max<int32_t>(1, right_w)),
            rect.height
        };

        return { left_rect, right_rect, split_pos, avail };
    }
    else
    {
        int32_t avail = std::max<int32_t>(0, static_cast<int32_t>(rect.height) - gap);
        int32_t top_h = static_cast<int32_t>(std::floor(avail * ratio));
        int32_t bottom_h = avail - top_h;

        int32_t split_pos = rect.y + top_h + gap / 2;

        Geometry top_rect {
            rect.x,
            rect.y,
            rect.width,
            static_cast<uint16_t>(std::max<int32_t>(1, top_h))
        };
        Geometry bottom_rect {
            rect.x,
            static_cast<int16_t>(rect.y + top_h + gap),
            rect.width,
            static_cast<uint16_t>(std::max<int32_t>(1, bottom_h))
        };

        return { top_rect, bottom_rect, split_pos, avail };
    }
}

} // namespace

// ── Geometry computation ────────────────────────────────────────────────

Geometry working_area_to_content_rect(Geometry const& wa, uint32_t padding, uint32_t border_width)
{
    int32_t inset = static_cast<int32_t>(padding + border_width);
    int32_t w = std::max<int32_t>(1, static_cast<int32_t>(wa.width) - 2 * inset);
    int32_t h = std::max<int32_t>(1, static_cast<int32_t>(wa.height) - 2 * inset);
    return {
        static_cast<int16_t>(wa.x + inset),
        static_cast<int16_t>(wa.y + inset),
        static_cast<uint16_t>(w),
        static_cast<uint16_t>(h)
    };
}

namespace {

void compute_geometries_impl(
    TreeNode const& node,
    Geometry const& content_rect,
    uint32_t padding,
    uint32_t border_width,
    std::vector<Geometry>& out)
{
    std::visit(
        [&](auto const& n)
        {
            using T = std::decay_t<decltype(n)>;

            if constexpr (std::is_same_v<T, LeafNode>)
            {
                if (out.size() <= n.window_index)
                    out.resize(n.window_index + 1);
                out[n.window_index] = content_rect;
            }
            else if constexpr (std::is_same_v<T, std::unique_ptr<SplitNode>>)
            {
                auto [left_rect, right_rect, split_pos, avail] =
                    subdivide(content_rect, n->direction, n->ratio, padding, border_width);

                compute_geometries_impl(n->left, left_rect, padding, border_width, out);
                compute_geometries_impl(n->right, right_rect, padding, border_width, out);
            }
        },
        node);
}

} // namespace

std::vector<Geometry> compute_geometries(
    TreeNode const& node,
    Geometry const& content_rect,
    uint32_t padding,
    uint32_t border_width)
{
    std::vector<Geometry> result;
    compute_geometries_impl(node, content_rect, padding, border_width, result);
    return result;
}

// ── Split border collection ─────────────────────────────────────────────

void collect_split_borders(
    TreeNode const& node,
    Geometry const& content_rect,
    uint32_t padding,
    uint32_t border_width,
    uint8_t depth,
    uint32_t path,
    std::vector<SplitHitResult>& out)
{
    auto const* split = std::get_if<std::unique_ptr<SplitNode>>(&node);
    if (!split)
        return;

    auto [left_rect, right_rect, split_pos, avail] =
        subdivide(content_rect, (*split)->direction, (*split)->ratio, padding, border_width);

    int32_t cross_min, cross_max;
    if ((*split)->direction == SplitDirection::Horizontal)
    {
        cross_min = content_rect.y;
        cross_max = content_rect.y + static_cast<int32_t>(content_rect.height);
    }
    else
    {
        cross_min = content_rect.x;
        cross_max = content_rect.x + static_cast<int32_t>(content_rect.width);
    }

    out.push_back(SplitHitResult {
        SplitAddress { depth, path },
        (*split)->direction,
        (*split)->ratio,
        split_pos,
        avail,
        cross_min,
        cross_max });

    collect_split_borders(
        (*split)->left, left_rect, padding, border_width,
        static_cast<uint8_t>(depth + 1), path, out);

    collect_split_borders(
        (*split)->right, right_rect, padding, border_width,
        static_cast<uint8_t>(depth + 1), static_cast<uint32_t>(path | (1u << depth)), out);
}

// ── Hit testing ─────────────────────────────────────────────────────────

std::optional<SplitHitResult> hit_test_split(
    TreeNode const& node,
    Geometry const& content_rect,
    uint32_t padding,
    uint32_t border_width,
    int16_t x,
    int16_t y,
    uint32_t threshold)
{
    std::vector<SplitHitResult> borders;
    collect_split_borders(node, content_rect, padding, border_width, 0, 0, borders);

    std::optional<SplitHitResult> best;
    int32_t best_dist = std::numeric_limits<int32_t>::max();
    int32_t thresh = static_cast<int32_t>(threshold);

    for (auto const& border : borders)
    {
        int32_t dist;
        int32_t cross_coord;
        if (border.direction == SplitDirection::Horizontal)
        {
            dist = std::abs(static_cast<int32_t>(x) - border.split_pixel_pos);
            cross_coord = static_cast<int32_t>(y);
        }
        else
        {
            dist = std::abs(static_cast<int32_t>(y) - border.split_pixel_pos);
            cross_coord = static_cast<int32_t>(x);
        }

        if (cross_coord < border.cross_min || cross_coord > border.cross_max)
            continue;

        if (dist <= thresh && dist < best_dist)
        {
            best_dist = dist;
            best = border;
        }
    }

    return best;
}

} // namespace lwm
