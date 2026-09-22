#include "lwm/core/invariants.hpp"
#include <catch2/catch_test_macros.hpp>

using namespace lwm;

TEST_CASE("Model validation accepts valid client kinds and independent fullscreen state", "[client][invariants]")
{
    std::vector<Monitor> monitors(1);
    monitors[0].workspaces.resize(2);
    monitors[0].workspaces[0].windows = { 1 };
    std::unordered_map<xcb_window_t, Client> clients;
    for (xcb_window_t id = 1; id <= 4; ++id) clients[id].id = id;
    set_floating_state(clients[2], { 10, 20, 300, 200 });
    clients[3].state = DockState{ };
    clients[4].state = DesktopState{ };

    SECTION("All client kinds") { monitors[0].workspaces[0].focused_window = 1; }
    SECTION("Minimized fullscreen remains a valid requested state")
    {
        clients[1].fullscreen = true;
        clients[1].iconic = true;
        clients[2].fullscreen = true;
        clients[2].iconic = true;
    }
    SECTION("Containers do not participate in workspace placement")
    {
        clients[3].workspace = 99;
        clients[4].monitor = 99;
    }
    REQUIRE_FALSE(invariants::validate(clients, monitors));
}

TEST_CASE("Model validation rejects inconsistent authoritative records", "[client][invariants]")
{
    std::vector<Monitor> monitors(2);
    for (auto& monitor : monitors) monitor.workspaces.resize(2);
    monitors[0].workspaces[0].windows = { 1 };
    std::unordered_map<xcb_window_t, Client> clients;
    clients[1].id = 1;

    SECTION("Duplicate within one workspace") { monitors[0].workspaces[0].windows.push_back(1); }
    SECTION("Duplicate across workspaces") { monitors[0].workspaces[1].windows.push_back(1); }
    SECTION("Duplicate across monitors") { monitors[1].workspaces[0].windows.push_back(1); }
    SECTION("Unmanaged member") { clients.clear(); }
    SECTION("Floating member") { set_floating_state(clients[1], { }); }
    SECTION("Dock member") { clients[1].state = DockState{ }; }
    SECTION("Desktop member") { clients[1].state = DesktopState{ }; }
    SECTION("Missing membership") { monitors[0].workspaces[0].windows.clear(); }
    SECTION("Wrong monitor") { clients[1].monitor = 1; }
    SECTION("Wrong workspace") { clients[1].workspace = 1; }
    SECTION("Invalid tiled monitor") { clients[1].monitor = 99; }
    SECTION("Invalid tiled workspace") { clients[1].workspace = 99; }
    SECTION("Invalid floating monitor")
    {
        monitors[0].workspaces[0].windows.clear();
        set_floating_state(clients[1], { });
        clients[1].monitor = 99;
    }
    SECTION("Invalid floating workspace")
    {
        monitors[0].workspaces[0].windows.clear();
        set_floating_state(clients[1], { });
        clients[1].workspace = 99;
    }
    SECTION("Mismatched registry key") { clients[1].id = 2; }
    SECTION("Mismatched container key")
    {
        clients[2].id = 3;
        clients[2].state = DockState{ };
    }
    SECTION("Zero registry key") { clients[0].state = DesktopState{ }; }
    SECTION("Unmanaged workspace focus") { monitors[0].workspaces[0].focused_window = 2; }
    SECTION("Focus in wrong workspace") { monitors[0].workspaces[1].focused_window = 1; }
    SECTION("Iconic workspace focus")
    {
        monitors[0].workspaces[0].focused_window = 1;
        clients[1].iconic = true;
    }
    SECTION("Invalid current workspace") { monitors[0].current_workspace = 99; }
    SECTION("Invalid previous workspace") { monitors[0].previous_workspace = 99; }
    SECTION("Monitor without workspaces") { monitors[1].workspaces.clear(); }
    REQUIRE(invariants::validate(clients, monitors));
}

TEST_CASE("Model validation accepts an empty registry without monitors", "[client][invariants]")
{
    REQUIRE_FALSE(invariants::validate({ }, { }));
}

TEST_CASE("Model validation checks active focus at completed transitions", "[client][invariants]")
{
    std::vector<Monitor> monitors(1);
    monitors[0].workspaces.resize(1);
    std::unordered_map<xcb_window_t, Client> clients;
    clients[1].id = 1;
    set_floating_state(clients[1], { });
    REQUIRE_FALSE(invariants::validate(clients, monitors, 1));

    SECTION("Unmanaged focus") { clients.clear(); }
    SECTION("Iconic focus") { clients[1].iconic = true; }
    SECTION("Hidden focus") { clients[1].hidden = true; }
    SECTION("Dock focus") { clients[1].state = DockState{ }; }
    SECTION("Desktop focus") { clients[1].state = DesktopState{ }; }
    auto violation = invariants::validate(clients, monitors, 1);
    REQUIRE(violation);
    REQUIRE(violation->window == 1);
}
