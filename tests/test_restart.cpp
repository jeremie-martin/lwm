#include "lwm/core/restart.hpp"
#include "lwm/core/state.hpp"
#include "state_fixture.hpp"
#include <catch2/catch_test_macros.hpp>

using namespace lwm;
using test::add;
using test::add_floating;

namespace {

restart::Snapshot sample()
{
    restart::Snapshot snapshot;
    snapshot.focused_monitor = 1;
    snapshot.active = 0x200;
    snapshot.showing_desktop = true;
    snapshot.monitors = {
        { "M0",
         { 0, 0, 1000, 800 },
         1, 0,
         { { LayoutStrategy::Monocle,
         { { SplitAddress{ 0 }, 0.25 }, { SplitAddress{ 40 }, 0.75 } },
         { 0x100, 0x300 },
         0x300 },
         { } }                                       },
        { "M1", { 1000, 0, 1000, 800 }, 0, 0, { { } } }
    };
    snapshot.clients = { { 0x100, 0, 0, Client::Kind::Tiled, { 1, 2, 3, 4 }, Geometry{ -5, -6, 70, 80 }, { true, false, std::nullopt, LayerHint::Below }, 3, true, false },
                         { 0x200, 1, 0, Client::Kind::Floating, { -32768, 32767, 65535, 1 }, std::nullopt, { }, 0, false, true } };
    snapshot.clients[1].tile_slot = TileSlot{ 7, "output with spaces", 1 };
    snapshot.named_scratchpads = {
        { "tëxt with spaces",        0x200 },
        {                 "",        0x100 },
        {          "pending", std::nullopt }
    };
    snapshot.clients[1].fullscreen_monitors = FullscreenMonitors{ 0, 1, 0, 1 };
    snapshot.pool = { 0x300 };
    snapshot.fullscreen_claims = { 0x200, 0x100 };
    return snapshot;
}

} // namespace

TEST_CASE("Restart snapshots round-trip every field", "[restart][codec]")
{
    auto snapshot = sample();
    auto words = restart::encode(snapshot);
    REQUIRE(words.front() == restart::format);
    CHECK(restart::decode(words) == snapshot);
    CHECK(restart::decode(restart::encode({ })) == restart::Snapshot{ });
}

TEST_CASE("Restart decoding rejects other formats and malformed records", "[restart][codec]")
{
    auto words = restart::encode(sample());
    CHECK_FALSE(restart::decode({ }));
    auto other = words;
    for (auto format : { restart::format - 1, restart::format + 1 })
    {
        other[0] = format;
        CHECK_FALSE(restart::decode(other));
    }
    // Every truncation and any trailing word is rejected rather than partially applied.
    for (size_t size = 0; size < words.size(); ++size)
    {
        CAPTURE(size);
        CHECK_FALSE(restart::decode(std::span(words).first(size)));
    }
    auto longer = words;
    longer.push_back(0);
    CHECK_FALSE(restart::decode(longer));
    // Oversized counts cannot drive allocation.
    std::vector<uint32_t> huge{ restart::format, 0, 0, 0, 0xFFFFFFFF };
    CHECK_FALSE(restart::decode(huge));
    // Claims must be unique references to saved clients.
    for (auto claims : {
             std::vector<xcb_window_t>{ 0x100, 0x100 },
             std::vector<xcb_window_t>{ XCB_NONE },
             std::vector<xcb_window_t>{ 0x999 }
    })
    {
        auto invalid = sample();
        invalid.fullscreen_claims = claims;
        CHECK_FALSE(restart::decode(restart::encode(invalid)));
    }
    // Out-of-range enumerations and ratios are rejected.
    auto snapshot = sample();
    snapshot.monitors[0].workspaces[0].ratios[SplitAddress{ 1 }] = 1.5;
    CHECK_FALSE(restart::decode(restart::encode(snapshot)));
}

TEST_CASE("Restart rejects tile slots on tiled clients or without output identity", "[restart][codec][tile-slot]")
{
    auto snapshot = sample();
    SECTION("Tiled client") { snapshot.clients[0].tile_slot = TileSlot{ 1, "M0", 0 }; }
    SECTION("Empty output") { snapshot.clients[1].tile_slot->output.clear(); }
    CHECK_FALSE(restart::decode(restart::encode(snapshot)));
}

