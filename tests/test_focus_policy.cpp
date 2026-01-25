#include "lwm/core/focus.hpp"
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

TEST_CASE("Monitor index at point finds correct monitor", "[focus][monitor]")
{
    auto monitors = make_dual_monitors();

    // Point on first monitor
    auto idx0 = focus::monitor_index_at_point(monitors, 500, 500);
    REQUIRE(idx0);
    REQUIRE(*idx0 == 0);

    // Point on second monitor
    auto idx1 = focus::monitor_index_at_point(monitors, 2500, 500);
    REQUIRE(idx1);
    REQUIRE(*idx1 == 1);
}

TEST_CASE("Monitor index at point returns nullopt for out-of-bounds", "[focus][monitor]")
{
    auto monitors = make_dual_monitors();

    // Point outside all monitors
    auto idx = focus::monitor_index_at_point(monitors, 5000, 5000);
    REQUIRE_FALSE(idx);
}

TEST_CASE("Monitor index at point handles monitor boundary exactly", "[focus][monitor]")
{
    auto monitors = make_dual_monitors();

    // Point exactly at boundary between monitors (x=1920)
    // Should be on second monitor (first monitor is 0-1919)
    auto idx = focus::monitor_index_at_point(monitors, 1920, 500);
    REQUIRE(idx);
    REQUIRE(*idx == 1);

    // Point at last pixel of first monitor (x=1919)
    auto idx2 = focus::monitor_index_at_point(monitors, 1919, 500);
    REQUIRE(idx2);
    REQUIRE(*idx2 == 0);
}

TEST_CASE("Monitor index at point handles negative coordinates", "[focus][monitor]")
{
    std::vector<Monitor> monitors;
    monitors.push_back(make_monitor(-1920, 0, 1920, 1080)); // Left of origin
    monitors.push_back(make_monitor(0, 0, 1920, 1080));

    auto idx = focus::monitor_index_at_point(monitors, -500, 500);
    REQUIRE(idx);
    REQUIRE(*idx == 0);
}

TEST_CASE("Monitor index at point with empty monitor list", "[focus][monitor]")
{
    std::vector<Monitor> monitors;

    auto idx = focus::monitor_index_at_point(monitors, 500, 500);
    REQUIRE_FALSE(idx);
}

// ─────────────────────────────────────────────────────────────────────────────
// pointer_move tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("Pointer movement keeps focus on same monitor", "[focus][monitor]")
{
    auto monitors = make_dual_monitors();

    auto result = focus::pointer_move(monitors, 0, 100, 100);
    REQUIRE_FALSE(result.active_monitor_changed);
    REQUIRE_FALSE(result.clear_focus);
}

TEST_CASE("Pointer movement to another monitor clears focus", "[focus][monitor]")
{
    auto monitors = make_dual_monitors();

    auto result = focus::pointer_move(monitors, 0, 2000, 200);
    REQUIRE(result.active_monitor_changed);
    REQUIRE(result.new_monitor == 1);
    REQUIRE(result.clear_focus);
}

TEST_CASE("Pointer movement at monitor edge", "[focus][monitor]")
{
    auto monitors = make_dual_monitors();

    // Just inside first monitor
    auto r1 = focus::pointer_move(monitors, 0, 1919, 500);
    REQUIRE_FALSE(r1.active_monitor_changed);

    // Just inside second monitor (boundary)
    auto r2 = focus::pointer_move(monitors, 0, 1920, 500);
    REQUIRE(r2.active_monitor_changed);
    REQUIRE(r2.new_monitor == 1);
}

TEST_CASE("Pointer movement outside all monitors stays on current", "[focus][monitor]")
{
    auto monitors = make_dual_monitors();

    // Point way outside
    auto result = focus::pointer_move(monitors, 0, 10000, 10000);
    REQUIRE_FALSE(result.active_monitor_changed);
    REQUIRE_FALSE(result.clear_focus);
}

TEST_CASE("Pointer movement from second to first monitor", "[focus][monitor]")
{
    auto monitors = make_dual_monitors();

    auto result = focus::pointer_move(monitors, 1, 500, 500);
    REQUIRE(result.active_monitor_changed);
    REQUIRE(result.new_monitor == 0);
    REQUIRE(result.clear_focus);
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
    // Unknown window in normal case
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

    // Unknown window with empty monitors list
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
