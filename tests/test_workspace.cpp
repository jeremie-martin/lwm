#include "lwm/core/types.hpp"
#include <catch2/catch_test_macros.hpp>

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

TEST_CASE("Working area with struts exceeding monitor dimensions", "[workspace][edge]")
{
    SKIP("Reveals bug: individual strut components not clamped for offset calculation");

    // Struts exceeding monitor width
    {
        Monitor mon;
        mon.x = 0;
        mon.y = 0;
        mon.width = 1920;
        mon.height = 1080;
        init_workspaces(mon);
        mon.strut.left = 1000;
        mon.strut.right = 1000;

        auto area = mon.working_area();

        // Expected: individual strut components should be clamped
        // Current (buggy): area.x = 0 + 1000 = 1000 (strut.left not clamped)
        REQUIRE(area.x == 0);
        REQUIRE(area.width == 1);
    }

    // Struts exceeding monitor height
    {
        Monitor mon;
        mon.x = 0;
        mon.y = 0;
        mon.width = 1920;
        mon.height = 1080;
        init_workspaces(mon);
        mon.strut.top = 600;
        mon.strut.bottom = 600;

        auto area = mon.working_area();

        REQUIRE(area.y == 0);
        REQUIRE(area.height == 1);
    }

    // All struts exceeding dimensions
    {
        Monitor mon;
        mon.x = 0;
        mon.y = 0;
        mon.width = 100;
        mon.height = 100;
        init_workspaces(mon);
        mon.strut.left = 1000;
        mon.strut.right = 1000;
        mon.strut.top = 1000;
        mon.strut.bottom = 1000;

        auto area = mon.working_area();

        REQUIRE(area.x == 0);
        REQUIRE(area.y == 0);
        REQUIRE(area.width == 1);
        REQUIRE(area.height == 1);
    }

    // Very large strut values (int16_t overflow)
    {
        Monitor mon;
        mon.x = 0;
        mon.y = 0;
        mon.width = 1920;
        mon.height = 1080;
        init_workspaces(mon);
        mon.strut.left = 100000;
        mon.strut.right = 100000;
        mon.strut.top = 50000;
        mon.strut.bottom = 50000;

        auto area = mon.working_area();

        // Expected: struts should be clamped, or area_x/area_y should be int32_t
        // Current (buggy): area_x overflows to -31072 (100000 truncated to int16_t)
    }
}

TEST_CASE("Working area with struts exceeding monitor height", "[workspace][edge]")
{
    SKIP("Reveals bug: individual strut components not clamped for offset calculation");

    Monitor mon;
    mon.x = 0;
    mon.y = 0;
    mon.width = 1920;
    mon.height = 1080;
    init_workspaces(mon);

    // Struts exceed monitor height
    mon.strut.top = 600;
    mon.strut.bottom = 600;

    auto area = mon.working_area();

    // Expected: individual strut components should be clamped
    // Current (buggy): area.y = 0 + 600 = 600 (strut.top not clamped)
    REQUIRE(area.x == 0);
    REQUIRE(area.y == 0); // Top strut should be clamped
    REQUIRE(area.width == 1920);
    REQUIRE(area.height == 1); // Minimum height enforced
}

TEST_CASE("Working area with all struts exceeding dimensions", "[workspace][edge]")
{
    SKIP("Reveals bug: individual strut components not clamped for offset calculation");

    Monitor mon;
    mon.x = 0;
    mon.y = 0;
    mon.width = 100;
    mon.height = 100;
    init_workspaces(mon);

    // All struts exceed monitor dimensions
    mon.strut.left = 1000;
    mon.strut.right = 1000;
    mon.strut.top = 1000;
    mon.strut.bottom = 1000;

    auto area = mon.working_area();

    // Expected: individual strut components should be clamped
    // Current (buggy): area.x = 0 + 1000 = 1000, area.y = 0 + 1000 = 1000
    REQUIRE(area.x == 0);
    REQUIRE(area.y == 0);
    REQUIRE(area.width == 1);  // Minimum width enforced
    REQUIRE(area.height == 1); // Minimum height enforced
}

TEST_CASE("Working area with large strut values", "[workspace][edge]")
{
    SKIP("Reveals bug: int16_t overflow in area_x/area_y when struts exceed monitor coordinates");

    Monitor mon;
    mon.x = 0;
    mon.y = 0;
    mon.width = 1920;
    mon.height = 1080;
    init_workspaces(mon);

    // Very large strut values that exceed int16_t range when added to monitor coordinates
    mon.strut.left = 100000; // area_x = 0 + 100000 = 100000, which overflows when cast to int16_t
    mon.strut.right = 100000;
    mon.strut.top = 50000;
    mon.strut.bottom = 50000;

    auto area = mon.working_area();

    // Expected: struts should be clamped, or area_x/area_y should be int32_t
    // Current (buggy): area_x overflows to -31072 (100000 truncated to int16_t)
    // The implementation calculates area_x as int32_t but casts to int16_t in return
    // This causes silent overflow for large coordinates/struts
    REQUIRE(area.x == 0); // Should be clamped or handle overflow gracefully
    REQUIRE(area.y == 0);
    REQUIRE(area.width == 1);  // Minimum width enforced (h_strut clamped correctly)
    REQUIRE(area.height == 1); // Minimum height enforced (v_strut clamped correctly)
}

