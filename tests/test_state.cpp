#include "lwm/core/floating.hpp"
#include "lwm/core/state.hpp"
#include "lwm/core/invariants.hpp"
#include "state_fixture.hpp"
#include <catch2/catch_test_macros.hpp>

using namespace lwm;
using test::add;
using test::add_floating;

namespace {
std::optional<Geometry> shown(State::Projected const& projected)
{
    return projected.presentation.transform(&State::Presentation::geometry);
}
}

TEST_CASE("Urgency tracks app and WM sources independently", "[state][urgency]")
{
    Urgency urgency;
    REQUIRE_FALSE(urgency.active());
    urgency.set(UrgencySource::App, true);
    urgency.set(UrgencySource::WmInitiated, true);
    urgency.set(UrgencySource::App, false);
    REQUIRE_FALSE(urgency.has(UrgencySource::App));
    REQUIRE(urgency.has(UrgencySource::WmInitiated));
    REQUIRE(urgency.active());
    urgency.set(UrgencySource::WmInitiated, false);
    REQUIRE_FALSE(urgency.active());
}

TEST_CASE("The active client never becomes urgent", "[state][urgency]")
{
    auto state = test::state();
    add(state, 1);
    add(state, 2);
    state.focus(2);
    WindowStates attention;
    attention.set(WindowState::DemandsAttention, true);
    state.request_states(2, StateChange::Add, attention);
    state.hint_urgency(2, true);
    state.urgency(2, UrgencySource::WmInitiated, true);
    CHECK_FALSE(state.require(2).urgency.active());
    state.request_states(1, StateChange::Add, attention);
    CHECK(state.require(1).urgency.has(UrgencySource::App));
    state.focus(1);
    state.settle();
    CHECK_FALSE(state.require(1).urgency.active());
}

TEST_CASE("Registration attaches tiles and removal releases every membership", "[state][registry]")
{
    auto state = test::state();
    test::configure(state, [](Config& config) { config.scratchpads = { { .name = "term" } }; });
    add(state, 1);
    add_floating(state, 2);
    add(state, 3);
    auto const& workspace = state.monitors()[0].workspaces[0];
    CHECK(workspace.windows == std::vector<xcb_window_t>{ 1, 3 });
    test::focus(state, 3);
    state.claim_scratchpad(3, ScratchpadConfig{ .name = "term" });
    state.pool_scratchpad(2);
    state.erase(3);
    state.settle();
    CHECK(workspace.windows == std::vector<xcb_window_t>{ 1 });
    CHECK(workspace.preferred_tile == XCB_NONE);
    CHECK(state.active_window() == 1);
    CHECK(state.named_scratchpad("term")->claimed_window() == XCB_NONE);
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
    auto expected = state.normal_geometry(state.require(2));
    state.floating(2, true);
    auto const& client = state.require(2);
    REQUIRE(!client.tiled());
    CHECK(floating_mode(client)->geometry == expected);
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
    SECTION("A hidden tile derives its normal rectangle without ever being arranged")
    {
        add(state, 4, { .workspace = 1, .geometry = { -5000, 10, 200, 100 } });
        auto normal = state.normal_geometry(state.require(4));
        state.floating(4, true);
        CHECK(floating_mode(state.require(4))->geometry == normal);
    }
}

TEST_CASE("Tile return slots follow output identity and expire with their workspace", "[state][tile-slot][hotplug]")
{
    auto state = test::state(2);
    for (xcb_window_t id : { 1, 2, 3 }) add(state, id);
    state.floating(2, true);
    REQUIRE(floating_mode(state.require(2))->tile_slot == TileSlot{ 1, "M0", 0 });
    SECTION("Reordering outputs preserves the original slot")
    {
        test::outputs(state, { test::output("M1"), test::output("M0", 1000) });
        REQUIRE(state.require(2).monitor == 1);
        state.floating(2, false);
        CHECK(state.monitors()[1].current().windows == std::vector<xcb_window_t>{ 1, 2, 3 });
    }
    SECTION("Removal never lends the slot to the replacement output")
    {
        test::outputs(state, { test::output("M1") });
        CHECK_FALSE(floating_mode(state.require(2))->tile_slot);
        state.floating(2, false);
        CHECK(state.monitors()[0].current().windows == std::vector<xcb_window_t>{ 1, 3, 2 });
    }
    SECTION("A slot expires even while its client lives on another output")
    {
        state.relocate(2, 1, 0);
        test::outputs(state, { test::output("M1") });
        CHECK_FALSE(floating_mode(state.require(2))->tile_slot);
        test::outputs(state, { test::output("M1"), test::output("M0", 1000) });
        add(state, 4, { .monitor = 1 });
        add(state, 5, { .monitor = 1 });
        state.relocate(2, 1, 0);
        state.floating(2, false);
        CHECK(state.monitors()[1].current().windows == std::vector<xcb_window_t>{ 4, 5, 2 });
    }
    SECTION("Visiting another workspace does not erase the original identity")
    {
        state.relocate(2, 1, 1);
        test::outputs(state, { test::output("M1"), test::output("M0", 1000) });
        state.relocate(2, 1, 0);
        state.floating(2, false);
        CHECK(state.monitors()[1].current().windows == std::vector<xcb_window_t>{ 1, 2, 3 });
    }
}