TEST_CASE("Restored tile slots are admitted only while the original workspace exists", "[restart][state][tile-slot]")
{
    auto source = test::state();
    for (xcb_window_t id : { 1, 2, 3 }) add(source, id);
    source.floating(2, true);
    auto snapshot = restart::decode(restart::encode(source.snapshot()));
    REQUIRE(snapshot);
    REQUIRE(snapshot->find(2));
    auto const& saved = *snapshot->find(2);
    REQUIRE(saved.tile_slot == TileSlot{ 1, "M0", 0 });
    auto target = test::state();
    SECTION("Original workspace exists") { }
    SECTION("Original output disappeared") { target.replace_monitors({ test::monitor("replacement") }); }
    SECTION("Saved workspace no longer exists")
    {
        for (auto& record : snapshot->clients)
            if (record.window == 2)
                record.tile_slot->workspace = 99;
    }
    // Client records are in recency order, so find the floating client by ID.
    auto record = *snapshot->find(2);
    add(target, 1);
    add(target, 3);
    Client client;
    client.id = 2;
    client.mode = FloatingMode{ record.geometry, record.tile_slot };
    target.insert(client);
    target.floating(2, false);
    bool valid = target.monitors()[0].name == "M0" && record.tile_slot->workspace == 0;
    CHECK(target.monitors()[0].current().windows
          == (valid ? std::vector<xcb_window_t>{ 1, 2, 3 } : std::vector<xcb_window_t>{ 1, 3, 2 }));
}

TEST_CASE("State snapshots restore workspaces, order, recency and scratchpads", "[restart][state]")
{
    auto source = test::state(2);
    source.configure_scratchpads(std::vector<std::string>{ "term" });
    add(source, 1);
    add(source, 2);
    add(source, 3);
    add_floating(source, 4, 1);
    source.swap_tiles(0, 0, 2);
    source.layout(0, LayoutStrategy::Monocle);
    source.ratio(0, SplitAddress{ 0 }, 0.3);
    source.switch_workspace(1, 2);
    source.focus(1);
    source.focus(4);
    source.claim_scratchpad("term", 2);
    source.pool_scratchpad(3);
    source.skip_pager(4, true);
    auto snapshot = restart::decode(restart::encode(source.snapshot()));
    REQUIRE(snapshot);

    // The workspace graph is restored first; adoption then registers windows in
    // scan order before membership is restored.
    auto target = test::state(2);
    target.configure_scratchpads(std::vector<std::string>{ "term" });
    target.restore_workspaces(*snapshot);
    for (xcb_window_t id : { 3, 2, 1 }) add(target, id);
    auto const& saved = *snapshot->find(4);
    add(target, 4, { .monitor = saved.monitor, .workspace = saved.workspace, .floating = true, .geometry = saved.geometry });
    target.restore_membership(*snapshot);

    auto const& workspace = target.monitors()[0].workspaces[0];
    CHECK(workspace.windows == source.monitors()[0].workspaces[0].windows);
    CHECK(workspace.layout_strategy == LayoutStrategy::Monocle);
    CHECK(workspace.split_ratios.at(SplitAddress{ 0 }) == 0.3);
    CHECK(workspace.focused_window == 1);
    CHECK(target.monitors()[1].current_workspace == 2);
    CHECK(target.monitors()[1].previous_workspace == 0);
    CHECK(target.scratchpad_claim(2)->name == "term");
    CHECK(target.scratchpad_pool() == std::vector<xcb_window_t>{ 3 });
    CHECK(target.require(4).mru_order > target.require(1).mru_order);
    CHECK(saved.preferences.skip_pager == true);
}

TEST_CASE("Restart claim order is explicit in the wire format", "[restart][codec]")
{
    std::vector<uint32_t> words{
        7, 0,  0,   0,  1, // format, focus, active, desktop, monitor count
        1, 77,             // one-byte output name "M"
        0, 0,  100, 80,    // output geometry
        0, 0,  1,          // current, previous, workspace count
        0, 0,  0,   0,     // master-stack, no focus, ratios, tiles
        1,                 // client count
        7, 0,  0,   0,     // id, monitor, workspace, tiled
        0, 0,  100, 80,    // normal geometry
        0, 0,  0,   0,  0, // absent remembered floating geometry
        0, 0,  0,   0,     // unset preferences
        0, 0,  0,          // urgency, borderless, pinned
        0,                 // absent tile return slot
        0,                 // absent fullscreen monitor hint
        0, 0,              // named slots, pool
        1, 7               // oldest-to-newest fullscreen claims
    };
    auto decoded = restart::decode(words);
    REQUIRE(decoded);
    CHECK(decoded->fullscreen_claims == std::vector<xcb_window_t>{ 7 });
    CHECK(restart::encode(*decoded) == words);
}