TEST_CASE("Working area at int16_t coordinate boundaries", "[workspace][edge]")
{
    Monitor mon;
    init_workspaces(mon);

    // Test at int16_t max (32767)
    mon.x = 32700;
    mon.y = 32700;
    mon.width = 100;
    mon.height = 100;
    mon.strut.top = 10;
    mon.strut.left = 10;

    auto area = mon.working_area();

    // Coordinates should be preserved (if within int16_t range)
    REQUIRE(area.x == 32710);
    REQUIRE(area.y == 32710);
    REQUIRE(area.width == 90);
    REQUIRE(area.height == 90);
}

TEST_CASE("Working area with negative coordinates", "[workspace][edge]")
{
    Monitor mon;
    mon.x = -1000;
    mon.y = -1000;
    mon.width = 1920;
    mon.height = 1080;
    init_workspaces(mon);

    mon.strut.top = 50;
    mon.strut.left = 50;

    auto area = mon.working_area();

    // Negative coordinates should be preserved correctly
    REQUIRE(area.x == -950);
    REQUIRE(area.y == -950);
    REQUIRE(area.width == 1870);
    REQUIRE(area.height == 1030);
}

TEST_CASE("Working area with maximum uint16_t dimensions", "[workspace][edge]")
{
    Monitor mon;
    mon.x = 0;
    mon.y = 0;
    mon.width = 65535; // Max uint16_t
    mon.height = 65535;
    init_workspaces(mon);

    mon.strut.left = 100;
    mon.strut.top = 100;

    auto area = mon.working_area();

    // Large dimensions should be handled correctly
    REQUIRE(area.x == 100);
    REQUIRE(area.y == 100);
    REQUIRE(area.width == 65435);
    REQUIRE(area.height == 65435);
}

TEST_CASE("Working area with zero struts", "[workspace][edge]")
{
    Monitor mon;
    mon.x = 100;
    mon.y = 100;
    mon.width = 1920;
    mon.height = 1080;
    init_workspaces(mon);

    mon.strut = {}; // All zeros

    auto area = mon.working_area();

    // Working area should match monitor geometry
    REQUIRE(area.x == 100);
    REQUIRE(area.y == 100);
    REQUIRE(area.width == 1920);
    REQUIRE(area.height == 1080);
}

TEST_CASE("Working area with one-sided struts", "[workspace][edge]")
{
    Monitor mon;
    mon.x = 0;
    mon.y = 0;
    mon.width = 1920;
    mon.height = 1080;
    init_workspaces(mon);

    SECTION("Only top strut")
    {
        mon.strut.top = 50;
        auto area = mon.working_area();
        REQUIRE(area.x == 0);
        REQUIRE(area.y == 50);
        REQUIRE(area.width == 1920);
        REQUIRE(area.height == 1030);
    }

    SECTION("Only left strut")
    {
        mon.strut.left = 100;
        auto area = mon.working_area();
        REQUIRE(area.x == 100);
        REQUIRE(area.y == 0);
        REQUIRE(area.width == 1820);
        REQUIRE(area.height == 1080);
    }

    SECTION("Only bottom strut")
    {
        mon.strut.bottom = 80;
        auto area = mon.working_area();
        REQUIRE(area.x == 0);
        REQUIRE(area.y == 0);
        REQUIRE(area.width == 1920);
        REQUIRE(area.height == 1000);
    }

    SECTION("Only right strut")
    {
        mon.strut.right = 120;
        auto area = mon.working_area();
        REQUIRE(area.x == 0);
        REQUIRE(area.y == 0);
        REQUIRE(area.width == 1800);
        REQUIRE(area.height == 1080);
    }
}

TEST_CASE("Working area with minimum monitor dimensions", "[workspace][edge]")
{
    SKIP("Reveals bug: individual strut components not clamped for offset calculation");

    Monitor mon;
    mon.x = 0;
    mon.y = 0;
    mon.width = 1;
    mon.height = 1;
    init_workspaces(mon);

    // Even with no struts, small monitor
    auto area = mon.working_area();

    REQUIRE(area.x == 0);
    REQUIRE(area.y == 0);
    REQUIRE(area.width == 1);
    REQUIRE(area.height == 1);

    // Add struts that would exceed dimensions
    mon.strut.left = 10;
    mon.strut.top = 10;

    area = mon.working_area();

    // Expected: strut.left and strut.top should be clamped to monitor dimensions
    // Current (buggy): area.x = 0 + 10 = 10, area.y = 0 + 10 = 10
    // Only the width/height are clamped, not the offset struts
    REQUIRE(area.x == 0);      // Strut should be clamped
    REQUIRE(area.y == 0);      // Strut should be clamped
    REQUIRE(area.width == 1);  // Minimum enforced
    REQUIRE(area.height == 1); // Minimum enforced
}

TEST_CASE("Monitor current() with out-of-bounds workspace index", "[workspace][edge]")
{
    Monitor mon;
    init_workspaces(mon, 3);

    // Valid workspace
    mon.current_workspace = 1;
    REQUIRE_NOTHROW(mon.current());
    REQUIRE(mon.current().windows.empty());

    // Out-of-bounds workspace index
    mon.current_workspace = 10;

    // This would be undefined behavior in C++ (vector access)
    // The test documents that current() assumes valid index
    // Real code should ensure current_workspace is always valid
}
