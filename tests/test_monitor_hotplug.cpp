#include "lwm/core/focus.hpp"
#include "state_fixture.hpp"
#include <catch2/catch_test_macros.hpp>
using namespace lwm;
namespace {
Monitor monitor(std::string name, int16_t x = 0)
{
    Monitor m;
    m.name = std::move(name);
    m.geometry.x = x;
    m.geometry.width = 1000;
    m.geometry.height = 800;
    m.workspaces.resize(3);
    return m;
}
}
TEST_CASE("Output reconciliation preserves complete surviving workspace state", "[hotplug][monitor]")
{
    auto state = test::state(2);
    test::outputs(state, { test::output("A"), test::output("B", 1000) });
    test::add(state, 10);
    state.relocate(10, 0, 2); // A hidden destination remembers the tile preference.
    test::add(state, 20, { .workspace = 2 });
    state.switch_workspace(0, 1);
    state.switch_workspace(0, 2);
    state.layout(0, LayoutStrategy::Monocle);
    state.ratio(0, SplitAddress{ 1 }, 0.3);
    test::outputs(state, { test::output("B"), test::output("A", -1000), test::output("C", 1000) });
    auto const& restored = state.monitors()[1].workspaces[2];
    CHECK(state.monitors()[1].name == "A");
    CHECK(restored.windows == std::vector<xcb_window_t>{ 10, 20 });
    CHECK(restored.preferred_tile == 10);
    CHECK(restored.layout_strategy == LayoutStrategy::Monocle);
    CHECK(restored.split_ratios.at(SplitAddress{ 1 }) == 0.3);
    CHECK(state.monitors()[1].current_workspace == 2);
    CHECK(state.monitors()[1].previous_workspace == 1);
    CHECK(state.monitors()[1].geometry.x == -1000);
    CHECK(state.require(10).monitor == 1);
    CHECK(state.monitors()[2].workspaces[2].windows.empty());
}

TEST_CASE("Removed outputs merge tiled membership without replacing surviving policy", "[hotplug][monitor]")
{
    auto state = test::state(2);
    test::outputs(state, { test::output("gone"), test::output("kept", 1000) });
    state.switch_workspace(0, 1);
    state.ratio(0, SplitAddress{ 0 }, 0.8);
    state.switch_workspace(0, 0);
    state.switch_workspace(1, 1);
    state.ratio(1, SplitAddress{ 0 }, 0.4);
    state.switch_workspace(1, 0);
    xcb_window_t expected = 20;
    SECTION("surviving focus takes precedence")
    {
        test::add(state, 30, { .monitor = 1 });
        state.relocate(30, 1, 1);
        expected = 30;
    }
    SECTION("empty destination inherits incoming focus") { }
    for (xcb_window_t id : { 10, 20 })
    {
        test::add(state, id);
        state.relocate(id, 0, 1);
    }
    REQUIRE(state.monitors()[0].workspaces[1].preferred_tile == 20);
    test::outputs(state, { test::output("kept") });
    auto const& ws = state.monitors()[0].workspaces[1];
    CHECK(ws.preferred_tile == expected);
    CHECK(ws.windows[ws.windows.size() - 2] == 10);
    CHECK(ws.windows.back() == 20);
    CHECK(ws.split_ratios.at(SplitAddress{ 0 }) == 0.4);
}

TEST_CASE("Repeated output refresh does not duplicate membership or discard ratios", "[hotplug][monitor][sequence]")
{
    auto state = test::state();
    test::add(state, 42);
    state.ratio(0, SplitAddress{ 0 }, 0.7);
    for (int i = 0; i < 20; ++i)
    {
        test::outputs(state, { test::output("M0", static_cast<int16_t>(i * 10)) });
        REQUIRE(state.monitors()[0].workspaces[0].windows == std::vector<xcb_window_t>{ 42 });
        REQUIRE(state.monitors()[0].workspaces[0].split_ratios.at(SplitAddress{ 0 }) == 0.7);
    }
}

TEST_CASE("Monitor lookup keeps computed centers wide until containment", "[monitor][bounds]")
{
    std::vector<Monitor> monitors{ monitor("left", -30000), monitor("right", 30000) };
    CHECK_FALSE(monitor_at(monitors, 35536, 100));
    CHECK(monitor_at(monitors, -29500, 100) == 0);
    CHECK(monitor_at(monitors, 30500, 100) == 1);
}
