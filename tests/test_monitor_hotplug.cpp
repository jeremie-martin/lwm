#include "lwm/core/focus.hpp"
#include "lwm/core/policy.hpp"
#include <catch2/catch_test_macros.hpp>
using namespace lwm;
namespace {
Monitor monitor(std::string name, int16_t x = 0)
{
    Monitor m;
    m.name = std::move(name);
    m.x = x;
    m.width = 1000;
    m.height = 800;
    m.workspaces.resize(3);
    return m;
}
}
TEST_CASE("Output reconciliation preserves complete surviving workspace state", "[hotplug][monitor]")
{
    std::vector<Monitor> old{ monitor("A"), monitor("B", 1000) };
    old[0].current_workspace = 2;
    old[0].previous_workspace = 1;
    auto& ws = old[0].workspaces[2];
    ws.windows = { 10, 20 };
    ws.preferred_tile = 10;
    ws.layout_strategy = LayoutStrategy::Monocle;
    ws.split_ratios[SplitAddress{ 1 }] = 0.3;
    std::vector<Monitor> next{ monitor("B"), monitor("A", -1000), monitor("C", 1000) };
    auto destinations = hotplug_policy::preserve_workspaces(old, next);
    REQUIRE(destinations == std::vector<size_t>{ 1, 0 });
    auto const& restored = next[1].workspaces[2];
    CHECK(restored.windows == std::vector<xcb_window_t>{ 10, 20 });
    CHECK(restored.preferred_tile == 10);
    CHECK(restored.layout_strategy == LayoutStrategy::Monocle);
    CHECK(restored.split_ratios.at(SplitAddress{ 1 }) == 0.3);
    CHECK(next[1].current_workspace == 2);
    CHECK(next[1].previous_workspace == 1);
    CHECK(next[1].x == -1000);
    CHECK(next[2].workspaces[2].windows.empty());
}
TEST_CASE("Removed outputs merge tiled membership without replacing surviving policy", "[hotplug][monitor]")
{
    std::vector<Monitor> old{ monitor("gone"), monitor("kept") };
    old[0].workspaces[1].windows = { 10, 20 };
    old[0].workspaces[1].preferred_tile = 20;
    old[0].workspaces[1].split_ratios[SplitAddress{ 0 }] = 0.8;
    old[1].workspaces[1].split_ratios[SplitAddress{ 0 }] = 0.4;
    SECTION("surviving focus takes precedence")
    {
        old[1].workspaces[1].windows = { 30 };
        old[1].workspaces[1].preferred_tile = 30;
    }
    SECTION("empty destination inherits incoming focus") { }
    auto expected = old[1].workspaces[1].preferred_tile == 30 ? 30u : 20u;
    std::vector<Monitor> next{ monitor("kept") };
    REQUIRE(hotplug_policy::preserve_workspaces(old, next) == std::vector<size_t>{ 0, 0 });
    auto const& ws = next[0].workspaces[1];
    CHECK(ws.preferred_tile == expected);
    CHECK(ws.windows[ws.windows.size() - 2] == 10);
    CHECK(ws.windows.back() == 20);
    CHECK(ws.split_ratios.at(SplitAddress{ 0 }) == 0.4);
}
TEST_CASE("Repeated output refresh does not duplicate membership or discard ratios", "[hotplug][monitor][sequence]")
{
    std::vector<Monitor> state{ monitor("A") };
    state[0].workspaces[0].windows = { 42 };
    state[0].workspaces[0].split_ratios[SplitAddress{ 0 }] = 0.7;
    for (int i = 0; i < 20; ++i)
    {
        std::vector<Monitor> next{ monitor("A", static_cast<int16_t>(i * 10)) };
        hotplug_policy::preserve_workspaces(state, next);
        state = std::move(next);
        REQUIRE(state[0].workspaces[0].windows == std::vector<xcb_window_t>{ 42 });
        REQUIRE(state[0].workspaces[0].split_ratios.at(SplitAddress{ 0 }) == 0.7);
    }
}

TEST_CASE("Monitor lookup keeps computed centers wide until containment", "[monitor][bounds]")
{
    std::vector<Monitor> monitors{ monitor("left", -30000), monitor("right", 30000) };
    CHECK_FALSE(focus::monitor_index_at_point(monitors, 35536, 100));
    CHECK(focus::monitor_index_at_point(monitors, -29500, 100) == 0);
    CHECK(focus::monitor_index_at_point(monitors, 30500, 100) == 1);
}