TEST_CASE("Restart restores claim history independently of focus and adoption order", "[restart][state]")
{
    auto source = test::state();
    for (xcb_window_t id : { 1, 2, 3 }) add(source, id);
    for (xcb_window_t id : { 2, 3, 1 }) source.fullscreen(id, true);
    source.iconic(3, true);
    source.focus(2); // Focus recency is deliberately not fullscreen claim order.
    source.switch_workspace(0, 1);
    auto snapshot = source.snapshot();
    CHECK(snapshot.fullscreen_claims == std::vector<xcb_window_t>{ 2, 3, 1 });

    auto target = test::state();
    target.restore_workspaces(snapshot);
    for (xcb_window_t id : { 3, 1, 2 })
    {
        add(target, id);
        target.fullscreen(id, true);
    }
    target.iconic(3, true);
    SECTION("hidden and minimized candidates retain their order")
    {
        target.restore_membership(snapshot);
        CHECK(target.fullscreen_owner(0) == XCB_NONE);
        target.switch_workspace(0, 0);
        CHECK(target.fullscreen_owner(0) == 1);
        target.fullscreen(1, false);
        CHECK(target.fullscreen_owner(0) == 2);
        target.iconic(3, false);
        CHECK(target.fullscreen_owner(0) == 3);
        target.request_fullscreen(2);
        CHECK(target.fullscreen_owner(0) == 2);
    }
    SECTION("missing clients are skipped and new arrivals retain newer claims")
    {
        target.erase(1);
        target.fullscreen(3, false); // The application withdrew this saved claim.
        add(target, 4);
        target.fullscreen(4, true);
        target.restore_membership(snapshot);
        target.switch_workspace(0, 0);
        CHECK(target.fullscreen_owner(0) == 4);
        target.fullscreen(4, false);
        CHECK(target.fullscreen_owner(0) == 2);
        target.fullscreen(2, false);
        CHECK(target.fullscreen_owner(0) == XCB_NONE);
    }
}

TEST_CASE("Restart rebinding matches live output reconciliation", "[restart][state][hotplug]")
{
    auto source = test::state(3);
    source.switch_workspace(0, 1);
    source.layout(0, LayoutStrategy::Monocle);
    source.ratio(0, SplitAddress{ 0 }, 0.3);
    source.switch_workspace(1, 2);
    source.ratio(1, SplitAddress{ 0 }, 0.7);
    for (xcb_window_t id : { 1, 2, 3 }) add(source, id, { .workspace = 1 });
    add(source, 4, { .monitor = 1, .workspace = 2 });
    add(source, 5, { .monitor = 2, .workspace = 1 });
    add(source,
        6,
        {
            .monitor = 2,
            .workspace = 2,
            .floating = true,
            .geometry = { 2200, 80, 250, 150 }
    });
    source.focus(2);
    source.floating(2, true);
    source.focus(3);
    source.focus(4);
    source.fullscreen_monitors(6, FullscreenMonitors{ 0, 2, 0, 2 });
    auto snapshot = restart::decode(restart::encode(source.snapshot()));
    REQUIRE(snapshot);

    std::vector<Monitor> discovered;
    SECTION("Reordered outputs")
    {
        discovered = { test::monitor("M2"), test::monitor("M0", 1000), test::monitor("M1", 2000) };
    }
    SECTION("Removed first output") { discovered = { test::monitor("M1"), test::monitor("M2", 1000) }; }
    SECTION("Removed non-first outputs") { discovered = { test::monitor("M0") }; }
    SECTION("All outputs replaced") { discovered = { test::monitor("new") }; }
    SECTION("New output before survivors")
    {
        discovered = { test::monitor("new"),
                       test::monitor("M0", 1000),
                       test::monitor("M1", 2000),
                       test::monitor("M2", 3000) };
    }
    SECTION("Fewer workspaces") { discovered = { test::monitor("M1", 0, 1), test::monitor("M0", 1000, 1) }; }
    SECTION("Additional workspaces") { discovered = { test::monitor("M1", 0, 4), test::monitor("M0", 1000, 4) }; }
    for (auto& monitor : discovered) monitor.strut.top = 40;
    // Different new-workspace defaults ensure removed outputs cannot replace them.
    discovered[0].workspaces[0].layout_strategy = LayoutStrategy::Monocle;
    source.replace_monitors(discovered);

    State restored;
    restored.replace_monitors(discovered);
    restored.restore_workspaces(*snapshot);
    // Admission order deliberately differs from saved tile order.
    for (auto it = snapshot->clients.rbegin(); it != snapshot->clients.rend(); ++it)
    {
        Client client;
        client.id = it->window;
        client.monitor = it->monitor;
        client.workspace = it->workspace;
        client.mode = it->kind == Client::Kind::Tiled
            ? ClientMode{ TiledMode{ it->floating, it->geometry } }
            : ClientMode{ FloatingMode{ it->geometry, it->tile_slot } };
        client.fullscreen_monitors = it->fullscreen_monitors;
        restored.insert(std::move(client));
    }
    restored.restore_membership(*snapshot);
    CHECK(restored.focused_monitor() == source.focused_monitor());
    for (size_t m = 0; m < source.monitors().size(); ++m)
    {
        auto const& expected = source.monitors()[m];
        auto const& actual = restored.monitors()[m];
        CHECK(actual.name == expected.name);
        CHECK(actual.current_workspace == expected.current_workspace);
        CHECK(actual.previous_workspace == expected.previous_workspace);
        REQUIRE(actual.workspaces.size() == expected.workspaces.size());
        for (size_t w = 0; w < actual.workspaces.size(); ++w)
        {
            CHECK(actual.workspaces[w].windows == expected.workspaces[w].windows);
            CHECK(actual.workspaces[w].focused_window == expected.workspaces[w].focused_window);
            CHECK(actual.workspaces[w].layout_strategy == expected.workspaces[w].layout_strategy);
            CHECK(actual.workspaces[w].split_ratios == expected.workspaces[w].split_ratios);
        }
    }
    for (auto const& [id, expected] : source.clients())
    {
        auto const& actual = restored.require(id);
        CHECK(actual.monitor == expected.monitor);
        CHECK(actual.workspace == expected.workspace);
        CHECK(actual.fullscreen_monitors == expected.fullscreen_monitors);
        if (auto const* mode = floating_mode(expected))
        {
            REQUIRE(floating_mode(actual));
            CHECK(floating_mode(actual)->geometry == mode->geometry);
            CHECK(floating_mode(actual)->tile_slot == mode->tile_slot);
        }
    }
    // Continuation: a floating tile returns to the same slot, or appends if its
    // original output/workspace disappeared, on both paths.
    source.floating(2, false);
    restored.floating(2, false);
    auto const& tile = source.require(2);
    CHECK(
        restored.monitors()[tile.monitor].workspaces[tile.workspace].windows
        == source.monitors()[tile.monitor].workspaces[tile.workspace].windows
    );
}

