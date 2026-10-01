#include "lwm/core/focus.hpp"
#include "state_fixture.hpp"
#include <catch2/catch_test_macros.hpp>

using namespace lwm;
using test::add;
using test::add_floating;

TEST_CASE(
    "Focus fallback uses remembered tiles, history, membership, sticky tiles, then floating recency",
    "[focus][policy]"
)
{
    auto state = test::state();
    for (xcb_window_t id : { 1, 2, 3 }) add(state, id);
    add(state, 4, { .workspace = 1 });
    add(state, 5, { .workspace = 2 });
    add_floating(state, 6);
    add_floating(state, 7);
    state.sticky(4, true);
    state.sticky(5, true);
    for (xcb_window_t id : { 2, 1, 3 }) state.focus(id);
    REQUIRE(focus::fallback(state, 0) == 3);
    state.iconic(3, true);
    REQUIRE(focus::fallback(state, 0) == 1);
    state.iconic(1, true);
    REQUIRE(focus::fallback(state, 0) == 2);
    state.iconic(2, true);
    // Sticky tiles follow reverse workspace order.
    REQUIRE(focus::fallback(state, 0) == 5);
    state.erase(5);
    REQUIRE(focus::fallback(state, 0) == 4);
    state.sticky(4, false);
    // Then the most recent visible floating client.
    REQUIRE(focus::fallback(state, 0) == 7);
    state.focus(6);
    REQUIRE(focus::fallback(state, 0) == 6);
    state.iconic(6, true);
    state.iconic(7, true);
    REQUIRE(focus::fallback(state, 0) == XCB_NONE);
}

TEST_CASE("Automatic focus excludes clients that cannot hold focus", "[focus][policy]")
{
    auto state = test::state(2);
    add(state, 1);
    REQUIRE(focus::fallback(state, 0) == 1);
    SECTION("Iconic") { state.iconic(1, true); }
    SECTION("Other monitor")
    {
        state.relocate(1, 1, 0);
        REQUIRE(focus::fallback(state, 1) == 1);
    }
    SECTION("Hidden workspace") { state.relocate(1, 0, 1); }
    SECTION("Showing desktop includes sticky clients")
    {
        state.sticky(1, true);
        state.show_desktop(true);
    }
    SECTION("Neither input protocol") { state.focus_hints(1, false, false); }
    SECTION("Fullscreen suppression")
    {
        add(state, 2);
        state.fullscreen(2, true);
        REQUIRE(focus::fallback(state, 0) == 2);
        state.focus_hints(2, false, false);
    }
    auto fullscreen = state.fullscreen_visibility();
    CHECK(state.focusable(state.require(1), fullscreen) == (state.require(1).monitor == 1));
    REQUIRE(focus::fallback(state, 0) == XCB_NONE);
    REQUIRE(focus::cycle_target(focus::recent_order(state), state, 0, XCB_NONE, true) == XCB_NONE);
}

TEST_CASE("Fullscreen transients and either input protocol keep focus eligibility", "[focus][policy]")
{
    auto state = test::state();
    add(state, 1);
    add_floating(state, 2);
    state.fullscreen(1, true);
    state.transient(2, 1);
    state.focus_hints(2, false, true);
    CHECK(state.focusable(state.require(2)));
    state.focus_hints(2, true, false);
    CHECK(state.focusable(state.require(2)));
    state.transient(2, XCB_NONE);
    CHECK_FALSE(state.focusable(state.require(2)));
}

TEST_CASE("MRU traversal keeps its order but reads eligibility and lifetime live", "[focus][policy]")
{
    auto state = test::state();
    add(state, 1);
    add_floating(state, 2);
    add(state, 3);
    add(state, 4);
    for (xcb_window_t id : { 1, 2, 3, 4 }) state.focus(id);
    auto order = focus::recent_order(state);
    REQUIRE(order == std::vector<xcb_window_t>{ 4, 3, 2, 1 });
    xcb_window_t current = 4;
    for (xcb_window_t expected : { 3, 2, 1, 4 })
    {
        current = focus::cycle_target(order, state, 0, current, true);
        REQUIRE(current == expected);
        state.focus(current);
    }
    REQUIRE(focus::cycle_target(order, state, 0, 4, false) == 1);
    state.erase(3);
    state.iconic(2, true);
    REQUIRE(focus::cycle_target(order, state, 0, 4, true) == 1);
    state.iconic(2, false);
    REQUIRE(focus::cycle_target(order, state, 0, 4, true) == 2);
    REQUIRE(focus::cycle_target(order, state, 0, 999, true) == 4);
    REQUIRE(focus::cycle_target(order, state, 0, 999, false) == 1);
    state.show_desktop(true);
    REQUIRE(focus::cycle_target(order, state, 0, 4, true) == XCB_NONE);
    REQUIRE(focus::cycle_target({ }, state, 0, 4, false) == XCB_NONE);
}

TEST_CASE("Monitor index at point uses half-open monitor bounds", "[focus][monitor]")
{
    std::vector monitors{ test::monitor("A", -1000), test::monitor("B", 0), test::monitor("C", 1000) };
    CHECK(focus::monitor_index_at_point(monitors, -500, 10) == 0);
    CHECK(focus::monitor_index_at_point(monitors, 999, 10) == 1);
    CHECK(focus::monitor_index_at_point(monitors, 1000, 10) == 2);
    CHECK_FALSE(focus::monitor_index_at_point(monitors, 2000, 10));
    CHECK_FALSE(focus::monitor_index_at_point(monitors, 10, 800));
    CHECK_FALSE(focus::monitor_index_at_point({ }, 0, 0));
}
