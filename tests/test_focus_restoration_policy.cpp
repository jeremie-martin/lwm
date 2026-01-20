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

    std::vector<xcb_window_t> sticky_tiled;
    auto selection = focus_policy::select_focus_candidate(ws, 0, 0, sticky_tiled, floating, eligible);

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

    std::vector<xcb_window_t> sticky_tiled;
    auto selection = focus_policy::select_focus_candidate(ws, 0, 0, sticky_tiled, floating, eligible);

    REQUIRE(selection);
    REQUIRE(selection->window == 0x3000);
    REQUIRE_FALSE(selection->is_floating);
}

TEST_CASE("Focus restoration ignores stale focused window", "[focus][policy]")
{
    Workspace ws = make_workspace({ 0x1000, 0x2000 }, 0x9999);

    std::unordered_set<xcb_window_t> eligible_set = { 0x1000, 0x2000 };
    auto eligible = [&](xcb_window_t window) { return eligible_set.contains(window); };

    std::vector<focus_policy::FloatingCandidate> floating;
    std::vector<xcb_window_t> sticky_tiled;

    auto selection = focus_policy::select_focus_candidate(ws, 0, 0, sticky_tiled, floating, eligible);

    REQUIRE(selection);
    REQUIRE(selection->window == 0x2000);
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

    std::vector<xcb_window_t> sticky_tiled;
    auto selection = focus_policy::select_focus_candidate(ws, 0, 1, sticky_tiled, floating, eligible);

    REQUIRE(selection);
    REQUIRE(selection->window == 0x6000);
    REQUIRE(selection->is_floating);
}

TEST_CASE("Floating candidate on another monitor is ignored even if sticky", "[focus][policy]")
{
    Workspace ws = make_workspace({}, XCB_NONE);

    std::unordered_set<xcb_window_t> eligible_set = { 0x8000 };
    auto eligible = [&](xcb_window_t window) { return eligible_set.contains(window); };

    std::vector<focus_policy::FloatingCandidate> floating = {
        { 0x8000, 1, 0, true },
    };

    std::vector<xcb_window_t> sticky_tiled;
    auto selection = focus_policy::select_focus_candidate(ws, 0, 0, sticky_tiled, floating, eligible);

    REQUIRE_FALSE(selection);
}

TEST_CASE("Sticky tiled candidate is eligible across workspaces", "[focus][policy]")
{
    Workspace ws = make_workspace({}, XCB_NONE);

    std::unordered_set<xcb_window_t> eligible_set = { 0x8100 };
    auto eligible = [&](xcb_window_t window) { return eligible_set.contains(window); };

    std::vector<xcb_window_t> sticky_tiled = { 0x8100 };
    std::vector<focus_policy::FloatingCandidate> floating;

    auto selection = focus_policy::select_focus_candidate(ws, 0, 1, sticky_tiled, floating, eligible);

    REQUIRE(selection);
    REQUIRE(selection->window == 0x8100);
    REQUIRE_FALSE(selection->is_floating);
}

TEST_CASE("Sticky tiled candidates are chosen before floating MRU", "[focus][policy]")
{
    Workspace ws = make_workspace({}, XCB_NONE);

    std::unordered_set<xcb_window_t> eligible_set = { 0x8200, 0x9000 };
    auto eligible = [&](xcb_window_t window) { return eligible_set.contains(window); };

    std::vector<xcb_window_t> sticky_tiled = { 0x8200 };
    std::vector<focus_policy::FloatingCandidate> floating = {
        { 0x9000, 0, 1, false },
    };

    auto selection = focus_policy::select_focus_candidate(ws, 0, 1, sticky_tiled, floating, eligible);

    REQUIRE(selection);
    REQUIRE(selection->window == 0x8200);
    REQUIRE_FALSE(selection->is_floating);
}

TEST_CASE("Current workspace remains preferred over sticky tiled", "[focus][policy]")
{
    Workspace ws = make_workspace({ 0x1000 }, 0x1000);

    std::unordered_set<xcb_window_t> eligible_set = { 0x1000, 0x8300 };
    auto eligible = [&](xcb_window_t window) { return eligible_set.contains(window); };

    std::vector<xcb_window_t> sticky_tiled = { 0x8300 };
    std::vector<focus_policy::FloatingCandidate> floating;

    auto selection = focus_policy::select_focus_candidate(ws, 0, 1, sticky_tiled, floating, eligible);

    REQUIRE(selection);
    REQUIRE(selection->window == 0x1000);
    REQUIRE_FALSE(selection->is_floating);
}

TEST_CASE("Sticky floating candidate is eligible across workspaces", "[focus][policy]")
{
    Workspace ws = make_workspace({}, XCB_NONE);

    std::unordered_set<xcb_window_t> eligible_set = { 0x8000 };
    auto eligible = [&](xcb_window_t window) { return eligible_set.contains(window); };

    std::vector<focus_policy::FloatingCandidate> floating = {
        { 0x8000, 0, 0, true },
    };

    std::vector<xcb_window_t> sticky_tiled;
    auto selection = focus_policy::select_focus_candidate(ws, 0, 1, sticky_tiled, floating, eligible);

    REQUIRE(selection);
    REQUIRE(selection->window == 0x8000);
    REQUIRE(selection->is_floating);
}

TEST_CASE("Non-sticky floating on another workspace is ignored", "[focus][policy]")
{
    Workspace ws = make_workspace({}, XCB_NONE);

    std::unordered_set<xcb_window_t> eligible_set = { 0x9000 };
    auto eligible = [&](xcb_window_t window) { return eligible_set.contains(window); };

    std::vector<focus_policy::FloatingCandidate> floating = {
        { 0x9000, 0, 0, false },
    };

    std::vector<xcb_window_t> sticky_tiled;
    auto selection = focus_policy::select_focus_candidate(ws, 0, 1, sticky_tiled, floating, eligible);

    REQUIRE_FALSE(selection);
}

TEST_CASE("Floating MRU promotion moves item to end", "[focus][policy]")
{
    std::vector<xcb_window_t> items = { 0x1000, 0x2000, 0x3000 };

    bool moved = focus_policy::promote_mru(items, 0x2000, [](xcb_window_t value) { return value; });

    REQUIRE(moved);
    REQUIRE(items == std::vector<xcb_window_t>{ 0x1000, 0x3000, 0x2000 });
}

TEST_CASE("Floating MRU promotion is a no-op for last item", "[focus][policy]")
{
    std::vector<xcb_window_t> items = { 0x1000, 0x2000 };

    bool moved = focus_policy::promote_mru(items, 0x2000, [](xcb_window_t value) { return value; });

    REQUIRE_FALSE(moved);
    REQUIRE(items == std::vector<xcb_window_t>{ 0x1000, 0x2000 });
}

TEST_CASE("Floating MRU promotion ignores missing items", "[focus][policy]")
{
    std::vector<xcb_window_t> items = { 0x1000, 0x2000 };

    bool moved = focus_policy::promote_mru(items, 0x9999, [](xcb_window_t value) { return value; });

    REQUIRE_FALSE(moved);
    REQUIRE(items == std::vector<xcb_window_t>{ 0x1000, 0x2000 });
}

TEST_CASE("Focus restoration returns none when no candidates", "[focus][policy]")
{
    Workspace ws = make_workspace({}, XCB_NONE);

    auto eligible = [](xcb_window_t) { return true; };
    std::vector<focus_policy::FloatingCandidate> floating;

    std::vector<xcb_window_t> sticky_tiled;
    auto selection = focus_policy::select_focus_candidate(ws, 0, 0, sticky_tiled, floating, eligible);

    REQUIRE_FALSE(selection);
}
