#include "lwm/core/types.hpp"
#include "state_fixture.hpp"
#include <catch2/catch_test_macros.hpp>

using namespace lwm;

namespace {

void init_workspaces(Monitor& monitor, size_t count = 10)
{
    monitor.workspaces.assign(count, Workspace{});
    monitor.current_workspace = 0;
}

} // namespace

TEST_CASE("Window can be found across workspaces", "[workspace]")
{
    Monitor mon;
    mon.name = "test";
    init_workspaces(mon);

    mon.workspaces[0].windows.push_back(0x1000);
    mon.workspaces[3].windows.push_back(0x2000);
    mon.workspaces[7].windows.push_back(0x3000);

    REQUIRE(mon.workspaces[0].find_window(0x1000) != mon.workspaces[0].windows.end());
    REQUIRE(mon.workspaces[3].find_window(0x2000) != mon.workspaces[3].windows.end());
    REQUIRE(mon.workspaces[7].find_window(0x3000) != mon.workspaces[7].windows.end());

    REQUIRE(mon.workspaces[0].find_window(0x2000) == mon.workspaces[0].windows.end());
    REQUIRE(mon.workspaces[1].find_window(0x1000) == mon.workspaces[1].windows.end());
}

TEST_CASE("Monitor working_area accounts for struts", "[monitor]")
{
    Monitor mon;
    mon.geometry.x = 0;
    mon.geometry.y = 0;
    mon.geometry.width = 1920;
    mon.geometry.height = 1080;
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
    REQUIRE(ws.preferred_tile == XCB_NONE);
}


TEST_CASE("Working area handles int16_t coordinate boundaries", "[workspace][edge]")
{
    Monitor mon;
    init_workspaces(mon);
    mon.geometry.x = 32700;
    mon.geometry.y = 32700;
    mon.geometry.width = 100;
    mon.geometry.height = 100;
    mon.strut.top = 10;
    mon.strut.left = 10;

    auto area = mon.working_area();
    REQUIRE(area.x == 32710);
    REQUIRE(area.y == 32710);
    REQUIRE(area.width == 90);
    REQUIRE(area.height == 90);
}

TEST_CASE("Working area handles negative coordinates", "[workspace][edge]")
{
    Monitor mon;
    mon.geometry.x = -1000;
    mon.geometry.y = -1000;
    mon.geometry.width = 1920;
    mon.geometry.height = 1080;
    init_workspaces(mon);
    mon.strut.top = 50;
    mon.strut.left = 50;

    auto area = mon.working_area();
    REQUIRE(area.x == -950);
    REQUIRE(area.y == -950);
    REQUIRE(area.width == 1870);
    REQUIRE(area.height == 1030);
}

TEST_CASE("Working area handles maximum uint16_t dimensions", "[workspace][edge]")
{
    Monitor mon;
    mon.geometry.x = 0;
    mon.geometry.y = 0;
    mon.geometry.width = 65535;
    mon.geometry.height = 65535;
    init_workspaces(mon);
    mon.strut.left = 100;
    mon.strut.top = 100;

    auto area = mon.working_area();
    REQUIRE(area.x == 100);
    REQUIRE(area.y == 100);
    REQUIRE(area.width == 65435);
    REQUIRE(area.height == 65435);
}

TEST_CASE("Working area with zero struts returns full monitor area", "[workspace][edge]")
{
    Monitor mon;
    mon.geometry.x = 100;
    mon.geometry.y = 100;
    mon.geometry.width = 1920;
    mon.geometry.height = 1080;
    init_workspaces(mon);
    mon.strut = {};

    auto area = mon.working_area();
    REQUIRE(area.x == 100);
    REQUIRE(area.y == 100);
    REQUIRE(area.width == 1920);
    REQUIRE(area.height == 1080);
}

TEST_CASE("Working area subtracts each strut independently", "[workspace][edge]")
{
    Monitor mon;
    mon.geometry.x = 0;
    mon.geometry.y = 0;
    mon.geometry.width = 1920;
    mon.geometry.height = 1080;
    init_workspaces(mon);

    SECTION("Top strut")
    {
        mon.strut.top = 50;
        auto area = mon.working_area();
        REQUIRE(area.y == 50);
        REQUIRE(area.height == 1030);
    }

    SECTION("Left strut")
    {
        mon.strut.left = 100;
        auto area = mon.working_area();
        REQUIRE(area.x == 100);
        REQUIRE(area.width == 1820);
    }

    SECTION("Bottom strut")
    {
        mon.strut.bottom = 80;
        auto area = mon.working_area();
        REQUIRE(area.height == 1000);
    }

    SECTION("Right strut")
    {
        mon.strut.right = 120;
        auto area = mon.working_area();
        REQUIRE(area.width == 1800);
    }
}

TEST_CASE("find_window on empty workspace returns end", "[workspace][edge]")
{
    Workspace ws;
    REQUIRE(ws.windows.empty());
    REQUIRE(ws.find_window(0x1000) == ws.windows.end());
}

TEST_CASE("Workspace switches reject the current and out-of-range workspaces", "[workspace][policy]")
{
    auto state = test::state(1, 3);
    REQUIRE(state.switch_workspace(0, 1));
    auto revision = state.revision();
    CHECK_FALSE(state.switch_workspace(0, 1));
    CHECK_FALSE(state.switch_workspace(0, 5));
    CHECK_FALSE(state.switch_workspace(4, 0));
    CHECK(state.revision() == revision);
    CHECK(state.monitors()[0].current_workspace == 1);
    CHECK(state.monitors()[0].previous_workspace == 0);
}
