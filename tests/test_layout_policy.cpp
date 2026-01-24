#include <catch2/catch_test_macros.hpp>
#include "lwm/layout/layout.hpp"

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
    AppearanceConfig appearance = make_appearance(10, 2);
    Geometry area{ 0, 0, 10, 10 };

    auto slots = layout_policy::calculate_slots(1, area, appearance);

    REQUIRE(slots.size() == 1);
    REQUIRE(slots[0].width == 50);
    REQUIRE(slots[0].height == 50);
}
