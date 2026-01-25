#include "lwm/core/focus.hpp"
#include "lwm/core/policy.hpp"
#include "lwm/core/types.hpp"
#include <catch2/catch_test_macros.hpp>

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
    monitors.push_back(make_monitor(0, 0, 1920, 1080));    // Top
    monitors.push_back(make_monitor(0, 1080, 1920, 1080)); // Bottom
    return monitors;
}

std::vector<Monitor> make_mixed_resolution_monitors()
{
    std::vector<Monitor> monitors;
    monitors.push_back(make_monitor(0, 0, 1920, 1080));    // 1080p
    monitors.push_back(make_monitor(1920, 0, 2560, 1440)); // 1440p
    monitors.push_back(make_monitor(4480, 0, 3840, 2160)); // 4K
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

// ─────────────────────────────────────────────────────────────────────────────
// Focus window state across monitors
// ─────────────────────────────────────────────────────────────────────────────

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
