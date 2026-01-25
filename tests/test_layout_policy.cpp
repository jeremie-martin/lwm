#include "lwm/layout/layout.hpp"
#include <catch2/catch_test_macros.hpp>

using namespace lwm;

namespace {

AppearanceConfig make_appearance(uint32_t padding, uint32_t border)
{
    AppearanceConfig appearance;
    appearance.padding = padding;
    appearance.border_width = border;
    return appearance;
}

} // namespace

TEST_CASE("Layout policy centers a single window with gaps", "[layout][policy]")
{
    AppearanceConfig appearance = make_appearance(10, 2);
    Geometry area{ 0, 0, 200, 100 };

    auto slots = layout_policy::calculate_slots(1, area, appearance);

    REQUIRE(slots.size() == 1);
    REQUIRE(slots[0].x == 12);
    REQUIRE(slots[0].y == 12);
    REQUIRE(slots[0].width == 176);
    REQUIRE(slots[0].height == 76);
}

TEST_CASE("Layout policy splits two windows side by side", "[layout][policy]")
{
    AppearanceConfig appearance = make_appearance(10, 2);
    Geometry area{ 0, 0, 200, 100 };

    auto slots = layout_policy::calculate_slots(2, area, appearance);

    REQUIRE(slots.size() == 2);
    REQUIRE(slots[0].x == 12);
    REQUIRE(slots[0].y == 12);
    REQUIRE(slots[0].width == 81);
    REQUIRE(slots[0].height == 76);
    REQUIRE(slots[1].x == 107);
    REQUIRE(slots[1].y == 12);
    REQUIRE(slots[1].width == 81);
    REQUIRE(slots[1].height == 76);
}

TEST_CASE("Layout policy creates master-stack layout for 3+ windows", "[layout][policy]")
{
    AppearanceConfig appearance = make_appearance(10, 2);
    Geometry area{ 0, 0, 300, 200 };

    auto slots = layout_policy::calculate_slots(3, area, appearance);

    REQUIRE(slots.size() == 3);
    // Master window on left
    REQUIRE(slots[0].x == 12);
    REQUIRE(slots[0].y == 12);
    REQUIRE(slots[0].width == 131);
    REQUIRE(slots[0].height == 176);
    // Stack windows on right
    REQUIRE(slots[1].x == 157);
    REQUIRE(slots[1].y == 12);
    REQUIRE(slots[1].width == 131);
    REQUIRE(slots[1].height == 81);
    REQUIRE(slots[2].x == 157);
    REQUIRE(slots[2].y == 107);
    REQUIRE(slots[2].width == 131);
    REQUIRE(slots[2].height == 81);
}

TEST_CASE("Layout policy chooses nearest drop target", "[layout][policy]")
{
    AppearanceConfig appearance = make_appearance(10, 2);
    Geometry area{ 0, 0, 200, 100 };

    size_t left_idx = layout_policy::drop_target_index(2, area, appearance, 20, 20);
    size_t right_idx = layout_policy::drop_target_index(2, area, appearance, 150, 20);

    REQUIRE(left_idx == 0);
    REQUIRE(right_idx == 1);
}

TEST_CASE("Layout policy enforces minimum dimensions", "[layout][policy]")
{
    // Even when area is too small, slots get minimum usable size
    constexpr uint16_t kMinDimension = 50;

    AppearanceConfig appearance = make_appearance(10, 2);
    Geometry area{ 0, 0, 10, 10 };

    auto slots = layout_policy::calculate_slots(1, area, appearance);

    REQUIRE(slots.size() == 1);
    REQUIRE(slots[0].width >= kMinDimension);
    REQUIRE(slots[0].height >= kMinDimension);
}

TEST_CASE("Layout policy with zero gaps divides area evenly", "[layout][policy][edge]")
{
    AppearanceConfig appearance = make_appearance(0, 0);
    Geometry area{ 0, 0, 200, 100 };

    auto slots = layout_policy::calculate_slots(2, area, appearance);

    REQUIRE(slots.size() == 2);
    REQUIRE(slots[0].x == 0);
    REQUIRE(slots[0].y == 0);
    REQUIRE(slots[0].width == 100);
    REQUIRE(slots[0].height == 100);
    REQUIRE(slots[1].x == 100);
    REQUIRE(slots[1].y == 0);
    REQUIRE(slots[1].width == 100);
    REQUIRE(slots[1].height == 100);
}

TEST_CASE("Layout policy with border only applies offset", "[layout][policy][edge]")
{
    AppearanceConfig appearance = make_appearance(0, 5);
    Geometry area{ 0, 0, 200, 100 };

    auto slots = layout_policy::calculate_slots(2, area, appearance);

    REQUIRE(slots.size() == 2);
    REQUIRE(slots[0].x == 5);
    REQUIRE(slots[0].y == 5);
    REQUIRE(slots[1].x > slots[0].x + slots[0].width);
}

TEST_CASE("Layout policy handles large padding relative to area", "[layout][policy][edge]")
{
    AppearanceConfig appearance = make_appearance(100, 10);
    Geometry area{ 0, 0, 200, 150 };

    auto slots = layout_policy::calculate_slots(2, area, appearance);

    REQUIRE(slots.size() == 2);
    REQUIRE(slots[0].width > 0);
    REQUIRE(slots[0].height >= 50);
    REQUIRE(slots[1].width > 0);
    REQUIRE(slots[1].height >= 50);
}

