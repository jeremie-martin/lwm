#include "lwm/core/policy.hpp"
#include <catch2/catch_test_macros.hpp>
#include <unordered_set>

using namespace lwm;

namespace {

focus_policy::FocusCycleCandidate make_tiled(xcb_window_t id) { return { id, false }; }

focus_policy::FocusCycleCandidate make_floating(xcb_window_t id) { return { id, true }; }

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// cycle_focus tests (next and prev)
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("Focus next cycles forward through windows", "[focus][cycling]")
{
    std::vector<focus_policy::FocusCycleCandidate> candidates = {
        make_tiled(0x1000),
        make_tiled(0x2000),
        make_tiled(0x3000),
    };

    // Basic forward cycling
    auto result1 = focus_policy::cycle_focus_next(candidates, 0x1000);
    REQUIRE(result1);
    REQUIRE(result1->id == 0x2000);

    // Wraps from last to first
    auto result2 = focus_policy::cycle_focus_next(candidates, 0x3000);
    REQUIRE(result2);
    REQUIRE(result2->id == 0x1000);

    // Single window returns same
    {
        std::vector<focus_policy::FocusCycleCandidate> single = { make_tiled(0x1000) };
        auto result3 = focus_policy::cycle_focus_next(single, 0x1000);
        REQUIRE(result3);
        REQUIRE(result3->id == 0x1000);
    }

    // Empty returns nullopt
    {
        std::vector<focus_policy::FocusCycleCandidate> empty;
        auto result4 = focus_policy::cycle_focus_next(empty, 0x1000);
        REQUIRE_FALSE(result4);
    }

    // Unknown current starts from first
    {
        auto result5 = focus_policy::cycle_focus_next(candidates, 0x9999);
        REQUIRE(result5);
        REQUIRE(result5->id == 0x2000);
    }
}

TEST_CASE("Focus prev cycles backward through windows", "[focus][cycling]")
{
    std::vector<focus_policy::FocusCycleCandidate> candidates = {
        make_tiled(0x1000),
        make_tiled(0x2000),
        make_tiled(0x3000),
    };

    // Basic backward cycling
    auto result1 = focus_policy::cycle_focus_prev(candidates, 0x3000);
    REQUIRE(result1);
    REQUIRE(result1->id == 0x2000);

    // Wraps from first to last
    auto result2 = focus_policy::cycle_focus_prev(candidates, 0x1000);
    REQUIRE(result2);
    REQUIRE(result2->id == 0x3000);

    // Single window returns same
    {
        std::vector<focus_policy::FocusCycleCandidate> single = { make_tiled(0x1000) };
        auto result3 = focus_policy::cycle_focus_prev(single, 0x1000);
        REQUIRE(result3);
        REQUIRE(result3->id == 0x1000);
    }

    // Empty returns nullopt
    {
        std::vector<focus_policy::FocusCycleCandidate> empty;
        auto result4 = focus_policy::cycle_focus_prev(empty, 0x1000);
        REQUIRE_FALSE(result4);
    }

    // Unknown current starts from first going back (wraps to last)
    {
        auto result5 = focus_policy::cycle_focus_prev(candidates, 0x9999);
        REQUIRE(result5);
        REQUIRE(result5->id == 0x3000);
    }
}

TEST_CASE("Focus cycling includes floating windows", "[focus][cycling]")
{
    // Next: tiled -> tiled -> floating
    {
        std::vector<focus_policy::FocusCycleCandidate> candidates = {
            make_tiled(0x1000),
            make_tiled(0x2000),
            make_floating(0x3000),
        };
        auto next = focus_policy::cycle_focus_next(candidates, 0x2000);
        REQUIRE(next);
        REQUIRE(next->id == 0x3000);
        REQUIRE(next->is_floating);
    }

    // Next: floating wraps to tiled
    {
        std::vector<focus_policy::FocusCycleCandidate> simple = {
            make_tiled(0x1000),
            make_floating(0x2000),
        };
        auto next_wrap = focus_policy::cycle_focus_next(simple, 0x2000);
        REQUIRE(next_wrap);
        REQUIRE(next_wrap->id == 0x1000);
        REQUIRE_FALSE(next_wrap->is_floating);

        // Prev: tiled wraps to floating
        auto prev = focus_policy::cycle_focus_prev(simple, 0x1000);
        REQUIRE(prev);
        REQUIRE(prev->id == 0x2000);
        REQUIRE(prev->is_floating);
    }
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
        { 0x2000, 0, 0, false }, // Same monitor, same workspace
        { 0x3000, 0, 1, false }, // Same monitor, different workspace
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
        { 0x1000, 0, 0, false }, // Same monitor
        { 0x2000, 1, 0, false }, // Different monitor
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
        { 0x1000, 0, 1, true }, // Sticky, different workspace
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
        { 0x1000, 1, 0, true }, // Sticky but different monitor
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

TEST_CASE("Full cycle through mixed tiled and floating in both directions", "[focus][cycling][integration]")
{
    std::vector<xcb_window_t> tiled = { 0x1000, 0x2000 };
    std::vector<focus_policy::FloatingCandidate> floating = {
        { 0x3000, 0, 0, false },
    };

    auto is_eligible = [](xcb_window_t) { return true; };
    auto candidates = focus_policy::build_cycle_candidates(tiled, floating, 0, 0, is_eligible);
    REQUIRE(candidates.size() == 3);

    // Forward cycle: 0x1000 -> 0x2000 -> 0x3000 -> 0x1000
    SECTION("Forward direction cycles correctly")
    {
        auto r1 = focus_policy::cycle_focus_next(candidates, 0x1000);
        REQUIRE(r1);
        REQUIRE(r1->id == 0x2000);

        auto r2 = focus_policy::cycle_focus_next(candidates, 0x2000);
        REQUIRE(r2);
        REQUIRE(r2->id == 0x3000);
        REQUIRE(r2->is_floating);

        auto r3 = focus_policy::cycle_focus_next(candidates, 0x3000);
        REQUIRE(r3);
        REQUIRE(r3->id == 0x1000);
        REQUIRE_FALSE(r3->is_floating);
    }

    // Backward cycle: 0x1000 -> 0x3000 -> 0x2000 -> 0x1000
    SECTION("Backward direction cycles correctly")
    {
        auto r1 = focus_policy::cycle_focus_prev(candidates, 0x1000);
        REQUIRE(r1);
        REQUIRE(r1->id == 0x3000);
        REQUIRE(r1->is_floating);

        auto r2 = focus_policy::cycle_focus_prev(candidates, 0x3000);
        REQUIRE(r2);
        REQUIRE(r2->id == 0x2000);

        auto r3 = focus_policy::cycle_focus_prev(candidates, 0x2000);
        REQUIRE(r3);
        REQUIRE(r3->id == 0x1000);
    }
}
