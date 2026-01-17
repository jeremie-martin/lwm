#include <catch2/catch_test_macros.hpp>
#include "lwm/core/focus.hpp"

using namespace lwm;

namespace {

Monitor make_monitor(int16_t x, int16_t y, uint16_t width, uint16_t height)
{
    Monitor monitor;
    monitor.x = x;
    monitor.y = y;
    monitor.width = width;
    monitor.height = height;
    monitor.workspaces.assign(10, Workspace{});
    monitor.current_workspace = 0;
    return monitor;
}

} // namespace

TEST_CASE("Pointer movement keeps focus on same monitor", "[focus][monitor]")
{
    std::vector<Monitor> monitors;
    monitors.push_back(make_monitor(0, 0, 1920, 1080));
    monitors.push_back(make_monitor(1920, 0, 1920, 1080));

    auto result = focus::pointer_move(monitors, 0, 100, 100);
    REQUIRE_FALSE(result.active_monitor_changed);
    REQUIRE_FALSE(result.clear_focus);
}

TEST_CASE("Pointer movement to another monitor clears focus", "[focus][monitor]")
{
    std::vector<Monitor> monitors;
    monitors.push_back(make_monitor(0, 0, 1920, 1080));
    monitors.push_back(make_monitor(1920, 0, 1920, 1080));

    auto result = focus::pointer_move(monitors, 0, 2000, 200);
    REQUIRE(result.active_monitor_changed);
    REQUIRE(result.new_monitor == 1);
    REQUIRE(result.clear_focus);
}

TEST_CASE("Focusing a window updates active monitor and workspace", "[focus][window]")
{
    std::vector<Monitor> monitors;
    monitors.push_back(make_monitor(0, 0, 1920, 1080));
    monitors.push_back(make_monitor(1920, 0, 1920, 1080));

    monitors[1].current_workspace = 1;
    monitors[1].workspaces[2].windows.push_back({ 0x2000, "target" });

    size_t active_monitor = 0;
    xcb_window_t active_window = XCB_NONE;

    auto change = focus::focus_window_state(monitors, active_monitor, active_window, 0x2000);
    REQUIRE(change);
    REQUIRE(change->target_monitor == 1);
    REQUIRE(change->workspace_changed);
    REQUIRE(change->old_workspace == 1);
    REQUIRE(change->new_workspace == 2);
    REQUIRE(active_monitor == 1);
    REQUIRE(active_window == 0x2000);
    REQUIRE(monitors[1].current_workspace == 2);
    REQUIRE(monitors[1].current().focused_window == 0x2000);
}

TEST_CASE("Focusing an unknown window is a no-op", "[focus][window]")
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