TEST_CASE("Layout policy distributes many stack windows evenly", "[layout][policy][edge]")
{
    AppearanceConfig appearance = make_appearance(5, 1);
    Geometry area{ 0, 0, 800, 600 };

    auto slots = layout_policy::calculate_slots(10, area, appearance);

    REQUIRE(slots.size() == 10);
    REQUIRE(slots[0].x == 6);
    REQUIRE(slots[0].y == 6);

    // All stack windows have equal height and x position
    uint16_t first_stack_height = slots[1].height;
    for (size_t i = 2; i < 10; ++i)
    {
        REQUIRE(slots[i].height == first_stack_height);
        REQUIRE(slots[i].x == slots[1].x);
    }

    // Stack windows are vertically stacked
    for (size_t i = 2; i < 10; ++i)
    {
        REQUIRE(slots[i].y > slots[i - 1].y);
    }
}

TEST_CASE("Layout policy drop target with point inside slot", "[layout][policy][edge]")
{
    AppearanceConfig appearance = make_appearance(0, 0);
    Geometry area{ 0, 0, 200, 100 };

    // For 2 windows with no gaps, first is at 0-99, second at 100-199
    size_t idx1 = layout_policy::drop_target_index(2, area, appearance, 50, 50);
    size_t idx2 = layout_policy::drop_target_index(2, area, appearance, 150, 50);
    size_t idx_edge = layout_policy::drop_target_index(2, area, appearance, 100, 50);

    REQUIRE(idx1 == 0);
    REQUIRE(idx2 == 1);
    // At exact boundary, should pick one of them (implementation-defined which)
    bool valid_edge = (idx_edge == 0) || (idx_edge == 1);
    REQUIRE(valid_edge);
}

TEST_CASE("Layout policy creates valid slots with very large window count", "[layout][policy][edge]")
{
    AppearanceConfig appearance = make_appearance(5, 1);
    Geometry area{ 0, 0, 1920, 1080 };
    auto slots = layout_policy::calculate_slots(100, area, appearance);

    REQUIRE(slots.size() == 100);
    for (auto const& slot : slots)
    {
        REQUIRE(slot.width >= 0);
        REQUIRE(slot.height >= 0);
    }
}

TEST_CASE("Layout policy clamps dimensions with very large padding", "[layout][policy][edge]")
{
    AppearanceConfig appearance = make_appearance(10000, 5);
    Geometry area{ 0, 0, 1920, 1080 };
    auto slots = layout_policy::calculate_slots(2, area, appearance);

    REQUIRE(slots.size() == 2);
    for (auto const& slot : slots)
    {
        REQUIRE(slot.width >= 0);
        REQUIRE(slot.height >= 50);
    }
}

TEST_CASE("Layout policy clamps dimensions with very large border", "[layout][policy][edge]")
{
    AppearanceConfig appearance = make_appearance(10, 10000);
    Geometry area{ 0, 0, 1920, 1080 };
    auto slots = layout_policy::calculate_slots(2, area, appearance);

    REQUIRE(slots.size() == 2);
    for (auto const& slot : slots)
    {
        REQUIRE(slot.width >= 0);
        REQUIRE(slot.height >= 50);
    }
}

TEST_CASE("Layout policy preserves large positive coordinates", "[layout][policy][edge]")
{
    AppearanceConfig appearance = make_appearance(0, 0);
    Geometry area{ 30000, 30000, 100, 100 };
    auto slots = layout_policy::calculate_slots(1, area, appearance);

    REQUIRE(slots.size() == 1);
    REQUIRE(slots[0].x == 30000);
    REQUIRE(slots[0].y == 30000);
}

TEST_CASE("Layout policy preserves negative coordinates", "[layout][policy][edge]")
{
    AppearanceConfig appearance = make_appearance(0, 0);
    Geometry area{ -30000, -30000, 100, 100 };
    auto slots = layout_policy::calculate_slots(1, area, appearance);

    REQUIRE(slots.size() == 1);
    REQUIRE(static_cast<int>(slots[0].x) == -30000);
    REQUIRE(static_cast<int>(slots[0].y) == -30000);
}

TEST_CASE("Layout policy preserves maximum uint16_t dimensions", "[layout][policy][edge]")
{
    AppearanceConfig appearance = make_appearance(0, 0);
    Geometry area{ 0, 0, 65535, 65535 };
    auto slots = layout_policy::calculate_slots(1, area, appearance);

    REQUIRE(slots.size() == 1);
    REQUIRE(slots[0].width == 65535);
    REQUIRE(slots[0].height == 65535);
}

TEST_CASE("Drop target handles edge cases", "[layout][policy][edge]")
{
    AppearanceConfig appearance = make_appearance(10, 2);
    Geometry area{ 0, 0, 200, 100 };

    SECTION("Empty slots returns index 0")
    {
        size_t idx = layout_policy::drop_target_index(0, area, appearance, 50, 50);
        REQUIRE(idx == 0);
    }

    SECTION("Far points return valid index within range")
    {
        AppearanceConfig zero_gaps = make_appearance(0, 0);
        size_t idx = layout_policy::drop_target_index(2, area, zero_gaps, 100000, 100000);
        REQUIRE(idx < 2);
    }
}