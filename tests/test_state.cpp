#include "lwm/core/floating.hpp"
#include "lwm/core/state.hpp"
#include "state_fixture.hpp"
#include <catch2/catch_test_macros.hpp>

using namespace lwm;
using test::add;
using test::add_floating;

TEST_CASE("Urgency tracks app and WM sources independently", "[state][urgency]")
{
    Urgency urgency;
    REQUIRE_FALSE(urgency.active());
    REQUIRE(urgency.add(UrgencySource::App));
    REQUIRE_FALSE(urgency.add(UrgencySource::App));
    REQUIRE(urgency.add(UrgencySource::WmInitiated));
    REQUIRE(urgency.remove(UrgencySource::App));
    REQUIRE_FALSE(urgency.has(UrgencySource::App));
    REQUIRE(urgency.has(UrgencySource::WmInitiated));
    REQUIRE(urgency.clear());
    REQUIRE_FALSE(urgency.active());
}

TEST_CASE("Registration attaches tiles and removal releases every membership", "[state][registry]")
{
    auto state = test::state();
    state.configure_scratchpads(std::vector<std::string>{ "term" });
    add(state, 1);
    add_floating(state, 2);
    add(state, 3);
    auto const& workspace = state.monitors()[0].workspaces[0];
    CHECK(workspace.windows == std::vector<xcb_window_t>{ 1, 3 });
    state.focus(3);
    state.claim_scratchpad("term", 3);
    state.pool_scratchpad(2);
    state.erase(3);
    CHECK(workspace.windows == std::vector<xcb_window_t>{ 1 });
    CHECK(workspace.focused_window == 1);
    CHECK(state.active_window() == XCB_NONE);
    CHECK(state.named_scratchpad("term")->window() == XCB_NONE);
    state.erase(2);
    CHECK(state.scratchpad_pool().empty());

    // Fixtures are listed and stacked but never join workspaces.
    state.insert_fixture(4, Fixture::Role::Dock);
    CHECK_FALSE(state.find(4));
    CHECK(state.find_fixture(4));
    state.erase(4);
    CHECK_FALSE(state.find_fixture(4));
}

TEST_CASE("Mode changes keep tile slots and one normal floating rectangle", "[state][mode]")
{
    auto state = test::state();
    for (xcb_window_t id : { 1, 2, 3 }) add(state, id);
    state.place_tile(2, { 100, 0, 400, 800 });
    state.floating(2, true);
    auto const& client = state.require(2);
    REQUIRE(client.kind() == Client::Kind::Floating);
    CHECK(floating_mode(client)->geometry == Geometry{ 100, 0, 400, 800 });
    state.geometry(2, { 50, 60, 300, 200 });
    state.floating(2, false);
    CHECK(state.monitors()[0].current().windows == std::vector<xcb_window_t>{ 1, 2, 3 });
    // The tile remembers the rectangle to restore when floated again.
    state.floating(2, true);
    CHECK(floating_mode(state.require(2))->geometry == Geometry{ 50, 60, 300, 200 });

    SECTION("A slot is honored only on the same workspace")
    {
        state.relocate(2, 0, 1);
        state.floating(2, false);
        CHECK(state.monitors()[0].workspaces[1].windows == std::vector<xcb_window_t>{ 2 });
    }
    SECTION("An unarranged tile's off-monitor rectangle is recovered onto the monitor")
    {
        add(state, 4, { .workspace = 1, .geometry = { -5000, 10, 200, 100 } });
        state.floating(4, true);
        CHECK(floating_mode(state.require(4))->geometry == Geometry{ 400, 350, 200, 100 });
    }
}

TEST_CASE("Type and transient updates change only default modes", "[state][mode]")
{
    auto state = test::state();
    add(state, 1);
    state.transient(1, 99);
    CHECK(state.require(1).kind() == Client::Kind::Floating);
    state.transient(1, XCB_NONE);
    CHECK(state.require(1).kind() == Client::Kind::Tiled);
    state.floating(1, false);
    state.window_type(1, WindowType::Dialog);
    CHECK(state.require(1).kind() == Client::Kind::Tiled);
    add(state, 2);
    state.pool_scratchpad(2);
    state.window_type(2, WindowType::Dialog);
    CHECK(state.require(2).kind() == Client::Kind::Tiled);
    // Runtime conversion into a dock type has no normal default.
    add(state, 3);
    state.window_type(3, WindowType::Dock);
    CHECK(state.require(3).kind() == Client::Kind::Tiled);
}

TEST_CASE("Visibility is derived from workspace, iconic, sticky and show-desktop state", "[state][visibility]")
{
    auto state = test::state(2);
    add(state, 1);
    auto const& client = state.require(1);
    CHECK(state.visible(client));
    state.iconic(1, true);
    CHECK_FALSE(state.visible(client));
    state.iconic(1, false);
    state.relocate(1, 0, 1);
    CHECK_FALSE(state.visible(client));
    state.sticky(1, true);
    CHECK(state.visible(client));
    // Sticky clients stay visible while the desktop is shown; others hide.
    state.show_desktop(true);
    CHECK(state.visible(client));
    state.sticky(1, false);
    CHECK_FALSE(state.in_view(client));
}

