#include "lwm/core/events.hpp"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

TEST_CASE("Subscription filter parser ignores unknown tokens", "[policy][events][subscribe]")
{
    REQUIRE(lwm::parse_event_filter("") == lwm::all_events);
    REQUIRE(lwm::parse_event_filter("window_map") == lwm::event_mask<lwm::event::window_map>);
    REQUIRE(
        lwm::parse_event_filter("window_map,key_action")
        == (lwm::event_mask<lwm::event::window_map> | lwm::event_mask<lwm::event::key_action>)
    );
    REQUIRE(
        lwm::parse_event_filter("window_map,unknown,key_action")
        == (lwm::event_mask<lwm::event::window_map> | lwm::event_mask<lwm::event::key_action>)
    );
    REQUIRE(lwm::parse_event_filter("unknown,not_an_event") == 0);
    REQUIRE(
        lwm::parse_event_filter(" key_action, window_map ")
        == (lwm::event_mask<lwm::event::key_action> | lwm::event_mask<lwm::event::window_map>)
    );
}

TEST_CASE("Event records preserve the complete subscription wire contract", "[events][subscribe]")
{
    using namespace lwm;
    using Json = nlohmann::json;
    std::vector<std::pair<Event, Json>> cases{
        { event::workspace_switch{ 2, 3, 4 }, {{"event", "workspace_switch"}, {"monitor", 2}, {"from", 3}, {"to", 4}} },
        { event::focus_change{ 7, "quoted\"class", "line\nnext" },
          {{"event", "focus_change"}, {"window", 7}, {"class", "quoted\"class"}, {"title", "line\nnext"}} },
        { event::window_map{ 8, "app", "floating", Placement{ 2, 3 } },
          {{"event", "window_map"}, {"window", 8}, {"class", "app"}, {"kind", "floating"}, {"monitor", 2}, {"workspace", 3}} },
        { event::window_map{ 9, "", "dock", {} },
          {{"event", "window_map"}, {"window", 9}, {"class", ""}, {"kind", "dock"}} },
        { event::window_map{ 10, "", "popup", {} },
          {{"event", "window_map"}, {"window", 10}, {"class", ""}, {"kind", "popup"}} },
        { event::window_unmap{ 8, "tiled", Placement{ 0, 0 } },
          {{"event", "window_unmap"}, {"window", 8}, {"kind", "tiled"}, {"monitor", 0}, {"workspace", 0}} },
        { event::window_unmap{ 9, "desktop", {} },
          {{"event", "window_unmap"}, {"window", 9}, {"kind", "desktop"}} },
        { event::layout_change{ "set_layout", std::string("monocle"), std::nullopt },
          {{"event", "layout_change"}, {"action", "set_layout"}, {"value", "monocle"}} },
        { event::layout_change{ "set_ratio", 0.6123456789, std::nullopt },
          {{"event", "layout_change"}, {"action", "set_ratio"}, {"value", 0.6123456789}} },
        { event::layout_change{ "adjust_ratio", std::nullopt, -0.05 },
          {{"event", "layout_change"}, {"action", "adjust_ratio"}, {"delta", -0.05}} },
        { event::layout_change{ "reset_ratios", std::nullopt, std::nullopt },
          {{"event", "layout_change"}, {"action", "reset_ratios"}} },
        { event::key_action{ "focus_monitor_left" }, {{"event", "key_action"}, {"action", "focus_monitor_left"}} },
        { event::config_reload{ true, "ipc", std::nullopt }, {{"event", "config_reload"}, {"success", true}, {"source", "ipc"}} },
        { event::config_reload{ false, "sighup", "bad config" },
          {{"event", "config_reload"}, {"success", false}, {"source", "sighup"}, {"error", "bad config"}} },
        { event::state_change{}, {{"event", "state_change"}} }
    };
    for (auto const& [event, payload] : cases)
    {
        INFO(payload.dump());
        auto expected = payload;
        expected["instance"] = "wm-123";
        expected["sequence"] = 0x100000001ULL;
        CHECK(Json::parse(event_json(event, "wm-123", 0x100000001ULL)) == expected);
        CHECK(parse_event_filter(payload.at("event").get<std::string>()) == (uint32_t{1} << event.index()));
    }
    // Adding an alternative must expose its filter as well as its payload.
    CHECK(event_names().size() == std::variant_size_v<Event>);
    CHECK(parse_event_filter("workspace_switch,focus_change,window_map,window_unmap,layout_change,key_action,config_reload,state_change") == 255);
}

TEST_CASE("Event encoding preserves opaque X metadata bytes", "[events][subscribe]")
{
    auto bytes = std::string(1, static_cast<char>(0xff));
    auto encoded = lwm::event_json(lwm::event::focus_change{ 7, bytes, "line\nnext" }, "wm", 1);
    CHECK(encoded == "{\"instance\":\"wm\",\"sequence\":1,\"event\":\"focus_change\",\"window\":7,\"class\":\""
        + bytes + "\",\"title\":\"line\\nnext\"}");
}
