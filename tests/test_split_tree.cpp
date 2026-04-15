#include "lwm/layout/split_tree.hpp"
#include "lwm/layout/strategy.hpp"
#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

using namespace lwm;

namespace {

Geometry make_area(int16_t x, int16_t y, uint16_t w, uint16_t h)
{
    return { x, y, w, h };
}

} // namespace

// ── Strategy construction ──────────────────────────────────────────────

TEST_CASE("Master-stack strategy builds single leaf for 1 window", "[split_tree][strategy]")
{
    auto tree = build_master_stack(1);
    REQUIRE(std::holds_alternative<LeafNode>(tree));
    CHECK(std::get<LeafNode>(tree).window_index == 0);
}

TEST_CASE("Master-stack strategy builds H-split for 2 windows", "[split_tree][strategy]")
{
    auto tree = build_master_stack(2);
    REQUIRE(std::holds_alternative<std::unique_ptr<SplitNode>>(tree));
    auto& root = *std::get<std::unique_ptr<SplitNode>>(tree);
    CHECK(root.direction == SplitDirection::Horizontal);
    CHECK_THAT(root.ratio, Catch::Matchers::WithinAbs(0.5, 0.001));
    REQUIRE(std::holds_alternative<LeafNode>(root.left));
    REQUIRE(std::holds_alternative<LeafNode>(root.right));
    CHECK(std::get<LeafNode>(root.left).window_index == 0);
    CHECK(std::get<LeafNode>(root.right).window_index == 1);
}

TEST_CASE("Master-stack strategy builds correct shape for 3 windows", "[split_tree][strategy]")
{
    auto tree = build_master_stack(3);
    REQUIRE(std::holds_alternative<std::unique_ptr<SplitNode>>(tree));
    auto& root = *std::get<std::unique_ptr<SplitNode>>(tree);
    CHECK(root.direction == SplitDirection::Horizontal);

    // Left child is leaf(0)
    REQUIRE(std::holds_alternative<LeafNode>(root.left));
    CHECK(std::get<LeafNode>(root.left).window_index == 0);

    // Right child is V-split
    REQUIRE(std::holds_alternative<std::unique_ptr<SplitNode>>(root.right));
    auto& stack = *std::get<std::unique_ptr<SplitNode>>(root.right);
    CHECK(stack.direction == SplitDirection::Vertical);
    CHECK_THAT(stack.ratio, Catch::Matchers::WithinAbs(0.5, 0.001));

    REQUIRE(std::holds_alternative<LeafNode>(stack.left));
    CHECK(std::get<LeafNode>(stack.left).window_index == 1);
    REQUIRE(std::holds_alternative<LeafNode>(stack.right));
    CHECK(std::get<LeafNode>(stack.right).window_index == 2);
}

TEST_CASE("Master-stack strategy builds right-leaning chain for 4 windows", "[split_tree][strategy]")
{
    auto tree = build_master_stack(4);
    auto& root = *std::get<std::unique_ptr<SplitNode>>(tree);

    // Right child is V-split chain
    auto& v1 = *std::get<std::unique_ptr<SplitNode>>(root.right);
    CHECK(v1.direction == SplitDirection::Vertical);
    REQUIRE(std::holds_alternative<LeafNode>(v1.left));
    CHECK(std::get<LeafNode>(v1.left).window_index == 1);

    auto& v2 = *std::get<std::unique_ptr<SplitNode>>(v1.right);
    CHECK(v2.direction == SplitDirection::Vertical);
    REQUIRE(std::holds_alternative<LeafNode>(v2.left));
    CHECK(std::get<LeafNode>(v2.left).window_index == 2);
    REQUIRE(std::holds_alternative<LeafNode>(v2.right));
    CHECK(std::get<LeafNode>(v2.right).window_index == 3);
}

// ── Geometry computation ───────────────────────────────────────────────

