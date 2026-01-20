#include <catch2/catch_test_macros.hpp>
#include "lwm/core/policy.hpp"
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

TEST_CASE("Focus restoration prefers focused tiled window", "[focus][policy]")
{
    Workspace ws = make_workspace({ 0x1000, 0x2000, 0x3000 }, 0x2000);

    std::unordered_set<xcb_window_t> eligible_set = { 0x2000, 0x3000, 0x4000 };
    auto eligible = [&](xcb_window_t window) { return eligible_set.contains(window); };

    std::vector<focus_policy::FloatingCandidate> floating = {
        { 0x4000, 0, 0 },
    };

    auto selection = focus_policy::select_focus_candidate(ws, 0, 0, floating, eligible);

    REQUIRE(selection);
    REQUIRE(selection->window == 0x2000);
    REQUIRE_FALSE(selection->is_floating);
}

TEST_CASE("Focus restoration falls back to last eligible tiled window", "[focus][policy]")
{
    Workspace ws = make_workspace({ 0x1000, 0x2000, 0x3000 }, 0x2000);

    std::unordered_set<xcb_window_t> eligible_set = { 0x1000, 0x3000 };
    auto eligible = [&](xcb_window_t window) { return eligible_set.contains(window); };

    std::vector<focus_policy::FloatingCandidate> floating;

    auto selection = focus_policy::select_focus_candidate(ws, 0, 0, floating, eligible);

    REQUIRE(selection);
    REQUIRE(selection->window == 0x3000);
    REQUIRE_FALSE(selection->is_floating);
}

TEST_CASE("Focus restoration falls back to floating MRU", "[focus][policy]")
{
    Workspace ws = make_workspace({ 0x1000 }, 0x1000);

    std::unordered_set<xcb_window_t> eligible_set = { 0x5000, 0x6000 };
    auto eligible = [&](xcb_window_t window) { return eligible_set.contains(window); };

    std::vector<focus_policy::FloatingCandidate> floating = {
        { 0x5000, 0, 1 },
        { 0x6000, 0, 1 },
        { 0x7000, 1, 0 },
    };

    auto selection = focus_policy::select_focus_candidate(ws, 0, 1, floating, eligible);

    REQUIRE(selection);
    REQUIRE(selection->window == 0x6000);
    REQUIRE(selection->is_floating);
}

TEST_CASE("Focus restoration returns none when no candidates", "[focus][policy]")
{
    Workspace ws = make_workspace({}, XCB_NONE);

    auto eligible = [](xcb_window_t) { return true; };
    std::vector<focus_policy::FloatingCandidate> floating;

    auto selection = focus_policy::select_focus_candidate(ws, 0, 0, floating, eligible);

    REQUIRE_FALSE(selection);
}
