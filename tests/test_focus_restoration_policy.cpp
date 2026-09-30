#include "lwm/core/focus.hpp"
#include <catch2/catch_test_macros.hpp>
using namespace lwm;

TEST_CASE(
    "Focus restoration uses remembered tiles, history, membership, sticky tiles, then floating recency",
    "[focus][policy]"
)
{
    focus::Clients clients;
    Monitor monitor;
    monitor.workspaces.resize(3);
    focus::Context context{ 0, 0, XCB_NONE, false };
    for (xcb_window_t id = 1; id <= 7; ++id)
    {
        Client client;
        client.id = id;
        client.mru_order = id;
        clients.emplace(id, client);
    }
    auto& ws = monitor.workspaces[0];
    ws.windows = { 1, 2, 3 };
    ws.focused_window = 3;
    ws.focus_history = { 2, 1, 3 };
    clients.at(4).workspace = 1;
    clients.at(4).sticky = true;
    monitor.workspaces[1].windows = { 4 };
    clients.at(5).workspace = 2;
    clients.at(5).sticky = true;
    monitor.workspaces[2].windows = { 5 };
    clients.at(6).state = FloatingState{ };
    clients.at(7).state = FloatingState{ };
    REQUIRE(focus::fallback(clients, monitor, context) == 3);
    clients.at(3).iconic = true;
    REQUIRE(focus::fallback(clients, monitor, context) == 1);
    ws.focus_history.clear();
    REQUIRE(focus::fallback(clients, monitor, context) == 2);
    clients.at(1).iconic = clients.at(2).iconic = true;
    REQUIRE(focus::fallback(clients, monitor, context) == 5);
    clients.erase(5);
    REQUIRE(focus::fallback(clients, monitor, context) == 4);
    clients.at(4).sticky = false;
    REQUIRE(focus::fallback(clients, monitor, context) == 7);
    clients.at(7).monitor = 1;
    REQUIRE(focus::fallback(clients, monitor, context) == 6);
    clients.at(6).workspace = 1;
    REQUIRE(focus::fallback(clients, monitor, context) == XCB_NONE);
    clients.at(6).sticky = true;
    REQUIRE(focus::fallback(clients, monitor, context) == 6);
}

TEST_CASE("Stale focus memory cannot select a moved or reclassified client", "[focus][policy]")
{
    focus::Clients clients;
    Monitor monitor;
    monitor.workspaces.resize(2);
    auto& ws = monitor.workspaces[0];
    ws.windows = { 1 };
    ws.focused_window = 2;
    ws.focus_history = { 2, 999 };
    for (xcb_window_t id : { 1, 2 })
    {
        Client c;
        c.id = id;
        clients.emplace(id, c);
    }
    SECTION("Moved sticky tile")
    {
        clients.at(2).workspace = 1;
        clients.at(2).sticky = true;
        monitor.workspaces[1].windows = { 2 };
    }
    SECTION("Converted floating window") { clients.at(2).state = FloatingState{ }; }
    REQUIRE(focus::fallback(clients, monitor, { 0, 0, XCB_NONE, false }) == 1);
}
