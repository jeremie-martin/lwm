#include <catch2/catch_test_macros.hpp>
#include "lwm/core/types.hpp"

using namespace lwm;

namespace {

void init_workspaces(Monitor& monitor, size_t count = 10)
{
    monitor.workspaces.assign(count, Workspace{});
    monitor.current_workspace = 0;
}

} // namespace

TEST_CASE("Windows persist across workspace switches", "[workspace][critical]")
{
    Monitor mon;
    mon.name = "test";
    mon.width = 1920;
    mon.height = 1080;
    init_workspaces(mon);

    // Add window to workspace 0
    mon.workspaces[0].windows.push_back(0x1000);
    mon.workspaces[0].focused_window = 0x1000;

    // "Switch" to workspace 1 (data structure operation only)
    mon.current_workspace = 1;

    // Verify window still exists in workspace 0
    REQUIRE(mon.workspaces[0].windows.size() == 1);
    REQUIRE(mon.workspaces[0].windows[0] == 0x1000);

    // "Switch" back to workspace 0
    mon.current_workspace = 0;

    // Window should still be there
    REQUIRE(mon.current().windows.size() == 1);
    REQUIRE(mon.current().windows[0] == 0x1000);
}

TEST_CASE("Focus fallback when focused window removed", "[focus]")
{
    Workspace ws;
    ws.windows.push_back(0x1000);
    ws.windows.push_back(0x2000);
    ws.focused_window = 0x1000;

    // Remove focused window
    auto it = ws.find_window(ws.focused_window);
    ws.windows.erase(it);
    ws.focused_window = ws.windows.empty() ? XCB_NONE : ws.windows.back();

    REQUIRE(ws.focused_window == 0x2000);
}

TEST_CASE("Window can be found across workspaces", "[workspace]")
{
    Monitor mon;
    mon.name = "test";
    init_workspaces(mon);

    // Add windows to different workspaces
    mon.workspaces[0].windows.push_back(0x1000);
    mon.workspaces[3].windows.push_back(0x2000);
    mon.workspaces[7].windows.push_back(0x3000);

    // Should find windows in their respective workspaces
    REQUIRE(mon.workspaces[0].find_window(0x1000) != mon.workspaces[0].windows.end());
    REQUIRE(mon.workspaces[3].find_window(0x2000) != mon.workspaces[3].windows.end());
    REQUIRE(mon.workspaces[7].find_window(0x3000) != mon.workspaces[7].windows.end());

    // Should NOT find windows in wrong workspaces
    REQUIRE(mon.workspaces[0].find_window(0x2000) == mon.workspaces[0].windows.end());
    REQUIRE(mon.workspaces[1].find_window(0x1000) == mon.workspaces[1].windows.end());
}

TEST_CASE("Monitor working_area accounts for struts", "[monitor]")
{
    Monitor mon;
    mon.x = 0;
    mon.y = 0;
    mon.width = 1920;
    mon.height = 1080;
    init_workspaces(mon);

    // No strut
    auto area = mon.working_area();
    REQUIRE(area.x == 0);
    REQUIRE(area.y == 0);
    REQUIRE(area.width == 1920);
    REQUIRE(area.height == 1080);

    // Add top strut (e.g., Polybar)
    mon.strut.top = 30;
    area = mon.working_area();
    REQUIRE(area.x == 0);
    REQUIRE(area.y == 30);
    REQUIRE(area.width == 1920);
    REQUIRE(area.height == 1050);

    // Add left strut too
    mon.strut.left = 50;
    area = mon.working_area();
    REQUIRE(area.x == 50);
    REQUIRE(area.y == 30);
    REQUIRE(area.width == 1870);
    REQUIRE(area.height == 1050);
}

TEST_CASE("Empty workspace has no focused window", "[workspace]")
{
    Workspace ws;
    REQUIRE(ws.windows.empty());
    REQUIRE(ws.focused_window == XCB_NONE);
}

TEST_CASE("Moving window between workspaces preserves data", "[workspace]")
{
    Monitor mon;
    mon.name = "test";
    init_workspaces(mon);

    // Add window to workspace 0
    xcb_window_t win = 0x1000;
    mon.workspaces[0].windows.push_back(win);
    mon.workspaces[0].focused_window = 0x1000;

    // Move window to workspace 2
    auto it = mon.workspaces[0].find_window(0x1000);
    xcb_window_t moved = *it;
    mon.workspaces[0].windows.erase(it);
    mon.workspaces[0].focused_window = XCB_NONE;
    mon.workspaces[2].windows.push_back(moved);
    mon.workspaces[2].focused_window = moved;

    // Verify window moved correctly
    REQUIRE(mon.workspaces[0].windows.empty());
    REQUIRE(mon.workspaces[0].focused_window == XCB_NONE);
    REQUIRE(mon.workspaces[2].windows.size() == 1);
    REQUIRE(mon.workspaces[2].windows[0] == 0x1000);
    REQUIRE(mon.workspaces[2].focused_window == 0x1000);
}
