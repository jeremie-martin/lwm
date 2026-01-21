#include <catch2/catch_test_macros.hpp>
#include "lwm/core/policy.hpp"
#include <unordered_set>

using namespace lwm;

namespace {

focus_policy::FocusCycleCandidate make_tiled(xcb_window_t id)
{
    return { id, false };
}

focus_policy::FocusCycleCandidate make_floating(xcb_window_t id)
{
    return { id, true };
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// cycle_focus_next tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("Focus next cycles forward through windows", "[focus][cycling]")
{
    std::vector<focus_policy::FocusCycleCandidate> candidates = {
        make_tiled(0x1000),
        make_tiled(0x2000),
        make_tiled(0x3000),
    };

    auto result = focus_policy::cycle_focus_next(candidates, 0x1000);
    REQUIRE(result);
    REQUIRE(result->id == 0x2000);
    REQUIRE_FALSE(result->is_floating);
}

TEST_CASE("Focus next wraps from last to first", "[focus][cycling]")
{
    std::vector<focus_policy::FocusCycleCandidate> candidates = {
        make_tiled(0x1000),
        make_tiled(0x2000),
        make_tiled(0x3000),
    };

    auto result = focus_policy::cycle_focus_next(candidates, 0x3000);
    REQUIRE(result);
    REQUIRE(result->id == 0x1000);
}

TEST_CASE("Focus next with single window returns same window", "[focus][cycling]")
{
    std::vector<focus_policy::FocusCycleCandidate> candidates = {
        make_tiled(0x1000),
    };

    auto result = focus_policy::cycle_focus_next(candidates, 0x1000);
    REQUIRE(result);
    REQUIRE(result->id == 0x1000);
}

TEST_CASE("Focus next with empty candidates returns nullopt", "[focus][cycling]")
{
    std::vector<focus_policy::FocusCycleCandidate> candidates;

    auto result = focus_policy::cycle_focus_next(candidates, 0x1000);
    REQUIRE_FALSE(result);
}

TEST_CASE("Focus next with unknown current starts from first", "[focus][cycling]")
{
    std::vector<focus_policy::FocusCycleCandidate> candidates = {
        make_tiled(0x1000),
        make_tiled(0x2000),
    };

    // Current window 0x9999 not in list, so starts at index 0 and moves to next
    auto result = focus_policy::cycle_focus_next(candidates, 0x9999);
    REQUIRE(result);
    REQUIRE(result->id == 0x2000);
}

TEST_CASE("Focus next includes floating windows after tiled", "[focus][cycling]")
{
    std::vector<focus_policy::FocusCycleCandidate> candidates = {
        make_tiled(0x1000),
        make_tiled(0x2000),
        make_floating(0x3000),
    };

    auto result = focus_policy::cycle_focus_next(candidates, 0x2000);
    REQUIRE(result);
    REQUIRE(result->id == 0x3000);
    REQUIRE(result->is_floating);
}

TEST_CASE("Focus next from floating wraps to tiled", "[focus][cycling]")
{
    std::vector<focus_policy::FocusCycleCandidate> candidates = {
        make_tiled(0x1000),
        make_floating(0x2000),
    };

    auto result = focus_policy::cycle_focus_next(candidates, 0x2000);
    REQUIRE(result);
    REQUIRE(result->id == 0x1000);
    REQUIRE_FALSE(result->is_floating);
}

// ─────────────────────────────────────────────────────────────────────────────
// cycle_focus_prev tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("Focus prev cycles backward through windows", "[focus][cycling]")
{
    std::vector<focus_policy::FocusCycleCandidate> candidates = {
        make_tiled(0x1000),
        make_tiled(0x2000),
        make_tiled(0x3000),
    };

    auto result = focus_policy::cycle_focus_prev(candidates, 0x3000);
    REQUIRE(result);
    REQUIRE(result->id == 0x2000);
}

TEST_CASE("Focus prev wraps from first to last", "[focus][cycling]")
{
    std::vector<focus_policy::FocusCycleCandidate> candidates = {
        make_tiled(0x1000),
        make_tiled(0x2000),
        make_tiled(0x3000),
    };

    auto result = focus_policy::cycle_focus_prev(candidates, 0x1000);
    REQUIRE(result);
    REQUIRE(result->id == 0x3000);
}

TEST_CASE("Focus prev with single window returns same window", "[focus][cycling]")
{
    std::vector<focus_policy::FocusCycleCandidate> candidates = {
        make_tiled(0x1000),
    };

    auto result = focus_policy::cycle_focus_prev(candidates, 0x1000);
    REQUIRE(result);
    REQUIRE(result->id == 0x1000);
}

TEST_CASE("Focus prev with empty candidates returns nullopt", "[focus][cycling]")
{
    std::vector<focus_policy::FocusCycleCandidate> candidates;

    auto result = focus_policy::cycle_focus_prev(candidates, 0x1000);
    REQUIRE_FALSE(result);
}

TEST_CASE("Focus prev with unknown current starts from first going back", "[focus][cycling]")
{
    std::vector<focus_policy::FocusCycleCandidate> candidates = {
        make_tiled(0x1000),
        make_tiled(0x2000),
    };

    // Current window 0x9999 not in list, so starts at index 0 and goes to last
    auto result = focus_policy::cycle_focus_prev(candidates, 0x9999);
    REQUIRE(result);
    REQUIRE(result->id == 0x2000);
}

TEST_CASE("Focus prev from tiled wraps to floating", "[focus][cycling]")
{
    std::vector<focus_policy::FocusCycleCandidate> candidates = {
        make_tiled(0x1000),
        make_floating(0x2000),
    };

    auto result = focus_policy::cycle_focus_prev(candidates, 0x1000);
    REQUIRE(result);
    REQUIRE(result->id == 0x2000);
    REQUIRE(result->is_floating);
}

// ─────────────────────────────────────────────────────────────────────────────
// build_cycle_candidates tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("Build candidates includes eligible tiled windows", "[focus][cycling]")
{
    std::vector<xcb_window_t> tiled = { 0x1000, 0x2000, 0x3000 };
    std::vector<focus_policy::FloatingCandidate> floating;

    std::unordered_set<xcb_window_t> eligible_set = { 0x1000, 0x3000 };
    auto is_eligible = [&](xcb_window_t w) { return eligible_set.contains(w); };

    auto candidates = focus_policy::build_cycle_candidates(tiled, floating, 0, 0, is_eligible);

    REQUIRE(candidates.size() == 2);
    REQUIRE(candidates[0].id == 0x1000);
    REQUIRE(candidates[1].id == 0x3000);
}

TEST_CASE("Build candidates includes floating on same workspace", "[focus][cycling]")
{
    std::vector<xcb_window_t> tiled = { 0x1000 };
    std::vector<focus_policy::FloatingCandidate> floating = {
        { 0x2000, 0, 0, false },  // Same monitor, same workspace
        { 0x3000, 0, 1, false },  // Same monitor, different workspace
    };

    auto is_eligible = [](xcb_window_t) { return true; };

    auto candidates = focus_policy::build_cycle_candidates(tiled, floating, 0, 0, is_eligible);

    REQUIRE(candidates.size() == 2);
    REQUIRE(candidates[0].id == 0x1000);
    REQUIRE(candidates[1].id == 0x2000);
}

TEST_CASE("Build candidates excludes floating on different monitor", "[focus][cycling]")
{
    std::vector<xcb_window_t> tiled;
    std::vector<focus_policy::FloatingCandidate> floating = {
        { 0x1000, 0, 0, false },  // Same monitor
        { 0x2000, 1, 0, false },  // Different monitor
    };

    auto is_eligible = [](xcb_window_t) { return true; };

    auto candidates = focus_policy::build_cycle_candidates(tiled, floating, 0, 0, is_eligible);

    REQUIRE(candidates.size() == 1);
    REQUIRE(candidates[0].id == 0x1000);
}

TEST_CASE("Build candidates includes sticky floating on different workspace", "[focus][cycling]")
{
    std::vector<xcb_window_t> tiled;
    std::vector<focus_policy::FloatingCandidate> floating = {
        { 0x1000, 0, 1, true },  // Sticky, different workspace
    };

    auto is_eligible = [](xcb_window_t) { return true; };

    auto candidates = focus_policy::build_cycle_candidates(tiled, floating, 0, 0, is_eligible);

    REQUIRE(candidates.size() == 1);
    REQUIRE(candidates[0].id == 0x1000);
    REQUIRE(candidates[0].is_floating);
}

TEST_CASE("Build candidates excludes sticky floating on different monitor", "[focus][cycling]")
{
    std::vector<xcb_window_t> tiled;
    std::vector<focus_policy::FloatingCandidate> floating = {
        { 0x1000, 1, 0, true },  // Sticky but different monitor
    };

    auto is_eligible = [](xcb_window_t) { return true; };

    auto candidates = focus_policy::build_cycle_candidates(tiled, floating, 0, 0, is_eligible);

    REQUIRE(candidates.empty());
}

TEST_CASE("Build candidates preserves tiled-then-floating order", "[focus][cycling]")
{
    std::vector<xcb_window_t> tiled = { 0x1000, 0x2000 };
    std::vector<focus_policy::FloatingCandidate> floating = {
        { 0x3000, 0, 0, false },
        { 0x4000, 0, 0, false },
    };

    auto is_eligible = [](xcb_window_t) { return true; };

    auto candidates = focus_policy::build_cycle_candidates(tiled, floating, 0, 0, is_eligible);

    REQUIRE(candidates.size() == 4);
    REQUIRE(candidates[0].id == 0x1000);
    REQUIRE_FALSE(candidates[0].is_floating);
    REQUIRE(candidates[1].id == 0x2000);
    REQUIRE_FALSE(candidates[1].is_floating);
    REQUIRE(candidates[2].id == 0x3000);
    REQUIRE(candidates[2].is_floating);
    REQUIRE(candidates[3].id == 0x4000);
    REQUIRE(candidates[3].is_floating);
}

TEST_CASE("Build candidates returns empty when all ineligible", "[focus][cycling]")
{
    std::vector<xcb_window_t> tiled = { 0x1000, 0x2000 };
    std::vector<focus_policy::FloatingCandidate> floating = {
        { 0x3000, 0, 0, false },
    };

    auto is_eligible = [](xcb_window_t) { return false; };

    auto candidates = focus_policy::build_cycle_candidates(tiled, floating, 0, 0, is_eligible);

    REQUIRE(candidates.empty());
}

// ─────────────────────────────────────────────────────────────────────────────
// Integration-style tests combining build + cycle
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("Full cycle through mixed tiled and floating", "[focus][cycling][integration]")
{
    std::vector<xcb_window_t> tiled = { 0x1000, 0x2000 };
    std::vector<focus_policy::FloatingCandidate> floating = {
        { 0x3000, 0, 0, false },
    };

    auto is_eligible = [](xcb_window_t) { return true; };

    auto candidates = focus_policy::build_cycle_candidates(tiled, floating, 0, 0, is_eligible);
    REQUIRE(candidates.size() == 3);

    // Start at first tiled
    auto r1 = focus_policy::cycle_focus_next(candidates, 0x1000);
    REQUIRE(r1);
    REQUIRE(r1->id == 0x2000);

    // Next is floating
    auto r2 = focus_policy::cycle_focus_next(candidates, 0x2000);
    REQUIRE(r2);
    REQUIRE(r2->id == 0x3000);
    REQUIRE(r2->is_floating);

    // Wrap to first tiled
    auto r3 = focus_policy::cycle_focus_next(candidates, 0x3000);
    REQUIRE(r3);
    REQUIRE(r3->id == 0x1000);
    REQUIRE_FALSE(r3->is_floating);
}

TEST_CASE("Full reverse cycle through mixed tiled and floating", "[focus][cycling][integration]")
{
    std::vector<xcb_window_t> tiled = { 0x1000, 0x2000 };
    std::vector<focus_policy::FloatingCandidate> floating = {
        { 0x3000, 0, 0, false },
    };

    auto is_eligible = [](xcb_window_t) { return true; };

    auto candidates = focus_policy::build_cycle_candidates(tiled, floating, 0, 0, is_eligible);
    REQUIRE(candidates.size() == 3);

    // Start at first tiled, go back to floating (wrap)
    auto r1 = focus_policy::cycle_focus_prev(candidates, 0x1000);
    REQUIRE(r1);
    REQUIRE(r1->id == 0x3000);
    REQUIRE(r1->is_floating);

    // Prev is second tiled
    auto r2 = focus_policy::cycle_focus_prev(candidates, 0x3000);
    REQUIRE(r2);
    REQUIRE(r2->id == 0x2000);

    // Prev is first tiled
    auto r3 = focus_policy::cycle_focus_prev(candidates, 0x2000);
    REQUIRE(r3);
    REQUIRE(r3->id == 0x1000);
}
