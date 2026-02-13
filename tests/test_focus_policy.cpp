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

TEST_CASE("Fullscreen reapply policy excludes focus-only transitions", "[fullscreen][policy]")
{
    using fullscreen_policy::ApplyContext;

    REQUIRE_FALSE(fullscreen_policy::should_reapply(ApplyContext::FocusTransition));
    REQUIRE(fullscreen_policy::should_reapply(ApplyContext::StateTransition));
    REQUIRE(fullscreen_policy::should_reapply(ApplyContext::VisibilityTransition));
    REQUIRE(fullscreen_policy::should_reapply(ApplyContext::LayoutTransition));
    REQUIRE(fullscreen_policy::should_reapply(ApplyContext::ConfigureTransition));
}

// ─────────────────────────────────────────────────────────────────────────────
// focus_window_state tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("Focusing a window updates active monitor and workspace", "[focus][window]")
{
    auto monitors = make_dual_monitors();
    monitors[1].current_workspace = 1;
    monitors[1].workspaces[2].windows.push_back(0x2000);

    size_t active_monitor = 0;
    xcb_window_t active_window = XCB_NONE;

    auto change = focus::focus_window_state(monitors, active_monitor, active_window, 0x2000);
    REQUIRE(change);
    REQUIRE(change->target_monitor == 1);
    REQUIRE(change->workspace_changed);
    REQUIRE(change->old_workspace == 1);
    REQUIRE(change->new_workspace == 2);
    REQUIRE(monitors[1].previous_workspace == 1);
    REQUIRE(active_monitor == 1);
    REQUIRE(active_window == 0x2000);
    REQUIRE(monitors[1].current_workspace == 2);
    REQUIRE(monitors[1].current().focused_window == 0x2000);
}

TEST_CASE("Focusing unknown or non-existent windows preserves state", "[focus][window]")
{
    SECTION("Unknown window in normal case")
    {
        std::vector<Monitor> monitors;
        monitors.push_back(make_monitor(0, 0, 1920, 1080));

        size_t active_monitor = 0;
        xcb_window_t active_window = 0x1234;

        auto change = focus::focus_window_state(monitors, active_monitor, active_window, 0x9999);
        REQUIRE_FALSE(change);
        REQUIRE(active_monitor == 0);
        REQUIRE(active_window == 0x1234);
    }

    SECTION("Unknown window with empty monitors list")
    {
        std::vector<Monitor> monitors;

        size_t active_monitor = 0;
        xcb_window_t active_window = 0x1234;

        auto change = focus::focus_window_state(monitors, active_monitor, active_window, 0x1000);
        REQUIRE_FALSE(change);
        REQUIRE(active_window == 0x1234);
    }
}

TEST_CASE("Focusing window on same workspace does not change workspace", "[focus][window]")
{
    std::vector<Monitor> monitors;
    monitors.push_back(make_monitor(0, 0, 1920, 1080));
    monitors[0].current_workspace = 0;
    monitors[0].workspaces[0].windows = { 0x1000, 0x2000 };

    size_t active_monitor = 0;
    xcb_window_t active_window = 0x1000;

    auto change = focus::focus_window_state(monitors, active_monitor, active_window, 0x2000);
    REQUIRE(change);
    REQUIRE(change->target_monitor == 0);
    REQUIRE_FALSE(change->workspace_changed);
    REQUIRE(active_window == 0x2000);
    REQUIRE(monitors[0].current_workspace == 0);
}

TEST_CASE("Focusing window updates focused_window in workspace", "[focus][window]")
{
    std::vector<Monitor> monitors;
    monitors.push_back(make_monitor(0, 0, 1920, 1080));
    monitors[0].workspaces[0].windows = { 0x1000, 0x2000 };
    monitors[0].workspaces[0].focused_window = 0x1000;

    size_t active_monitor = 0;
    xcb_window_t active_window = 0x1000;

    auto change = focus::focus_window_state(monitors, active_monitor, active_window, 0x2000);
    REQUIRE(change);
    REQUIRE(monitors[0].workspaces[0].focused_window == 0x2000);
}