TEST_CASE("Single window gets full content rect", "[split_tree][geometry]")
{
    auto tree = build_master_stack(1);
    Geometry area = make_area(0, 0, 1920, 1080);
    uint32_t padding = 10;
    uint32_t border = 2;

    auto content = working_area_to_content_rect(area, padding, border);
    auto geoms = compute_geometries(tree, content, padding, border);

    REQUIRE(geoms.size() == 1);
    // Single window gets the full content rect
    CHECK(geoms[0].x == content.x);
    CHECK(geoms[0].y == content.y);
    CHECK(geoms[0].width == content.width);
    CHECK(geoms[0].height == content.height);
}

TEST_CASE("Two windows split evenly with default 0.5 ratio", "[split_tree][geometry]")
{
    auto tree = build_master_stack(2);
    Geometry area = make_area(0, 0, 1920, 1080);
    uint32_t padding = 10;
    uint32_t border = 2;

    auto content = working_area_to_content_rect(area, padding, border);
    auto geoms = compute_geometries(tree, content, padding, border);

    REQUIRE(geoms.size() == 2);
    // Both windows should have the same height
    CHECK(geoms[0].height == geoms[1].height);
    // Left window starts at content rect origin
    CHECK(geoms[0].x == content.x);
    // Right window is offset
    CHECK(geoms[1].x > geoms[0].x + geoms[0].width);
    // Widths should be approximately equal
    CHECK(std::abs(static_cast<int>(geoms[0].width) - static_cast<int>(geoms[1].width)) <= 1);
}

TEST_CASE("Ratio overlay changes geometry proportions", "[split_tree][geometry]")
{
    auto tree = build_master_stack(2);
    Geometry area = make_area(0, 0, 1000, 500);
    uint32_t padding = 10;
    uint32_t border = 2;

    // Set master ratio to 0.7
    SplitRatioMap ratios;
    ratios[SplitAddress { 0, 0 }] = 0.7;
    overlay_ratios(tree, ratios);

    auto content = working_area_to_content_rect(area, padding, border);
    auto geoms = compute_geometries(tree, content, padding, border);

    REQUIRE(geoms.size() == 2);
    // Master (left) should be significantly wider than stack (right)
    CHECK(geoms[0].width > geoms[1].width);
    // Master should be roughly 70% of available
    double ratio = static_cast<double>(geoms[0].width) / static_cast<double>(geoms[0].width + geoms[1].width);
    CHECK_THAT(ratio, Catch::Matchers::WithinAbs(0.7, 0.05));
}

TEST_CASE("Three windows: stack windows split vertically", "[split_tree][geometry]")
{
    auto tree = build_master_stack(3);
    Geometry area = make_area(0, 0, 1920, 1080);
    uint32_t padding = 10;
    uint32_t border = 2;

    auto content = working_area_to_content_rect(area, padding, border);
    auto geoms = compute_geometries(tree, content, padding, border);

    REQUIRE(geoms.size() == 3);
    // Master takes full height
    CHECK(geoms[0].height == content.height);
    // Stack windows share the right side
    CHECK(geoms[1].x == geoms[2].x);
    // Stack windows should be approximately equal height
    CHECK(std::abs(static_cast<int>(geoms[1].height) - static_cast<int>(geoms[2].height)) <= 1);
    // Stack windows are stacked vertically (win2 above win3)
    CHECK(geoms[2].y > geoms[1].y);
}

// ── Hit testing ────────────────────────────────────────────────────────

TEST_CASE("Hit test detects horizontal split border", "[split_tree][hit_test]")
{
    auto tree = build_master_stack(2);
    Geometry area = make_area(0, 0, 1000, 500);
    uint32_t padding = 10;
    uint32_t border = 2;

    auto content = working_area_to_content_rect(area, padding, border);

    // The split border should be roughly at x = 500
    auto hit = hit_test_split(tree, content, padding, border, 500, 250, 20);
    REQUIRE(hit.has_value());
    CHECK(hit->direction == SplitDirection::Horizontal);
    CHECK(hit->address.depth == 0);
}

TEST_CASE("Hit test returns nullopt far from borders", "[split_tree][hit_test]")
{
    auto tree = build_master_stack(2);
    Geometry area = make_area(0, 0, 1000, 500);
    uint32_t padding = 10;
    uint32_t border = 2;

    auto content = working_area_to_content_rect(area, padding, border);

    // Far from the split border
    auto hit = hit_test_split(tree, content, padding, border, 100, 250, 8);
    CHECK(!hit.has_value());
}