TEST_CASE("Type and transient updates change only default modes", "[state][mode]")
{
    auto state = test::state();
    add(state, 1);
    state.transient(1, 99);
    CHECK(!state.require(1).tiled());
    state.transient(1, XCB_NONE);
    CHECK(state.require(1).tiled());
    state.floating(1, false);
    state.window_type(1, WindowType::Dialog);
    CHECK(state.require(1).tiled());
    add(state, 2);
    state.pool_scratchpad(2);
    state.window_type(2, WindowType::Dialog);
    CHECK(state.require(2).tiled());
    // Runtime conversion into a dock type has no normal default.
    add(state, 3);
    state.window_type(3, WindowType::Dock);
    CHECK(state.require(3).tiled());
}

TEST_CASE("Visibility is derived from workspace, iconic and sticky state", "[state][visibility]")
{
    auto state = test::state(2);
    add(state, 1);
    auto const& client = state.require(1);
    CHECK(state.visible(client));
    test::iconic(state, 1, true);
    CHECK_FALSE(state.visible(client));
    test::iconic(state, 1, false);
    state.relocate(1, 0, 1);
    CHECK_FALSE(state.visible(client));
    state.sticky(1, true);
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
    CHECK(state.fullscreen_owners().at(0) == 2);
    CHECK_FALSE(state.visible(state.require(1)));
    CHECK_FALSE(state.visible(state.require(3)));
    // A direct transient of the owner stays visible.
    state.transient(4, 2);
    CHECK(state.visible(state.require(4)));
    // Re-entering fullscreen or restoring a minimized fullscreen client reclaims.
    state.request_fullscreen(1);
    CHECK(state.fullscreen_owners().at(0) == 1);
    test::iconic(state, 2, true);
    test::iconic(state, 2, false);
    CHECK(state.fullscreen_owners().at(0) == 2);
    test::iconic(state, 2, true);
    CHECK(state.fullscreen_owners().at(0) == 1);
    state.relocate(1, 0, 1);
    CHECK(state.fullscreen_owners().at(0) == XCB_NONE);
    state.sticky(1, true);
    CHECK(state.fullscreen_owners().at(0) == 1);
    CHECK(state.fullscreen_owners() == std::vector<xcb_window_t>{ 1, XCB_NONE });
    // Fullscreen supersedes maximize.
    state.maximize(3, true, true);
    state.fullscreen(3, true);
    CHECK_FALSE(state.require(3).maximized_horz);
    state.maximize(3, true, false);
    CHECK_FALSE(state.require(3).maximized_horz);
}

TEST_CASE(
    "Fullscreen exemptions follow managed ancestry without changing view eligibility",
    "[state][fullscreen][ancestry]"
)
{
    auto state = test::state(2);
    add(state, 1);
    add_floating(state, 2);
    add_floating(state, 3);
    add_floating(state, 4);
    state.transient(2, 1);
    state.transient(3, 2);
    state.fullscreen(1, true);
    CHECK(state.visible(state.require(3)));
    CHECK(state.focusable(state.require(3)));
    CHECK_FALSE(state.visible(state.require(4)));

    SECTION("Minimized descendants stay hidden")
    {
        test::iconic(state, 3, true);
        CHECK_FALSE(state.visible(state.require(3)));
        test::iconic(state, 3, false);
        CHECK(state.visible(state.require(3)));
    }
    SECTION("Off-workspace descendants need their own sticky preference")
    {
        state.relocate(3, 0, 1);
        CHECK_FALSE(state.visible(state.require(3)));
        state.sticky(3, true);
        CHECK(state.visible(state.require(3)));
    }
    SECTION("Intermediate visibility does not rewrite ancestry")
    {
        test::iconic(state, 2, true);
        CHECK(state.visible(state.require(3)));
        state.relocate(2, 1, 1);
        CHECK(state.visible(state.require(3)));
    }
    SECTION("Each monitor applies its own owner")
    {
        state.relocate(4, 1, 0);
        state.fullscreen(4, true);
        state.relocate(3, 1, 0);
        CHECK_FALSE(state.visible(state.require(3)));
        state.transient(2, 4);
        CHECK(state.visible(state.require(3)));
    }
    SECTION("Removing an intermediate parent breaks the exemption")
    {
        state.erase(2);
        CHECK_FALSE(state.visible(state.require(3)));
    }
    SECTION("Reparenting an intermediate node updates descendants immediately")
    {
        state.transient(2, 4);
        CHECK_FALSE(state.visible(state.require(3)));
        state.transient(4, 1);
        CHECK(state.visible(state.require(3)));
    }
}

TEST_CASE("Fullscreen ancestry terminates on missing parents and cycles", "[state][fullscreen][ancestry]")
{
    auto state = test::state();
    for (xcb_window_t id : { 1, 2, 3, 4 }) add_floating(state, id);
    state.fullscreen(1, true);
    state.transient(4, 3);
    state.transient(3, 2);
    SECTION("Missing parent") { state.transient(2, 99); }
    SECTION("Self-link") { state.transient(2, 2); }
    SECTION("Cycle unrelated to the owner") { state.transient(2, 3); }
    CHECK(state.suppressed(state.require(4)));
    state.fullscreen(1, false);
    CHECK_FALSE(state.suppressed(state.require(4)));
    state.fullscreen(1, true);
    // Even a cycle containing the owner grants an exemption when the walk
    // reaches it. Hints remain intact; stacking resolves ordering separately.
    state.transient(2, 1);
    state.transient(1, 4);
    CHECK_FALSE(state.suppressed(state.require(4)));
    CHECK(state.require(1).transient_for == 4);
}