TEST_CASE("Focusing window changes workspace and updates previous_workspace", "[focus][window]")
{
    std::vector<Monitor> monitors;
    monitors.push_back(make_monitor(0, 0, 1920, 1080));
    monitors[0].current_workspace = 0;
    monitors[0].previous_workspace = 5; // Some previous value
    monitors[0].workspaces[3].windows.push_back(0x3000);

    size_t active_monitor = 0;
    xcb_window_t active_window = XCB_NONE;

    auto change = focus::focus_window_state(monitors, active_monitor, active_window, 0x3000);
    REQUIRE(change);
    REQUIRE(change->workspace_changed);
    REQUIRE(change->old_workspace == 0);
    REQUIRE(change->new_workspace == 3);
    REQUIRE(monitors[0].previous_workspace == 0); // Updated to old current
    REQUIRE(monitors[0].current_workspace == 3);
}

// ─────────────────────────────────────────────────────────────────────────────
// Sticky window focus tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("Focusing sticky window does not change workspace", "[focus][window][sticky]")
{
    std::vector<Monitor> monitors;
    monitors.push_back(make_monitor(0, 0, 1920, 1080));
    monitors[0].current_workspace = 0;
    // Window is on workspace 3 but is sticky
    monitors[0].workspaces[3].windows.push_back(0x1000);

    size_t active_monitor = 0;
    xcb_window_t active_window = XCB_NONE;

    // Focus with is_sticky=true
    auto change = focus::focus_window_state(monitors, active_monitor, active_window, 0x1000, true);
    REQUIRE(change);
    REQUIRE(change->target_monitor == 0);
    // Workspace should NOT change for sticky windows
    REQUIRE_FALSE(change->workspace_changed);
    REQUIRE(monitors[0].current_workspace == 0); // Still on workspace 0
    REQUIRE(active_window == 0x1000);
}

TEST_CASE("Focusing non-sticky window on different workspace does change workspace", "[focus][window][sticky]")
{
    std::vector<Monitor> monitors;
    monitors.push_back(make_monitor(0, 0, 1920, 1080));
    monitors[0].current_workspace = 0;
    monitors[0].workspaces[3].windows.push_back(0x1000);

    size_t active_monitor = 0;
    xcb_window_t active_window = XCB_NONE;

    // Focus with is_sticky=false (default)
    auto change = focus::focus_window_state(monitors, active_monitor, active_window, 0x1000, false);
    REQUIRE(change);
    REQUIRE(change->workspace_changed);
    REQUIRE(monitors[0].current_workspace == 3);
}

// ─────────────────────────────────────────────────────────────────────────────
// Edge cases
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("Multiple windows on workspace can be focused in sequence", "[focus][window]")
{
    std::vector<Monitor> monitors;
    monitors.push_back(make_monitor(0, 0, 1920, 1080));
    monitors[0].workspaces[0].windows = { 0x1000, 0x2000, 0x3000 };

    size_t active_monitor = 0;
    xcb_window_t active_window = XCB_NONE;

    // Focus first window
    auto c1 = focus::focus_window_state(monitors, active_monitor, active_window, 0x1000);
    REQUIRE(c1);
    REQUIRE(active_window == 0x1000);

    // Focus second window
    auto c2 = focus::focus_window_state(monitors, active_monitor, active_window, 0x2000);
    REQUIRE(c2);
    REQUIRE(active_window == 0x2000);

    // Focus third window
    auto c3 = focus::focus_window_state(monitors, active_monitor, active_window, 0x3000);
    REQUIRE(c3);
    REQUIRE(active_window == 0x3000);

    // Focus first window again
    auto c4 = focus::focus_window_state(monitors, active_monitor, active_window, 0x1000);
    REQUIRE(c4);
    REQUIRE(active_window == 0x1000);
}
