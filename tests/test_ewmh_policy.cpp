#include "state_fixture.hpp"
#include <catch2/catch_test_macros.hpp>

using namespace lwm;

TEST_CASE("X timestamp ordering handles 32-bit wraparound", "[ewmh][policy][timestamp]")
{
    REQUIRE(timestamp_is_before(100, 200));
    REQUIRE_FALSE(timestamp_is_before(200, 100));
    REQUIRE_FALSE(timestamp_is_before(200, 200));

    // A small post-wrap timestamp follows a timestamp near UINT32_MAX.
    REQUIRE_FALSE(timestamp_is_before(0x00000020U, 0xFFFFFFF0U));
    REQUIRE(timestamp_is_before(0xFFFFFFF0U, 0x00000020U));
}

TEST_CASE("EWMH desktops are monitor-major placements on existing monitors", "[ewmh][policy]")
{
    for (size_t count : { 1, 3, 10 })
    {
        auto state = test::state(3, count);
        for (size_t monitor = 0; monitor < 3; ++monitor)
            for (size_t workspace = 0; workspace < count; ++workspace)
            {
                auto desktop = state.desktop_index(monitor, workspace);
                CHECK(desktop == monitor * count + workspace);
                CHECK(state.desktop_placement(desktop) == std::pair{ monitor, workspace });
            }
        // Desktops beyond the last monitor and the sticky value are not placements.
        CHECK_FALSE(state.desktop_placement(static_cast<uint32_t>(3 * count)));
        CHECK_FALSE(state.desktop_placement(0xFFFFFFFF));
    }
}
