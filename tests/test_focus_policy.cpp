#include "lwm/core/focus.hpp"
#include "lwm/core/policy.hpp"
#include <catch2/catch_test_macros.hpp>

using namespace lwm;

namespace {

Monitor make_monitor(int16_t x, int16_t y, uint16_t width, uint16_t height, size_t workspaces = 10)
{
    Monitor monitor;
    monitor.x = x;
    monitor.y = y;
    monitor.width = width;
    monitor.height = height;
    monitor.workspaces.assign(workspaces, Workspace{});
    monitor.current_workspace = 0;
    return monitor;
}

std::vector<Monitor> make_dual_monitors()
{
    std::vector<Monitor> monitors;
    monitors.push_back(make_monitor(0, 0, 1920, 1080));
    monitors.push_back(make_monitor(1920, 0, 1920, 1080));
    return monitors;
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// monitor_index_at_point tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("Monitor index at point finds correct monitor and handles edge cases", "[focus][monitor]")
{
    SECTION("Finds monitor containing point")
    {
        auto monitors = make_dual_monitors();

        auto idx0 = focus::monitor_index_at_point(monitors, 500, 500);
        REQUIRE(idx0);
        REQUIRE(*idx0 == 0);

        auto idx1 = focus::monitor_index_at_point(monitors, 2500, 500);
        REQUIRE(idx1);
        REQUIRE(*idx1 == 1);
    }

    SECTION("Returns nullopt for out-of-bounds or empty list")
    {
        auto monitors = make_dual_monitors();

        auto out_of_bounds = focus::monitor_index_at_point(monitors, 5000, 5000);
        REQUIRE_FALSE(out_of_bounds);

        std::vector<Monitor> empty;
        auto empty_result = focus::monitor_index_at_point(empty, 500, 500);
        REQUIRE_FALSE(empty_result);
    }

    SECTION("Handles monitor boundaries and negative coordinates")
    {
        auto monitors = make_dual_monitors();

        auto boundary_right = focus::monitor_index_at_point(monitors, 1920, 500);
        REQUIRE(boundary_right);
        REQUIRE(*boundary_right == 1);

        auto boundary_left = focus::monitor_index_at_point(monitors, 1919, 500);
        REQUIRE(boundary_left);
        REQUIRE(*boundary_left == 0);

        std::vector<Monitor> neg_monitors;
        neg_monitors.push_back(make_monitor(-1920, 0, 1920, 1080));
        neg_monitors.push_back(make_monitor(0, 0, 1920, 1080));

        auto neg_idx = focus::monitor_index_at_point(neg_monitors, -500, 500);
        REQUIRE(neg_idx);
        REQUIRE(*neg_idx == 0);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// pointer_move tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("Pointer movement handles monitor transitions, boundaries, and edge cases", "[focus][monitor]")
{
    auto monitors = make_dual_monitors();

    SECTION("Keeps focus on same monitor and handles out-of-bounds")
    {
        auto same_monitor = focus::pointer_move(monitors, 0, 100, 100);
        REQUIRE_FALSE(same_monitor.monitor_changed());
        REQUIRE_FALSE(same_monitor.clears_focus());
        REQUIRE(same_monitor.transition == focus::PointerTransition::None);

        auto out_of_bounds = focus::pointer_move(monitors, 0, 10000, 10000);
        REQUIRE_FALSE(out_of_bounds.monitor_changed());
        REQUIRE_FALSE(out_of_bounds.clears_focus());
        REQUIRE(out_of_bounds.transition == focus::PointerTransition::None);
    }

    SECTION("Moves to another monitor clears focus")
    {
        auto to_second = focus::pointer_move(monitors, 0, 2000, 200);
        REQUIRE(to_second.monitor_changed());
        REQUIRE(to_second.transition == focus::PointerTransition::MonitorChangedClearFocus);
        REQUIRE(to_second.new_monitor == 1);
        REQUIRE(to_second.clears_focus());

        auto to_first = focus::pointer_move(monitors, 1, 500, 500);
        REQUIRE(to_first.monitor_changed());
        REQUIRE(to_first.transition == focus::PointerTransition::MonitorChangedClearFocus);
        REQUIRE(to_first.new_monitor == 0);
        REQUIRE(to_first.clears_focus());
    }

    SECTION("Handles monitor edge boundary (1919 vs 1920)")
    {
        auto left_edge = focus::pointer_move(monitors, 0, 1919, 500);
        REQUIRE_FALSE(left_edge.monitor_changed());

        auto right_edge = focus::pointer_move(monitors, 0, 1920, 500);
        REQUIRE(right_edge.monitor_changed());
        REQUIRE(right_edge.transition == focus::PointerTransition::MonitorChangedClearFocus);
        REQUIRE(right_edge.new_monitor == 1);
    }
}

TEST_CASE("Focus border policy skips fullscreen windows", "[focus][policy]")
{
    REQUIRE(focus_policy::should_apply_focus_border(false));
    REQUIRE_FALSE(focus_policy::should_apply_focus_border(true));
}