TEST_CASE("The most recent fullscreen claim in view owns its monitor", "[state][fullscreen]")
{
    auto state = test::state(2);
    for (xcb_window_t id : { 1, 2, 3 }) add(state, id);
    add_floating(state, 4);
    state.fullscreen(1, true);
    state.fullscreen(2, true);
    CHECK(state.fullscreen_owner(0) == 2);
    CHECK_FALSE(state.visible(state.require(1)));
    CHECK_FALSE(state.visible(state.require(3)));
    // A direct transient of the owner stays visible.
    state.transient(4, 2);
    CHECK(state.visible(state.require(4)));
    // Re-entering fullscreen or restoring a minimized fullscreen client reclaims.
    state.fullscreen(1, true);
    CHECK(state.fullscreen_owner(0) == 1);
    state.iconic(2, true);
    state.iconic(2, false);
    CHECK(state.fullscreen_owner(0) == 2);
    state.iconic(2, true);
    CHECK(state.fullscreen_owner(0) == 1);
    state.relocate(1, 0, 1);
    CHECK(state.fullscreen_owner(0) == XCB_NONE);
    state.sticky(1, true);
    CHECK(state.fullscreen_owner(0) == 1);
    CHECK(state.fullscreen_owners() == std::vector<xcb_window_t>{ 1, XCB_NONE });
    state.show_desktop(true);
    CHECK(state.fullscreen_owner(0) == XCB_NONE);
    // Fullscreen supersedes maximize.
    state.maximize(3, true, true);
    state.fullscreen(3, true);
    CHECK_FALSE(state.require(3).maximized_horz);
    state.maximize(3, true, false);
    CHECK_FALSE(state.require(3).maximized_horz);
}

TEST_CASE("Scratchpad names and the pool are the only membership records", "[state][scratchpad]")
{
    auto state = test::state();
    state.configure_scratchpads(std::vector<std::string>{ "a", "b" });
    add(state, 1);
    add(state, 2);
    state.scratchpad_pending("a", true);
    CHECK(state.named_scratchpad("a")->pending_launch());
    state.pool_scratchpad(1);
    state.claim_scratchpad("a", 1);
    CHECK_FALSE(state.pooled(1));
    CHECK(state.scratchpad_claim(1)->name == "a");
    state.pool_scratchpad(1);
    CHECK_FALSE(state.pooled(1));
    // Surviving names keep claims; removed names release visible windows.
    state.claim_scratchpad("b", 2);
    state.iconic(2, true);
    state.configure_scratchpads(std::vector<std::string>{ "a" });
    CHECK(state.scratchpad_claim(1));
    CHECK_FALSE(state.scratchpad_claim(2));
    CHECK_FALSE(state.require(2).iconic);
}

TEST_CASE("Topology replacement rebinds clients and fits floating rectangles", "[state][hotplug]")
{
    auto state = test::state(2);
    add(state, 1, { .monitor = 1, .workspace = 2 });
    add(state, 2, { .monitor = 1, .workspace = 1, .floating = true, .geometry = { 1200, 10, 200, 100 } });
    add(state, 3, { .monitor = 0, .floating = true, .geometry = { 900, 10, 200, 100 } });
    state.focus_monitor(1);
    state.replace_monitors({ test::monitor("M0") });
    auto const& tile = state.require(1);
    CHECK(tile.monitor == 0);
    CHECK(tile.workspace == 2);
    CHECK(state.monitors()[0].workspaces[2].windows == std::vector<xcb_window_t>{ 1 });
    CHECK(state.focused_monitor() == 0);
    // Displaced floating clients are centered; survivors are clamped.
    CHECK(floating_mode(state.require(2))->geometry == Geometry{ 400, 350, 200, 100 });
    CHECK(floating_mode(state.require(3))->geometry == Geometry{ 800, 10, 200, 100 });
}

TEST_CASE("Revision tracks domain mutations but not layout targets", "[state]")
{
    auto state = test::state();
    add(state, 1);
    auto revision = state.revision();
    state.place_tile(1, { 1, 2, 3, 4 });
    CHECK(state.revision() == revision);
    state.title(1, "changed");
    CHECK(state.revision() > revision);
    revision = state.revision();
    state.user_time(1, 0, XCB_NONE);
    CHECK(state.revision() == revision);
}

TEST_CASE("Maximize presentation preserves normal placement", "[state][floating]")
{
    Geometry normal{ 100, 120, 500, 360 }, area{ 0, 0, 1920, 1080 };
    CHECK(floating::presentation_geometry(normal, area, false, false) == normal);
    CHECK(floating::presentation_geometry(normal, area, true, false) == Geometry{ 0, 120, 1920, 360 });
    CHECK(floating::presentation_geometry(normal, area, false, true) == Geometry{ 100, 0, 500, 1080 });
    CHECK(floating::presentation_geometry(normal, area, true, true) == area);
}
