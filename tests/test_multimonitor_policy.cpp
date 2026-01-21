#include <catch2/catch_test_macros.hpp>
#include "lwm/core/policy.hpp"
#include "lwm/core/types.hpp"
#include "lwm/core/focus.hpp"

using namespace lwm;

namespace {

Monitor make_monitor(int16_t x, int16_t y, uint16_t width, uint16_t height, size_t workspaces = 10)
{
    Monitor mon;
    mon.x = x;
    mon.y = y;
    mon.width = width;
    mon.height = height;
    mon.workspaces.assign(workspaces, Workspace{});
    mon.current_workspace = 0;
    return mon;
}

std::vector<Monitor> make_dual_monitors()
{
    std::vector<Monitor> monitors;
    monitors.push_back(make_monitor(0, 0, 1920, 1080));
    monitors.push_back(make_monitor(1920, 0, 1920, 1080));
    return monitors;
}

std::vector<Monitor> make_triple_monitors_horizontal()
{
    std::vector<Monitor> monitors;
    monitors.push_back(make_monitor(0, 0, 1920, 1080));
    monitors.push_back(make_monitor(1920, 0, 1920, 1080));
    monitors.push_back(make_monitor(3840, 0, 1920, 1080));
    return monitors;
}

std::vector<Monitor> make_stacked_monitors()
{
    std::vector<Monitor> monitors;
    monitors.push_back(make_monitor(0, 0, 1920, 1080));      // Top
    monitors.push_back(make_monitor(0, 1080, 1920, 1080));   // Bottom
    return monitors;
}

std::vector<Monitor> make_mixed_resolution_monitors()
{
    std::vector<Monitor> monitors;
    monitors.push_back(make_monitor(0, 0, 1920, 1080));       // 1080p
    monitors.push_back(make_monitor(1920, 0, 2560, 1440));    // 1440p
    monitors.push_back(make_monitor(4480, 0, 3840, 2160));    // 4K
    return monitors;
}

std::vector<Monitor> make_offset_monitors()
{
    std::vector<Monitor> monitors;
    // Primary monitor at origin
    monitors.push_back(make_monitor(0, 0, 1920, 1080));
    // Second monitor to the left (negative x)
    monitors.push_back(make_monitor(-1920, 0, 1920, 1080));
    // Third monitor above (negative y)
    monitors.push_back(make_monitor(0, -1080, 1920, 1080));
    return monitors;
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// Monitor geometry tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("Monitor geometry is correctly stored", "[multimonitor][geometry]")
{
    auto monitors = make_dual_monitors();

    REQUIRE(monitors[0].x == 0);
    REQUIRE(monitors[0].y == 0);
    REQUIRE(monitors[0].width == 1920);
    REQUIRE(monitors[0].height == 1080);

    REQUIRE(monitors[1].x == 1920);
    REQUIRE(monitors[1].y == 0);
    REQUIRE(monitors[1].width == 1920);
    REQUIRE(monitors[1].height == 1080);
}

TEST_CASE("Monitor working area calculation with struts", "[multimonitor][geometry]")
{
    auto monitors = make_dual_monitors();

    // Add strut to first monitor (e.g., panel at top)
    monitors[0].strut.top = 30;
    auto area0 = monitors[0].working_area();
    REQUIRE(area0.x == 0);
    REQUIRE(area0.y == 30);
    REQUIRE(area0.width == 1920);
    REQUIRE(area0.height == 1050);

    // Add strut to second monitor (e.g., dock at left)
    monitors[1].strut.left = 50;
    auto area1 = monitors[1].working_area();
    REQUIRE(area1.x == 1920 + 50);
    REQUIRE(area1.y == 0);
    REQUIRE(area1.width == 1920 - 50);
    REQUIRE(area1.height == 1080);
}

TEST_CASE("Mixed resolution monitors have independent working areas", "[multimonitor][geometry]")
{
    auto monitors = make_mixed_resolution_monitors();

    auto area0 = monitors[0].working_area();
    auto area1 = monitors[1].working_area();
    auto area2 = monitors[2].working_area();

    REQUIRE(area0.width == 1920);
    REQUIRE(area0.height == 1080);

    REQUIRE(area1.width == 2560);
    REQUIRE(area1.height == 1440);

    REQUIRE(area2.width == 3840);
    REQUIRE(area2.height == 2160);
}

// ─────────────────────────────────────────────────────────────────────────────
// Pointer movement across monitors
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("Pointer within monitor does not change active monitor", "[multimonitor][focus]")
{
    auto monitors = make_dual_monitors();

    // Pointer within first monitor
    auto result = focus::pointer_move(monitors, 0, 500, 500);
    REQUIRE_FALSE(result.active_monitor_changed);
    REQUIRE_FALSE(result.clear_focus);
}

TEST_CASE("Pointer crossing to second monitor changes active monitor", "[multimonitor][focus]")
{
    auto monitors = make_dual_monitors();

    // Pointer crosses from first to second monitor
    auto result = focus::pointer_move(monitors, 0, 2000, 500);
    REQUIRE(result.active_monitor_changed);
    REQUIRE(result.new_monitor == 1);
    REQUIRE(result.clear_focus);
}

TEST_CASE("Pointer crossing back to first monitor changes active monitor", "[multimonitor][focus]")
{
    auto monitors = make_dual_monitors();

    // Pointer crosses from second to first monitor
    auto result = focus::pointer_move(monitors, 1, 500, 500);
    REQUIRE(result.active_monitor_changed);
    REQUIRE(result.new_monitor == 0);
    REQUIRE(result.clear_focus);
}

TEST_CASE("Pointer movement to stacked monitor (below)", "[multimonitor][focus]")
{
    auto monitors = make_stacked_monitors();

    // Pointer on top monitor
    auto result1 = focus::pointer_move(monitors, 0, 960, 500);
    REQUIRE_FALSE(result1.active_monitor_changed);

    // Pointer crosses to bottom monitor
    auto result2 = focus::pointer_move(monitors, 0, 960, 1200);
    REQUIRE(result2.active_monitor_changed);
    REQUIRE(result2.new_monitor == 1);
}

TEST_CASE("Pointer movement with negative coordinates", "[multimonitor][focus]")
{
    auto monitors = make_offset_monitors();

    // Pointer on primary monitor
    auto result1 = focus::pointer_move(monitors, 0, 500, 500);
    REQUIRE_FALSE(result1.active_monitor_changed);

    // Pointer crosses to left monitor (negative x)
    auto result2 = focus::pointer_move(monitors, 0, -500, 500);
    REQUIRE(result2.active_monitor_changed);
    REQUIRE(result2.new_monitor == 1);

    // Pointer crosses to top monitor (negative y)
    auto result3 = focus::pointer_move(monitors, 0, 500, -500);
    REQUIRE(result3.active_monitor_changed);
    REQUIRE(result3.new_monitor == 2);
}

// ─────────────────────────────────────────────────────────────────────────────
// Focus window state across monitors
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("Focusing window on different monitor switches active monitor", "[multimonitor][focus]")
{
    auto monitors = make_dual_monitors();
    monitors[1].workspaces[0].windows.push_back(0x2000);

    size_t active_monitor = 0;
    xcb_window_t active_window = XCB_NONE;

    auto change = focus::focus_window_state(monitors, active_monitor, active_window, 0x2000);

    REQUIRE(change);
    REQUIRE(change->target_monitor == 1);
    REQUIRE(active_monitor == 1);
    REQUIRE(active_window == 0x2000);
}

TEST_CASE("Focusing window on different workspace of same monitor", "[multimonitor][focus]")
{
    auto monitors = make_dual_monitors();
    monitors[0].current_workspace = 0;
    monitors[0].workspaces[3].windows.push_back(0x1000);

    size_t active_monitor = 0;
    xcb_window_t active_window = XCB_NONE;

    auto change = focus::focus_window_state(monitors, active_monitor, active_window, 0x1000);

    REQUIRE(change);
    REQUIRE(change->target_monitor == 0);
    REQUIRE(change->workspace_changed);
    REQUIRE(change->old_workspace == 0);
    REQUIRE(change->new_workspace == 3);
    REQUIRE(monitors[0].current_workspace == 3);
    REQUIRE(monitors[0].previous_workspace == 0);
}

TEST_CASE("Focusing window on second monitor different workspace", "[multimonitor][focus]")
{
    auto monitors = make_dual_monitors();
    monitors[0].current_workspace = 0;
    monitors[1].current_workspace = 1;
    monitors[1].workspaces[5].windows.push_back(0x2000);

    size_t active_monitor = 0;
    xcb_window_t active_window = XCB_NONE;

    auto change = focus::focus_window_state(monitors, active_monitor, active_window, 0x2000);

    REQUIRE(change);
    REQUIRE(change->target_monitor == 1);
    REQUIRE(change->workspace_changed);
    REQUIRE(change->old_workspace == 1);
    REQUIRE(change->new_workspace == 5);
    REQUIRE(monitors[1].current_workspace == 5);
}

// ─────────────────────────────────────────────────────────────────────────────
// EWMH desktop indexing across monitors
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("Desktop indices are unique across monitors", "[multimonitor][ewmh]")
{
    constexpr size_t workspaces = 10;

    // Monitor 0, workspaces 0-9 -> desktops 0-9
    for (size_t ws = 0; ws < workspaces; ++ws)
    {
        REQUIRE(ewmh_policy::desktop_index(0, ws, workspaces) == ws);
    }

    // Monitor 1, workspaces 0-9 -> desktops 10-19
    for (size_t ws = 0; ws < workspaces; ++ws)
    {
        REQUIRE(ewmh_policy::desktop_index(1, ws, workspaces) == 10 + ws);
    }

    // Monitor 2, workspaces 0-9 -> desktops 20-29
    for (size_t ws = 0; ws < workspaces; ++ws)
    {
        REQUIRE(ewmh_policy::desktop_index(2, ws, workspaces) == 20 + ws);
    }
}

TEST_CASE("Desktop index decoding identifies correct monitor", "[multimonitor][ewmh]")
{
    constexpr size_t workspaces = 10;

    // Desktop 5 -> Monitor 0, Workspace 5
    auto d5 = ewmh_policy::desktop_to_indices(5, workspaces);
    REQUIRE(d5);
    REQUIRE(d5->first == 0);
    REQUIRE(d5->second == 5);

    // Desktop 15 -> Monitor 1, Workspace 5
    auto d15 = ewmh_policy::desktop_to_indices(15, workspaces);
    REQUIRE(d15);
    REQUIRE(d15->first == 1);
    REQUIRE(d15->second == 5);

    // Desktop 25 -> Monitor 2, Workspace 5
    auto d25 = ewmh_policy::desktop_to_indices(25, workspaces);
    REQUIRE(d25);
    REQUIRE(d25->first == 2);
    REQUIRE(d25->second == 5);
}

// ─────────────────────────────────────────────────────────────────────────────
// Visibility across monitors
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("Window on correct workspace of monitor 1 is visible", "[multimonitor][visibility]")
{
    auto monitors = make_dual_monitors();
    monitors[0].current_workspace = 0;
    monitors[1].current_workspace = 2;

    // Window on monitor 1, workspace 2 (current)
    REQUIRE(visibility_policy::is_window_visible(false, false, false, 1, 2, monitors));

    // Window on monitor 1, workspace 0 (not current)
    REQUIRE_FALSE(visibility_policy::is_window_visible(false, false, false, 1, 0, monitors));
}

TEST_CASE("Sticky window visible across workspaces on same monitor", "[multimonitor][visibility]")
{
    auto monitors = make_dual_monitors();
    monitors[0].current_workspace = 0;
    monitors[1].current_workspace = 2;

    // Sticky window on monitor 1, workspace 5 (not current, but sticky)
    REQUIRE(visibility_policy::is_window_visible(false, false, true, 1, 5, monitors));
}

TEST_CASE("Window on invalid monitor is not visible", "[multimonitor][visibility]")
{
    auto monitors = make_dual_monitors();  // Only 2 monitors (0, 1)

    // Monitor index 5 doesn't exist
    REQUIRE_FALSE(visibility_policy::is_window_visible(false, false, false, 5, 0, monitors));
    REQUIRE_FALSE(visibility_policy::is_window_visible(false, false, true, 5, 0, monitors));
}

// ─────────────────────────────────────────────────────────────────────────────
// Focus candidate selection across monitors
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("Focus candidate selection respects monitor boundary", "[multimonitor][focus]")
{
    auto monitors = make_dual_monitors();
    monitors[0].workspaces[0].windows = { 0x1000, 0x1001 };
    monitors[0].workspaces[0].focused_window = 0x1000;

    auto eligible = [](xcb_window_t) { return true; };

    // Floating candidates on different monitors
    std::vector<focus_policy::FloatingCandidate> floating = {
        { 0x2000, 0, 0, false },  // Same monitor
        { 0x3000, 1, 0, false },  // Different monitor
    };

    std::vector<xcb_window_t> sticky_tiled;
    auto selection = focus_policy::select_focus_candidate(
        monitors[0].workspaces[0], 0, 0, sticky_tiled, floating, eligible
    );

    REQUIRE(selection);
    // Should prefer focused tiled window
    REQUIRE(selection->window == 0x1000);
}

TEST_CASE("Focus candidate from floating excludes other monitors", "[multimonitor][focus]")
{
    auto monitors = make_dual_monitors();
    // Empty workspace on monitor 0
    monitors[0].workspaces[0].windows = {};
    monitors[0].workspaces[0].focused_window = XCB_NONE;

    auto eligible = [](xcb_window_t) { return true; };

    // Only floating candidate is on monitor 1
    std::vector<focus_policy::FloatingCandidate> floating = {
        { 0x3000, 1, 0, false },  // Different monitor
    };

    std::vector<xcb_window_t> sticky_tiled;
    auto selection = focus_policy::select_focus_candidate(
        monitors[0].workspaces[0], 0, 0, sticky_tiled, floating, eligible
    );

    // Should NOT select window from different monitor
    REQUIRE_FALSE(selection);
}

// ─────────────────────────────────────────────────────────────────────────────
// Triple monitor scenarios
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("Pointer movement across triple monitors", "[multimonitor][focus]")
{
    auto monitors = make_triple_monitors_horizontal();

    // Start on monitor 0
    auto r1 = focus::pointer_move(monitors, 0, 500, 500);
    REQUIRE_FALSE(r1.active_monitor_changed);

    // Move to monitor 1
    auto r2 = focus::pointer_move(monitors, 0, 2500, 500);
    REQUIRE(r2.active_monitor_changed);
    REQUIRE(r2.new_monitor == 1);

    // Move to monitor 2
    auto r3 = focus::pointer_move(monitors, 1, 4000, 500);
    REQUIRE(r3.active_monitor_changed);
    REQUIRE(r3.new_monitor == 2);

    // Move back to monitor 0 (skip monitor 1)
    auto r4 = focus::pointer_move(monitors, 2, 500, 500);
    REQUIRE(r4.active_monitor_changed);
    REQUIRE(r4.new_monitor == 0);
}

TEST_CASE("Focus window across triple monitors", "[multimonitor][focus]")
{
    auto monitors = make_triple_monitors_horizontal();
    monitors[2].workspaces[0].windows.push_back(0x3000);

    size_t active_monitor = 0;
    xcb_window_t active_window = XCB_NONE;

    // Focus window on monitor 2 from monitor 0
    auto change = focus::focus_window_state(monitors, active_monitor, active_window, 0x3000);

    REQUIRE(change);
    REQUIRE(change->target_monitor == 2);
    REQUIRE(active_monitor == 2);
}