TEST_CASE(
    "Fullscreen ancestry handles long chains and disconnected cycles iteratively",
    "[state][fullscreen][ancestry]"
)
{
    auto state = test::state();
    constexpr xcb_window_t count = 2048;
    for (xcb_window_t id = 1; id <= count; ++id) add_floating(state, id);
    for (xcb_window_t id = 2; id <= count; ++id) state.transient(id, id - 1);
    state.fullscreen(1, true);
    auto revision = state.revision();
    auto fullscreen = state.fullscreen_visibility();
    for (auto const& [id, client] : state.clients()) CHECK(state.visible(client, fullscreen));
    CHECK(state.revision() == revision);

    // A long cycle with no route to the owner grants no exemption.
    state.transient(2, count);
    fullscreen = state.fullscreen_visibility();
    for (auto const& [id, client] : state.clients()) CHECK(state.visible(client, fullscreen) == (id == 1));
}

TEST_CASE("Fullscreen assignments preserve history while requests renew it", "[state][fullscreen]")
{
    auto state = test::state();
    add(state, 1);
    add(state, 2);
    state.fullscreen(1, true);
    state.fullscreen(2, true);
    SECTION("Visible") { }
    SECTION("Off workspace") { state.switch_workspace(0, 1); }
    SECTION("Hidden") { test::iconic(state, 1, true); }
    auto claims = [&] { return std::pair{ state.require(1).fullscreen_claim, state.require(2).fullscreen_claim }; };
    auto before = claims();
    CHECK(before.first < before.second);
    state.settle();
    auto revision = state.revision();
    state.fullscreen(2, true);
    state.fullscreen(1, true);
    CHECK(claims() == before);
    CHECK(state.revision() == revision);
    CHECK_FALSE(state.settle());

    state.request_fullscreen(1);
    CHECK(claims().first > claims().second);
    CHECK(state.revision() > revision);
    state.settle();
    CHECK(state.active_window() == (state.focusable(state.require(1)) ? 1
                                  : state.focusable(state.require(2)) ? 2 : XCB_NONE));
    // A request changes priority, not placement or hiding.
    state.switch_workspace(0, 0);
    if (state.require(1).iconic)
        CHECK(state.fullscreen_owners().at(0) == 2);
    test::iconic(state, 1, false);
    CHECK(state.fullscreen_owners().at(0) == 1);

    state.fullscreen(1, false);
    CHECK(state.fullscreen_owners().at(0) == 2);
    state.settle();
    revision = state.revision();
    state.fullscreen(1, false);
    CHECK(state.revision() == revision);
    CHECK_FALSE(state.settle());
    state.fullscreen(1, true);
    CHECK(state.fullscreen_owners().at(0) == 1);
}

TEST_CASE("Fullscreen admission establishes priority and excludes maximize", "[state][fullscreen]")
{
    auto state = test::state();
    add(state, 1);
    state.request_fullscreen(1);
    Client client;
    client.id = 2;
    client.fullscreen_claim = 1;
    client.maximized_horz = client.maximized_vert = true;
    state.insert(client);
    CHECK(state.fullscreen_owners().at(0) == 2);
    CHECK_FALSE(state.require(2).maximized_horz);
    CHECK_FALSE(state.require(2).maximized_vert);
    // Applying an initial rule does not add a second claim for admission.
    auto revision = state.revision();
    state.fullscreen(2, true);
    CHECK(state.revision() == revision);
    state.fullscreen(2, false);
    CHECK_FALSE(state.require(2).fullscreen());
    CHECK(state.fullscreen_owners().at(0) == 1);
}

TEST_CASE("Scratchpad names and the pool are the only membership records", "[state][scratchpad]")
{
    auto state = test::state();
    test::configure(state, [](Config& config) { config.scratchpads = { { .name = "a" }, { .name = "b" } }; });
    add(state, 1);
    add(state, 2);
    state.scratchpad_pending("a", true);
    CHECK(state.named_scratchpad("a")->pending_launch());
    state.pool_scratchpad(1);
    state.claim_scratchpad(1, ScratchpadConfig{ .name = "a" });
    CHECK_FALSE(state.pooled(1));
    CHECK(state.scratchpad_claim(1)->name == "a");
    state.pool_scratchpad(1);
    CHECK_FALSE(state.pooled(1));
    // Surviving names keep claims; removed names release and deiconify windows.
    state.claim_scratchpad(2, ScratchpadConfig{ .name = "b" });
    test::iconic(state, 2, true);
    test::configure(state, [](Config& config) { config.scratchpads = { { .name = "a" } }; });
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
    test::outputs(state, { test::output("M0") });
    auto const& tile = state.require(1);
    CHECK(tile.monitor == 0);
    CHECK(tile.workspace == 2);
    CHECK(state.monitors()[0].workspaces[2].windows == std::vector<xcb_window_t>{ 1 });
    CHECK(state.focused_monitor() == 0);
    // Displaced floating clients are centered; survivors are clamped.
    CHECK(floating_mode(state.require(2))->geometry == Geometry{ 400, 350, 200, 100 });
    CHECK(floating_mode(state.require(3))->geometry == Geometry{ 800, 10, 200, 100 });
}