TEST_CASE("Hit test detects vertical split in stack", "[split_tree][hit_test]")
{
    auto tree = build_master_stack(3);
    Geometry area = make_area(0, 0, 1000, 1000);
    uint32_t padding = 10;
    uint32_t border = 2;

    auto content = working_area_to_content_rect(area, padding, border);

    // The vertical stack split should be roughly at y = 500, x = 750 (right half)
    auto hit = hit_test_split(tree, content, padding, border, 750, 500, 20);
    REQUIRE(hit.has_value());
    CHECK(hit->direction == SplitDirection::Vertical);
    CHECK(hit->address.depth == 1);
}

TEST_CASE("Hit test reports the live ratio for non-root stack splits", "[split_tree][hit_test]")
{
    auto tree = build_master_stack(4);
    Geometry area = make_area(0, 0, 1000, 1000);
    uint32_t padding = 10;
    uint32_t border = 2;

    auto content = working_area_to_content_rect(area, padding, border);

    std::vector<SplitHitResult> borders;
    collect_split_borders(tree, content, padding, border, 0, 0, borders);

    auto it = std::ranges::find_if(
        borders,
        [](SplitHitResult const& border_hit)
        {
            return border_hit.address == SplitAddress { 1, 1 };
        });

    REQUIRE(it != borders.end());
    CHECK_THAT(it->ratio, Catch::Matchers::WithinAbs(1.0 / 3.0, 0.001));

    int16_t probe_x = static_cast<int16_t>((it->cross_min + it->cross_max) / 2);
    int16_t probe_y = static_cast<int16_t>(it->split_pixel_pos);
    auto hit = hit_test_split(tree, content, padding, border, probe_x, probe_y, 20);
    REQUIRE(hit.has_value());
    CHECK(hit->address == SplitAddress { 1, 1 });
    CHECK_THAT(hit->ratio, Catch::Matchers::WithinAbs(1.0 / 3.0, 0.001));
}

TEST_CASE("Hit test returns nullopt for single window", "[split_tree][hit_test]")
{
    auto tree = build_master_stack(1);
    Geometry area = make_area(0, 0, 1000, 500);
    uint32_t padding = 10;
    uint32_t border = 2;

    auto content = working_area_to_content_rect(area, padding, border);

    auto hit = hit_test_split(tree, content, padding, border, 500, 250, 20);
    CHECK(!hit.has_value());
}

// ── SplitAddress ───────────────────────────────────────────────────────

TEST_CASE("SplitAddress comparison works correctly", "[split_tree][address]")
{
    SplitAddress a { 0, 0 };
    SplitAddress b { 0, 0 };
    SplitAddress c { 1, 1 };

    CHECK(a == b);
    CHECK(a != c);
    CHECK(a < c);
}

TEST_CASE("SplitAddress serialization preserves the full path", "[split_tree][address]")
{
    SplitAddress address { 27, 0xF1234567u };

    auto serialized = serialize_split_address(address);
    CHECK(serialized.depth == 27);
    CHECK(serialized.path == 0xF1234567u);

    auto restored = deserialize_split_address(serialized.depth, serialized.path);
    REQUIRE(restored.has_value());
    CHECK(*restored == address);
}

// ── Overlay ratios ─────────────────────────────────────────────────────

TEST_CASE("Overlay ratios modifies tree in place", "[split_tree][overlay]")
{
    auto tree = build_master_stack(3);
    SplitRatioMap ratios;
    ratios[SplitAddress { 0, 0 }] = 0.7;     // root H-split
    ratios[SplitAddress { 1, 1 }] = 0.3;      // first V-split (right child of root)

    overlay_ratios(tree, ratios);

    auto& root = *std::get<std::unique_ptr<SplitNode>>(tree);
    CHECK_THAT(root.ratio, Catch::Matchers::WithinAbs(0.7, 0.001));

    auto& vsplit = *std::get<std::unique_ptr<SplitNode>>(root.right);
    CHECK_THAT(vsplit.ratio, Catch::Matchers::WithinAbs(0.3, 0.001));
}
