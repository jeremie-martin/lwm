#include "lwm/core/events.hpp"

#include <catch2/catch_test_macros.hpp>

TEST_CASE("Subscription filter parser ignores unknown tokens", "[policy][events][subscribe]")
{
    REQUIRE(lwm::parse_event_filter("") == lwm::Event_All);
    REQUIRE(lwm::parse_event_filter("window_map") == lwm::Event_WindowMap);
    REQUIRE(
        lwm::parse_event_filter("window_map,key_action")
        == (lwm::Event_WindowMap | lwm::Event_KeyAction)
    );
    REQUIRE(
        lwm::parse_event_filter("window_map,unknown,key_action")
        == (lwm::Event_WindowMap | lwm::Event_KeyAction)
    );
    REQUIRE(lwm::parse_event_filter("unknown,not_an_event") == 0);
    REQUIRE(
        lwm::parse_event_filter(" key_action, window_map ")
        == (lwm::Event_KeyAction | lwm::Event_WindowMap)
    );
}