TEST_CASE("Unchanged restart topology preserves intentional floating geometry and monitor hints", "[restart][state]")
{
    auto source = test::state();
    add(source,
        1,
        {
            .floating = true,
            .geometry = { -100, -100, 1500, 1000 }
    });
    source.fullscreen_monitors(1, FullscreenMonitors{ });
    auto snapshot = source.snapshot();
    auto before = snapshot.clients;
    snapshot.rebind(test::state().monitors());
    CHECK(snapshot.clients == before);
}

TEST_CASE("Restart preserves pending requests only for surviving scratchpad names", "[restart][state][scratchpad]")
{
    auto source = test::state();
    source.configure_scratchpads(std::vector<std::string>{ "pending", "removed", "claimed", "empty" });
    source.scratchpad_pending("pending", true);
    source.scratchpad_pending("removed", true);
    add(source, 1);
    source.claim_scratchpad("claimed", 1);
    auto snapshot = restart::decode(restart::encode(source.snapshot()));
    REQUIRE(snapshot);
    auto restored = test::state();
    restored.configure_scratchpads(std::vector<std::string>{ "pending", "claimed", "empty", "new" });
    SECTION("Claimed client survives") { add(restored, 1); }
    SECTION("Claimed client disappeared") { }
    restored.restore_membership(*snapshot);
    CHECK(restored.named_scratchpad("pending")->pending_launch());
    CHECK_FALSE(restored.named_scratchpad("removed"));
    CHECK_FALSE(restored.named_scratchpad("empty")->pending_launch());
    CHECK_FALSE(restored.named_scratchpad("new")->pending_launch());
    CHECK_FALSE(restored.named_scratchpad("claimed")->pending_launch());
    CHECK(restored.named_scratchpad("claimed")->window() == (restored.find(1) ? 1 : XCB_NONE));
    restored.scratchpad_pending("pending", false);
    CHECK_FALSE(restored.named_scratchpad("pending")->pending_launch());
}

TEST_CASE("Restart rejects ambiguous output identities and scratchpad ownership", "[restart][codec]")
{
    auto snapshot = sample();
    SECTION("Empty output name") { snapshot.monitors[0].name.clear(); }
    SECTION("Duplicate output name") { snapshot.monitors[1].name = snapshot.monitors[0].name; }
    SECTION("Invalid client output") { snapshot.clients[0].monitor = snapshot.monitors.size(); }
    SECTION("Invalid client workspace") { snapshot.clients[0].workspace = snapshot.monitors[0].workspaces.size(); }
    SECTION("Invalid focused output") { snapshot.focused_monitor = snapshot.monitors.size(); }
    SECTION("Duplicate scratchpad name") { snapshot.named_scratchpads[1].name = snapshot.named_scratchpads[0].name; }
    SECTION("Duplicate claim") { snapshot.named_scratchpads[1].window = snapshot.named_scratchpads[0].window; }
    SECTION("Claim without a client") { snapshot.named_scratchpads[1].window = 999; }
    SECTION("Claim with no window") { snapshot.named_scratchpads[1].window = XCB_NONE; }
    CHECK_FALSE(restart::decode(restart::encode(snapshot)));
}