TEST_CASE("Geometry derivation does not mutate the domain", "[state]")
{
    auto state = test::state();
    add(state, 1);
    auto revision = state.revision();
    state.freeze();
    state.project(state.fullscreen_visibility());
    state.normal_geometry(state.require(1));
    state.thaw();
    CHECK(state.revision() == revision);
    state.title(1, "changed");
    CHECK(state.revision() > revision);
    revision = state.revision();
    state.user_time(1, 0);
    CHECK(state.revision() == revision);
    // Repeating the current placement or mode is not a mutation.
    state.floating(1, false);
    revision = state.revision();
    state.relocate(1, 0, 0);
    state.relocate(1, 0, 0, State::RelocationGeometry::Preserve, 0);
    state.floating(1, false);
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

TEST_CASE("Tile geometry is a current projection independent of publication", "[state][geometry][layout]")
{
    auto state = test::state();
    test::configure(state, [](Config& config) { config.appearance = { .padding = 0, .border_width = 0 }; });
    for (xcb_window_t id : { 1, 2, 3 }) add(state, id);
    CHECK(state.normal_geometry(state.require(2)) == Geometry{ 500, 0, 500, 400 });
    test::iconic(state, 3, true);
    CHECK(state.normal_geometry(state.require(2)) == Geometry{ 500, 0, 500, 800 });
    state.ratio(0, SplitAddress{ 0 }, 0.25);
    CHECK(state.normal_geometry(state.require(2)) == Geometry{ 250, 0, 750, 800 });
    state.insert_fixture(99, Fixture::Role::Dock, DockStrut{ .top = { 100 } });
    CHECK(state.normal_geometry(state.require(2)) == Geometry{ 250, 100, 750, 700 });
    auto expected = state.normal_geometry(state.require(2));
    SECTION("Hidden") { state.switch_workspace(0, 1); }
    SECTION("Minimized") { test::iconic(state, 2, true); }
    SECTION("Fullscreen") { state.fullscreen(2, true); }
    SECTION("Shown")
    {
        auto clients = state.project(state.fullscreen_visibility());
        REQUIRE(clients.size() == 3);
        CHECK(clients[1].client->id == 2);
        CHECK(shown(clients[1]) == std::optional{ expected });
        CHECK_FALSE(clients[2].presentation);
    }
    CHECK(state.normal_geometry(state.require(2)) == expected);
    state.floating(2, true);
    CHECK(floating_mode(state.require(2))->geometry == expected);
}

TEST_CASE("Presentation draws borders inside the frames the model places", "[state][geometry]")
{
    auto state = test::state(); // 1000x800 monitor, padding 10, border 2
    add(state, 1);
    add_floating(state, 2);
    auto presented = [&](xcb_window_t id)
    {
        auto [geometry, border] = state.presentation(state.require(id));
        return std::pair{ geometry, border };
    };
    // Padding surrounds the tile frame on every side.
    CHECK(presented(1) == std::pair{ Geometry{ 10, 10, 976, 776 }, 2U });
    state.maximize(2, true, true);
    CHECK(presented(2) == std::pair{ Geometry{ 0, 0, 996, 796 }, 2U });
    state.fullscreen(2, true);
    CHECK(presented(2) == std::pair{ Geometry{ 0, 0, 1000, 800 }, 0U });
    test::configure(state, [](Config& config) { config.rules.push_back({ .actions = { .borderless = true } }); });
    CHECK(presented(1) == std::pair{ Geometry{ 10, 10, 980, 780 }, 0U });
}

TEST_CASE("Every presentation reconstructs its allocated frame even with oversized borders", "[state][geometry]")
{
    auto state = test::state();
    add_floating(state, 1);
    for (uint32_t width : { 0U, 1U, 2U, 32767U, 65535U })
    {
        test::configure(state, [&](Config& config) { config.appearance.border_width = width; });
        for (uint16_t x : { 1, 2, 3, 4, 9, 65535 })
            for (uint16_t y : { 1, 2, 3, 4, 9, 65535 })
            {
                state.geometry(1, { 10, 20, x, y });
                auto [window, border] = state.presentation(state.require(1));
                INFO("frame=" << x << "x" << y << " configured border=" << width);
                CHECK(window.width + 2 * border == x);
                CHECK(window.height + 2 * border == y);
                CHECK(window.width > 0);
                CHECK(window.height > 0);
            }
    }
    test::outputs(state, { { "M0", { 0, 0, 12, 3 } } });
    test::configure(state, [](Config& config) { config.appearance.padding = 0; });
    for (xcb_window_t id : { 2, 3, 4, 5 }) add(state, id);
    for (auto const& projected : state.project(state.fullscreen_visibility()))
    {
        REQUIRE(projected.presentation);
        auto [window, border] = *projected.presentation;
        auto frame = state.frame(*projected.client);
        CHECK(window.width + 2 * border == frame.width);
        CHECK(window.height + 2 * border == frame.height);
    }
}

TEST_CASE("Window size requests share clipped normal geometry across presentation states", "[state][geometry][requests]")
{
    auto state = test::state();
    add(state, 1, { .floating = true, .geometry = { 10, 20, 3, 3 } });
    SECTION("Normal") { }
    SECTION("Maximized") { state.maximize(1, true, true); }
    SECTION("Fullscreen") { state.fullscreen(1, true); }
    auto normal = [&] { return state.normal_geometry(state.require(1)); };
    state.moveresize_request(1, { .x = 30 });
    CHECK(normal() == Geometry{ 30, 20, 3, 3 });
    state.size_hints(1, { .width = 1, .height = 1 });
    CHECK(normal() == Geometry{ 30, 20, 3, 3 });
    state.moveresize_request(1, { .width = 9 });
    CHECK(normal() == Geometry{ 30, 20, 13, 5 }); // The unchanged inner height is one pixel.
    auto revision = state.revision();
    state.moveresize_request(1, { .width = 9 });
    CHECK(state.revision() == revision);
    state.size_hints(1, { .height = 7 });
    CHECK(normal() == Geometry{ 30, 20, 13, 11 });
}

TEST_CASE("A projection contains every client once with its final visible rectangle", "[state][geometry][projection]")
{
    auto state = test::state(2);
    test::configure(state, [](Config& config) { config.appearance = { .padding = 0, .border_width = 0 }; });
    add(state, 9);
    add_floating(state, 2);
    state.insert_fixture(99, Fixture::Role::Dock);
    add(state, 7, { .workspace = 1 });
    add(state, 4);
    add(state, 8, { .monitor = 1 });
    add(state, 6, { .workspace = 1 });
    test::iconic(state, 4, true);
    state.sticky(6, true);
    state.maximize(2, true, false);
    bool fullscreen = false;
    SECTION("Normal and maximized") { }
    SECTION("Fullscreen and its floating transient")
    {
        fullscreen = true;
        state.fullscreen(9, true);
        state.transient(2, 9);
        state.geometry(2, { 10, 10, 200, 100 });
    }
    state.freeze();
    auto clients = state.project(state.fullscreen_visibility());
    state.thaw();
    std::vector<xcb_window_t> order;
    for (auto const& projected : clients) order.push_back(projected.client->id);
    REQUIRE(order == std::vector<xcb_window_t>{ 9, 2, 7, 4, 8, 6 });
    CHECK(shown(clients[0]) == Geometry{ 0, 0, static_cast<uint16_t>(fullscreen ? 1000 : 500), 800 });
    CHECK(shown(clients[1]) == Geometry{ 0, 10, 1000, 100 });
    CHECK_FALSE(clients[2].presentation);
    CHECK_FALSE(clients[3].presentation);
    CHECK(shown(clients[4]) == Geometry{ 1000, 0, 1000, 800 });
    CHECK(shown(clients[5]) == (fullscreen ? std::nullopt : std::optional{ Geometry{ 500, 0, 500, 800 } }));
}

TEST_CASE("Workareas follow dock reservations and unchanged topology preserves intent", "[state][hotplug][workarea]")
{
    auto state = test::state(2);
    add(state, 1, { .floating = true, .geometry = { -100, -100, 1500, 1000 } });
    state.fullscreen_monitors(1, FullscreenMonitors{ 0, 0, 0, 1 });
    state.insert_fixture(9, Fixture::Role::Dock, DockStrut{ .top = { 40 } });
    CHECK(state.working_area(state.monitors()[0]) == Geometry{ 0, 40, 1000, 760 });
    CHECK(state.working_area(state.monitors()[1]) == Geometry{ 1000, 40, 1000, 760 });
    state.reserve(9, DockStrut{ .top = { 60 } });
    CHECK(state.working_area(state.monitors()[1]) == Geometry{ 1000, 60, 1000, 740 });
    // Desktop windows never reserve space.
    state.insert_fixture(10, Fixture::Role::Desktop, DockStrut{ .left = { 300 } });
    CHECK(state.working_area(state.monitors()[0]).x == 0);
    state.erase(9);
    CHECK(state.working_area(state.monitors()[0]) == state.monitors()[0].geometry);

    // A spurious refresh of the same outputs keeps intentional geometry and monitor hints.
    test::outputs(state, { test::output("M0"), test::output("M1", 1000) });
    CHECK(floating_mode(state.require(1))->geometry == Geometry{ -100, -100, 1500, 1000 });
    CHECK(state.require(1).fullscreen_monitors);
    // A changed topology invalidates index-based hints and fits the rectangle.
    test::outputs(state, { test::output("M0") });
    CHECK_FALSE(state.require(1).fullscreen_monitors);
    CHECK(floating_mode(state.require(1))->geometry == Geometry{ 0, 0, 1500, 1000 });
}

TEST_CASE("Admission derives roles and placement from observations alone", "[state][admission]")
{
    auto state = test::state(1, 3);
    WindowObservation dock{ .id = 1, .type = WindowType::Dock, .strut = { .top = { 30 } } };
    WindowObservation popup{ .id = 2, .type = WindowType::Tooltip };
    state.admit(dock);
    state.admit(popup);
    REQUIRE(state.find_fixture(1));
    CHECK(state.working_area(state.monitors()[0]).y == 30);
    CHECK_FALSE(state.find(2));
    CHECK_FALSE(state.find_fixture(2));

    WindowObservation parent{ .id = 3, .desktop = 2 };
    state.admit(parent);
    CHECK(state.require(3).workspace == 2);
    CHECK(state.require(3).desktop_pinned);

    // A transient joins its managed parent's workspace in every mode, as a
    // later WM_TRANSIENT_FOR change would.
    test::configure(state, [](Config& config) { config.rules.push_back({ .transient = true, .actions = { .floating = false } }); });
    WindowObservation tiled_child{ .id = 4, .transient_for = 3 };
    WindowObservation floating_child{ .id = 5, .transient_for = 3, .geometry = Geometry{ 0, 0, 200, 100 } };
    state.admit(tiled_child);
    CHECK(state.require(4).tiled());
    CHECK(state.require(4).workspace == 2);
    test::configure(state, [](Config& config) { config.rules.clear(); });
    state.admit(floating_child);
    REQUIRE(floating_mode(state.require(5)));
    CHECK(state.require(5).workspace == 2);
    // Centered on the parent's presentation: the left tile of the parent workspace.
    auto parent_area = state.frame(state.require(3));
    auto const& placed = floating_mode(state.require(5))->geometry;
    CHECK(placed.x + placed.width / 2 == parent_area.x + parent_area.width / 2);

    // Applications cannot start minimized: only LWM hides windows.
    WindowObservation hinted{ .id = 6, .states = [] {
        WindowStates states;
        states.set(WindowState::Fullscreen);
        states.set(WindowState::Hidden);
        return states;
    }() };
    state.admit(hinted);
    CHECK(state.require(6).fullscreen());
    CHECK_FALSE(state.require(6).iconic);
    for (auto const& [id, client] : state.clients()) CHECK(client.fullscreen_claim <= state.require(6).fullscreen_claim);
}

TEST_CASE("Button presses choose bindings before click focus and split gestures", "[state][drag][input]")
{
    auto state = test::state(); // Two tiles split at x=500 by default padding
    add(state, 1);
    add(state, 2);
    test::focus(state, 2);
    uint16_t const super = XCB_MOD_MASK_4;
    test::configure(state, [&](Config& config) {
        config.mousebinds = { { super, 1, MouseGrip::Move }, { super, 3, MouseGrip::Resize },
                              { super, 2, Action{ action::ToggleFloat{ } } } };
    });
    auto split = [](State::Press const& press)
    { return press.interaction && std::holds_alternative<State::SplitHit>(*press.interaction); };

    // An ordinary click focuses its client and is replayed to it.
    auto click = state.press(1, 100, 100, 1, 0, 10);
    CHECK_FALSE(click.consumed);
    CHECK_FALSE(click.interaction);
    CHECK(state.active_window() == 1);
    // Bindings consume the press; a tiled resize binding prefers the split under it.
    CHECK(split(state.press(1, 500, 400, 3, super, 20)));
    auto move = state.press(1, 100, 100, 1, super | XCB_MOD_MASK_LOCK, 30);
    REQUIRE(move.interaction);
    CHECK(std::get<State::Grip>(*move.interaction).window == 1);
    // A plain gap click resizes; a quick second click or Ctrl click resets the split.
    state.ratio(0, SplitAddress{ 0 }, 0.3);
    REQUIRE(split(state.press(XCB_NONE, 310, 400, 1, 0, 1000)));
    CHECK(state.monitors()[0].current().split_ratios.contains(SplitAddress{ 0 }));
    CHECK_FALSE(state.press(XCB_NONE, 310, 400, 1, 0, 1100).interaction);
    CHECK_FALSE(state.monitors()[0].current().split_ratios.contains(SplitAddress{ 0 }));
    state.ratio(0, SplitAddress{ 0 }, 0.3);
    CHECK_FALSE(state.press(XCB_NONE, 310, 400, 1, XCB_MOD_MASK_CONTROL, 5000).interaction);
    CHECK_FALSE(state.monitors()[0].current().split_ratios.contains(SplitAddress{ 0 }));
    // A command binding focuses the clicked client, then returns its action.
    auto command = state.press(2, 700, 100, 2, super, 5500);
    CHECK(command.consumed);
    CHECK(command.action == Action{ action::ToggleFloat{ } });
    CHECK(state.active_window() == 2);
    // A client that refuses focus runs nothing; the command cannot reach window 2.
    state.focus_hints(1, false, false);
    auto refused = state.press(1, 100, 100, 2, super, 5600);
    CHECK(refused.consumed);
    CHECK_FALSE(refused.action);
    CHECK(state.active_window() == 2);
    state.focus_hints(1, true, false);
    state.focus(1);
    // Hidden clients swallow clicks without focusing.
    test::iconic(state, 2, true);
    CHECK(state.press(2, 700, 100, 1, 0, 6000).consumed);
    CHECK(state.active_window() == 1);
}

TEST_CASE("Adoption resolves the complete transient scene before centering", "[state][admission][placement][restart]")
{
    for (bool handoff : { false, true })
        for (bool reversed : { false, true })
            for (bool pinned : { false, true })
            {
                CAPTURE(handoff, reversed, pinned);
                auto state = test::state(2);
                test::configure(state, [](Config& config) {
                    config.rules = {
                        { .match = { .class_regex = std::regex("root") }, .actions = { .workspace = 2, .monitor = size_t{1} } },
                        { .match = { .class_regex = std::regex("tile") }, .actions = { .floating = false } },
                    };
                });
                auto saved = state.snapshot();
                std::vector<WindowObservation> scene{
                    { .id = 1, .wm_class = pinned ? "pinned" : "root", .desktop = pinned ? std::optional<uint32_t>{5} : std::nullopt },
                    { .id = 2, .transient_for = 1, .geometry = Geometry{ 0, 0, 200, 100 } },
                    { .id = 3, .wm_class = "tile", .transient_for = 2 },
                    { .id = 4, .desktop = 5 },
                    { .id = 5, .transient_for = 3, .geometry = Geometry{ 0, 0, 80, 60 } },
                };
                if (reversed)
                    std::ranges::reverse(scene);
                state.adopt(scene, handoff ? &saved : nullptr, std::nullopt);
                state.settle();
                for (auto id : { 1, 2, 3, 4, 5 })
                {
                    CHECK(state.require(id).monitor == 1);
                    CHECK(state.require(id).workspace == 2);
                }
                REQUIRE(state.monitors()[1].workspaces[2].windows.size() == 3);
                CHECK(center(state.frame(state.require(2))) == center(state.frame(state.require(1))));
                CHECK(center(state.frame(state.require(5))) == center(state.frame(state.require(3))));
                CHECK_FALSE(invariants::validate(state));
            }
}

TEST_CASE("Adoption bounds malformed transient chains without losing membership", "[state][admission][placement]")
{
    for (xcb_window_t parent : { xcb_window_t{XCB_NONE}, xcb_window_t{99}, xcb_window_t{1}, xcb_window_t{2} })
    {
        CAPTURE(parent);
        auto state = test::state();
        state.adopt({ { .id = 1, .transient_for = parent }, { .id = 2, .transient_for = 1 } }, nullptr, std::nullopt);
        state.settle();
        CHECK(state.clients().size() == 2);
        CHECK_FALSE(invariants::validate(state));
        CHECK(state.project(state.fullscreen_visibility()).size() == 2);
    }
}

TEST_CASE("Adopted newcomers anchor to saved intent without replaying the parent's rule", "[state][admission][restart][placement]")
{
    for (bool floating : { false, true })
    {
        CAPTURE(floating);
        auto source = test::state();
        test::add(source, 1, { .workspace = 1, .floating = floating, .geometry = { 100, 100, 300, 200 } });
        auto saved = source.snapshot();
        auto state = test::state();
        test::configure(state, [](Config& config) {
            config.rules = { { .match = { .class_regex = std::regex("parent") },
                               .actions = { .workspace = 2, .geometry = RuleGeometry{ .position = std::pair<int16_t, int16_t>{ 1, 2 }, .width = 400, .height = 300 } } } };
        });
        state.adopt({ { .id = 2, .transient_for = 1, .geometry = Geometry{ 0, 0, 100, 60 } },
                      { .id = 1, .wm_class = "parent" } }, &saved, std::nullopt);
        state.settle();
        CHECK(state.require(1).workspace == 1);
        CHECK(state.require(2).workspace == 1);
        CHECK(state.require(1).mode == source.require(1).mode);
        CHECK(center(state.frame(state.require(2))) == center(state.frame(state.require(1))));
        CHECK_FALSE(invariants::validate(state));
    }
}

TEST_CASE("Rule sizes use the same frame sizing as application requests", "[state][rules][geometry]")
{
    auto state = test::state();
    test::configure(state, [](Config& config) {
        config.rules = { { .match = { .title_regex = std::regex("small") }, .actions = { .geometry = RuleGeometry{ .width = 9 } } } };
    });
    add(state, 1, { .floating = true, .geometry = { 10, 20, 3, 3 } });
    state.title(1, "small");
    // A 1x1 window inside a clipped border becomes 9x1 inside the configured 2px border.
    auto frame = floating_mode(state.require(1))->geometry;
    CHECK(frame.width == 13);
    CHECK(frame.height == 5);
}

TEST_CASE("Rule positions are relative to the dock-reduced workarea", "[state][rules][geometry][workarea]")
{
    auto state = test::state(2);
    state.insert_fixture(9, Fixture::Role::Dock, DockStrut{ .top = { 40 } });
    test::configure(state, [](Config& config) {
        config.appearance.border_width = 0;
        config.rules = { { .match = { .title_regex = std::regex("placed") },
                           .actions = { .monitor = size_t{ 1 },
                                        .geometry = RuleGeometry{ .position = std::pair<int16_t, int16_t>{ 100, 200 },
                                                                  .width = 300, .height = 150 } } } };
    });
    add(state, 1, { .floating = true });
    state.title(1, "placed");
    CHECK(floating_mode(state.require(1))->geometry == Geometry{ 1100, 240, 300, 150 });
    // Reload reapplies the rule against the current workarea.
    state.reserve(9, DockStrut{ .top = { 60 } });
    test::configure(state, [](Config&) { });
    CHECK(floating_mode(state.require(1))->geometry == Geometry{ 1100, 260, 300, 150 });
}

TEST_CASE("Reload with fewer workspaces folds clients, claims and focus into the last one", "[state][reload][fullscreen]")
{
    auto state = test::state(1, 3);
    add(state, 1);
    add(state, 2, { .workspace = 1 });
    add(state, 3, { .workspace = 2 });
    add(state, 4, { .workspace = 2 });
    state.fullscreen(2, true);
    state.fullscreen(4, true);
    state.switch_workspace(0, 1);
    test::focus(state, 2);
    REQUIRE(state.active_window() == 2);
    test::configure(state, [](Config& config) { config.workspaces = test::names(1); });
    state.settle();
    REQUIRE(state.monitors()[0].workspaces.size() == 1);
    for (xcb_window_t id : { 1, 2, 3, 4 }) CHECK(state.require(id).workspace == 0);
    CHECK(state.monitors()[0].current_workspace == 0);
    CHECK(state.fullscreen_owners().at(0) == 4);
    CHECK(state.active_window() == 4);
    CHECK_FALSE(invariants::validate(state));
}

TEST_CASE("Initial geometry rules override hints after monitor relocation in both admission paths", "[state][admission][rules][placement]")
{
    for (bool batch : { false, true })
        for (int geometry_rule : { 0, 1, 2 })
        {
            CAPTURE(batch, geometry_rule);
            auto state = test::state(2);
            test::configure(state, [&](Config& config) {
                config.appearance.border_width = 0;
                RuleActions actions{ .workspace = 2, .monitor = size_t{1} };
                if (geometry_rule)
                    actions.geometry = RuleGeometry{
                        .position = geometry_rule == 1 ? std::optional{ std::pair<int16_t, int16_t>{ 100, 200 } } : std::nullopt,
                        .width = 210, .height = 140 };
                config.rules = { { .match = { .class_regex = std::regex("child") }, .actions = actions } };
            });
            WindowObservation parent{ .id = 1 };
            WindowObservation child{ .id = 2, .wm_class = "child", .transient_for = 1,
                                     .geometry = Geometry{ 0, 0, 200, 100 },
                                     .size_hints = { .user_position = std::pair<int16_t, int16_t>{ 120, 130 },
                                                     .width = 170, .height = 110 } };
            if (batch)
                state.adopt({ child, parent }, nullptr, std::nullopt);
            else
            {
                state.admit(parent);
                state.admit(child);
            }
            CHECK(state.require(2).monitor == 1);
            CHECK(state.require(2).workspace == 2);
            REQUIRE(floating_mode(state.require(2)));
            Geometry expected = geometry_rule == 0 ? Geometry{ 1415, 345, 170, 110 }
                : geometry_rule == 1 ? Geometry{ 1100, 200, 210, 140 } : Geometry{ 1395, 330, 210, 140 };
            CHECK(floating_mode(state.require(2))->geometry == expected);
            state.settle();
            CHECK_FALSE(invariants::validate(state));
        }
}

TEST_CASE("Ratio bounds apply to writes and every workspace after reload", "[state][reload][layout]")
{
    auto state = test::state(2);
    for (size_t monitor = 0; monitor < 2; ++monitor)
    {
        state.ratio(monitor, SplitAddress{ 0 }, 0.2);
        state.switch_workspace(monitor, 1);
        state.ratio(monitor, SplitAddress{ 0 }, 0.8);
    }
    test::configure(state, [](Config& config) { config.layout.min_ratio = 0.4; });
    for (auto const& monitor : state.monitors())
    {
        CHECK(monitor.workspaces[0].split_ratios.at(SplitAddress{ 0 }) == 0.4);
        CHECK(monitor.workspaces[1].split_ratios.at(SplitAddress{ 0 }) == 0.6);
    }
    state.ratio(0, SplitAddress{ 1 }, 0.1);
    CHECK(state.monitors()[0].current().split_ratios.at(SplitAddress{ 1 }) == 0.4);
    CHECK_FALSE(invariants::validate(state));
    // An outward adjustment at an implicit default must remain a no-op.
    state.reset_ratios(0);
    test::configure(state, [](Config& config) { config.layout.default_ratio = 0.4; });
    state.adjust_ratio(-0.1);
    CHECK(state.monitors()[0].current().split_ratios.empty());
}

TEST_CASE("Split double clicks identify their output and workspace", "[state][drag][input]")
{
    auto state = test::state(1, 2);
    add(state, 1);
    add(state, 2);
    add(state, 3, { .workspace = 1 });
    add(state, 4, { .workspace = 1 });
    state.ratio(0, SplitAddress{ 0 }, 0.3);
    auto first = state.press(XCB_NONE, 310, 400, 1, 0, 1000);
    REQUIRE(first.interaction);
    state.begin_drag(*first.interaction, 310, 400, 1);
    state.end_drag(true);
    state.settle();
    SECTION("A different workspace has its own split")
    {
        state.switch_workspace(0, 1);
    }
    SECTION("A replacement output can reuse the monitor index")
    {
        test::outputs(state, { test::output("replacement") });
    }
    state.ratio(0, SplitAddress{ 0 }, 0.3);
    state.settle();
    auto second = state.press(XCB_NONE, 310, 400, 1, 0, 1100);
    REQUIRE(second.interaction);
    CHECK(std::holds_alternative<State::SplitHit>(*second.interaction));
    CHECK(state.monitors()[0].current().split_ratios.at(SplitAddress{ 0 }) == 0.3);
    CHECK_FALSE(state.press(XCB_NONE, 310, 400, 1, 0, 1200).interaction);
    CHECK_FALSE(state.monitors()[0].current().split_ratios.contains(SplitAddress{ 0 }));
}
