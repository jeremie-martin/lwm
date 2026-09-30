#include "lwm/core/focus.hpp"
#include <catch2/catch_test_macros.hpp>
using namespace lwm;

TEST_CASE("Automatic focus reads current visibility and input contracts", "[focus][policy]")
{
    Client client;
    client.id = 1;
    focus::Context context{ 0, 0, XCB_NONE, false };
    REQUIRE(focus::eligible(client, context));
    SECTION("Iconic") { client.iconic = true; }
    SECTION("Other monitor")
    {
        client.monitor = 1;
        client.sticky = true;
    }
    SECTION("Hidden workspace") { client.workspace = 1; }
    SECTION("Showing desktop includes sticky clients")
    {
        context.showing_desktop = true;
        client.sticky = true;
    }
    SECTION("Dock") { client.state = DockState{ }; }
    SECTION("Desktop") { client.state = DesktopState{ }; }
    SECTION("Neither input protocol")
    {
        client.accepts_input = false;
        client.supports_take_focus = false;
    }
    SECTION("Fullscreen suppression") { context.fullscreen_owner = 2; }
    REQUIRE_FALSE(focus::eligible(client, context));
}

TEST_CASE(
    "Automatic focus includes sticky clients and fullscreen transients with either input protocol",
    "[focus][policy]"
)
{
    Client client;
    client.id = 1;
    client.workspace = 1;
    client.sticky = true;
    focus::Context context{ 0, 0, 1, false };
    SECTION("Fullscreen owner") { }
    SECTION("Fullscreen transient")
    {
        context.fullscreen_owner = 2;
        client.transient_for = 2;
    }
    client.accepts_input = false;
    client.supports_take_focus = true;
    REQUIRE(focus::eligible(client, context));
    client.accepts_input = true;
    client.supports_take_focus = false;
    REQUIRE(focus::eligible(client, context));
}

TEST_CASE("MRU traversal keeps order but reads eligibility and lifetime live", "[focus][policy]")
{
    focus::Clients clients;
    for (xcb_window_t id : { 1, 2, 3, 4 })
    {
        Client client;
        client.id = id;
        client.mru_order = id;
        clients.emplace(id, client);
    }
    clients.at(2).state = FloatingState{ };
    auto order = focus::recent_order(clients);
    REQUIRE(order == std::vector<xcb_window_t>{ 4, 3, 2, 1 });
    focus::Context context{ 0, 0, XCB_NONE, false };
    xcb_window_t current = 4;
    for (xcb_window_t expected : { 3, 2, 1, 4 })
    {
        current = focus::cycle_target(order, clients, context, current, true);
        REQUIRE(current == expected);
        clients.at(current).mru_order += 10;
    }
    REQUIRE(focus::cycle_target(order, clients, context, 4, false) == 1);
    clients.erase(3);
    clients.at(2).iconic = true;
    REQUIRE(focus::cycle_target(order, clients, context, 4, true) == 1);
    clients.at(2).iconic = false;
    REQUIRE(focus::cycle_target(order, clients, context, 4, true) == 2);
    REQUIRE(focus::cycle_target(order, clients, context, 999, true) == 4);
    REQUIRE(focus::cycle_target(order, clients, context, 999, false) == 1);
    clients.at(1).iconic = clients.at(2).iconic = true;
    REQUIRE(focus::cycle_target(order, clients, context, 4, true) == 4);
    REQUIRE(focus::cycle_target(order, clients, context, 4, false) == 4);
    context.showing_desktop = true;
    REQUIRE(focus::cycle_target(order, clients, context, 4, true) == XCB_NONE);
    REQUIRE(focus::cycle_target({ }, clients, context, 4, false) == XCB_NONE);
}
