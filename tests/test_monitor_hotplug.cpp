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

TEST_CASE("Identical topology preserves interaction context and revision", "[hotplug][monitor][drag][focus]")
{
    auto state = test::state();
    for (auto id : { 1, 2, 3 })
    {
        test::add(state, id);
        test::focus(state, id);
    }
    SECTION("focus traversal retains its original MRU order")
    {
        REQUIRE(state.cycle_focus(true));
        state.settle();
        REQUIRE(state.active_window() == 2);
        auto revision = state.revision();
        test::outputs(state, { test::output("M0") });
        CHECK(state.revision() == revision);
        REQUIRE(state.cycle_focus(true));
        CHECK(state.active_window() == 1);
    }
    SECTION("window move remains grabbed in the model")
    {
        state.begin_drag(State::Grip{ 3, floating::ResizeEdge::None }, 600, 100, 1);
        state.drag_to(650, 150);
        state.settle();
        auto revision = state.revision();
        test::outputs(state, { test::output("M0") });
        CHECK(state.revision() == revision);
        REQUIRE(state.drag());
        state.settle();
        CHECK(state.drag());
        test::outputs(state, { test::output("M0", 10) });
        CHECK_FALSE(state.drag());
    }
    SECTION("split resize remains valid")
    {
        auto hit = state.split_at(500, 400);
        REQUIRE(hit);
        state.begin_drag(*hit, 500, 400, 1);
        state.settle();
        auto revision = state.revision();
        test::outputs(state, { test::output("M0") });
        CHECK(state.revision() == revision);
        state.settle();
        REQUIRE(state.drag());
        state.drag_to(600, 400);
        CHECK(state.end_drag(true));
    }
}

TEST_CASE("Root extent changes reconcile only the workarea-dependent interactions", "[hotplug][monitor][workarea][drag]")
{
    auto state = test::state();
    test::add(state, 1);
    test::add(state, 2);
    state.insert_fixture(9, Fixture::Role::Dock, DockStrut{ .right = { 100 } });
    REQUIRE(state.monitors()[0].working_area().width == 900);
    bool split = true;
    SECTION("split drag depends on its captured workarea")
    {
        auto hit = state.split_at(450, 400);
        REQUIRE(hit);
        state.begin_drag(*hit, 450, 400, 1);
    }
    SECTION("floating drag retains its unchanged window context")
    {
        split = false;
        state.floating(1, true);
        state.begin_drag(State::Grip{ 1, floating::ResizeEdge::None }, 100, 100, 1);
    }
    state.settle();
    auto revision = state.revision();
    // This extent does not move the reservation's right boundary.
    state.replace_topology({ { test::output("M0") }, { 0, 0, 1000, 900 } });
    CHECK(state.revision() == revision);
    state.settle();
    REQUIRE(state.drag());
    // Enlarging the root moves its right edge beyond the unchanged output.
    state.replace_topology({ { test::output("M0") }, { 0, 0, 1200, 900 } });
    CHECK(state.monitors()[0].working_area().width == 1000);
    CHECK(state.revision() > revision);
    state.settle();
    CHECK(bool(state.drag()) == !split);
}
