#include "lwm/core/policy.hpp"
#include <catch2/catch_test_macros.hpp>
#include <unordered_set>

using namespace lwm;

namespace {

Workspace make_workspace(std::vector<xcb_window_t> windows, xcb_window_t focused)
{
    Workspace ws;
    ws.windows = std::move(windows);
    ws.focused_window = focused;
    return ws;
}

} // namespace

TEST_CASE(
    "Priority order: current tiled > sticky other tiled > current floating > sticky other floating",
    "[focus][policy][edge]"
)
{
    Workspace ws = make_workspace({ 0x1000 }, 0x1000);

    std::unordered_set<xcb_window_t> eligible_set = { 0x1000, 0x8100, 0x9000, 0x8000 };
    auto eligible = [&](xcb_window_t window) { return eligible_set.contains(window); };

    std::vector<xcb_window_t> sticky_tiled = { 0x8100 };
    std::vector<focus_policy::FloatingCandidate> floating = {
        { 0x9000, 0, 0, false },
        { 0x8000, 0, 1,  true },
    };

    auto selection = focus_policy::select_focus_candidate(ws, 0, 0, sticky_tiled, floating, eligible);

    REQUIRE(selection);
    REQUIRE(selection->window == 0x1000);
    REQUIRE_FALSE(selection->is_floating);
}

TEST_CASE("Empty workspace with sticky tiled and sticky floating on other workspace", "[focus][policy][edge]")
{
    Workspace ws = make_workspace({}, XCB_NONE);

    std::unordered_set<xcb_window_t> eligible_set = { 0x8100, 0x8000 };
    auto eligible = [&](xcb_window_t window) { return eligible_set.contains(window); };

    std::vector<xcb_window_t> sticky_tiled = { 0x8100 };
    std::vector<focus_policy::FloatingCandidate> floating = {
        { 0x8000, 0, 1, true },
    };

    auto selection = focus_policy::select_focus_candidate(ws, 0, 0, sticky_tiled, floating, eligible);

    REQUIRE(selection);
    REQUIRE(selection->window == 0x8100);
    REQUIRE_FALSE(selection->is_floating);
}

TEST_CASE("Empty workspace with only sticky floating on other workspace", "[focus][policy][edge]")
{
    Workspace ws = make_workspace({}, XCB_NONE);

    std::unordered_set<xcb_window_t> eligible_set = { 0x8000 };
    auto eligible = [&](xcb_window_t window) { return eligible_set.contains(window); };

    std::vector<xcb_window_t> sticky_tiled;
    std::vector<focus_policy::FloatingCandidate> floating = {
        { 0x8000, 0, 1, true },
    };

    auto selection = focus_policy::select_focus_candidate(ws, 0, 0, sticky_tiled, floating, eligible);

    REQUIRE(selection);
    REQUIRE(selection->window == 0x8000);
    REQUIRE(selection->is_floating);
}
