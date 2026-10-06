#include "lwm/core/invariants.hpp"
#include "lwm/core/restart.hpp"
#include "lwm/core/state.hpp"
#include "state_fixture.hpp"
#include <catch2/catch_test_macros.hpp>
#include <cstring>
#include <nlohmann/json.hpp>

using namespace lwm;
using test::add;
using test::add_floating;

namespace {

// The private handoff envelope is format, byte length, then zero-padded JSON.
std::vector<uint32_t> pack_json(std::string const& text)
{
    std::vector<uint32_t> words(2 + (text.size() + 3) / 4);
    words[0] = restart::format;
    words[1] = static_cast<uint32_t>(text.size());
    std::memcpy(words.data() + 2, text.data(), text.size());
    return words;
}

nlohmann::json unpack_json(std::vector<uint32_t> const& words)
{
    REQUIRE(words.size() >= 2);
    REQUIRE(words[0] == restart::format);
    REQUIRE(words[1] <= (words.size() - 2) * 4);
    auto data = reinterpret_cast<char const*>(words.data() + 2);
    return nlohmann::json::parse(data, data + words[1]);
}

restart::Snapshot sample()
{
    restart::Snapshot snapshot;
    snapshot.focused_monitor = 1;
    snapshot.active = 0x200;
    snapshot.showing_desktop = true;
    snapshot.monitors = { test::monitor("M0", 0, 2), test::monitor("M1", 1000, 1) };
    snapshot.monitors[0].current_workspace = 1;
    snapshot.monitors[0].workspaces[0] = {
        { 0x100 }, 0x100, LayoutStrategy::Monocle,
        { { SplitAddress{ 0 }, 0.25 }, { SplitAddress{ 40 }, 0.75 } }
    };
    snapshot.clients = {
        { 0x100,
         0, 0,
         TiledMode{ Geometry{ -5, -6, 70, 80 } },
         { true, false, std::nullopt, LayerHint::Below },
         3,  true,
         false                                                                            },
        { 0x200, 1, 0, FloatingMode{ { -32768, 32767, 65535, 1 } },     {}, 0, false, true }
    };
    snapshot.clients[1].mru_order = (uint64_t{ 1 } << 40) + 7;
    std::get<FloatingMode>(snapshot.clients[1].mode).tile_slot = TileSlot{ 7, "output with spaces", 1 };
    snapshot.named_scratchpads = {
        { "tëxt with spaces",        0x200 },
        {              "tile",        0x100 },
        {           "pending", std::nullopt }
    };
    snapshot.clients[1].fullscreen_monitors = FullscreenMonitors{ 0, 1, 0, 1 };
    snapshot.clients.push_back({ .id = 0x300, .monitor = 1, .mode = FloatingMode{ Geometry{ 20, 30, 80, 60 } } });
    snapshot.pool = { 0x300 };
    snapshot.clients[0].order = 2;
    snapshot.clients[1].order = 0;
    snapshot.clients[2].order = 1;
    snapshot.fixtures = { { 0x400, Fixture::Role::Dock, 3 } };
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
    CHECK(restart::decode(restart::encode({})) == restart::Snapshot{});
}

TEST_CASE("Restart decoding rejects other formats and malformed records", "[restart][codec]")
{
    auto words = restart::encode(sample());
    CHECK_FALSE(restart::decode({}));
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
    // Claims must reference distinct saved clients.
    for (auto claim : { xcb_window_t{ 0x200 }, xcb_window_t{ XCB_NONE }, xcb_window_t{ 0x999 } })
    {
        auto invalid = sample();
        invalid.fullscreen_claims[1] = claim;
        CHECK_FALSE(restart::decode(restart::encode(invalid)));
    }
    auto invalid_recency = sample();
    invalid_recency.clients[0].mru_order = invalid_recency.clients[1].mru_order;
    CHECK_FALSE(restart::decode(restart::encode(invalid_recency)));
    invalid_recency.clients[0].mru_order = UINT64_MAX;
    CHECK_FALSE(restart::decode(restart::encode(invalid_recency)));
    // Out-of-range enumerations and ratios are rejected.
    auto snapshot = sample();
    snapshot.monitors[0].workspaces[0].split_ratios[SplitAddress{ 1 }] = 1.5;
    CHECK_FALSE(restart::decode(restart::encode(snapshot)));
}

TEST_CASE("Restart rejects tile slots without output identity", "[restart][codec][tile-slot]")
{
    auto snapshot = sample();
    std::get<FloatingMode>(snapshot.clients[1].mode).tile_slot->output.clear();
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
    REQUIRE(std::get<FloatingMode>(saved.mode).tile_slot == TileSlot{ 1, "M0", 0 });
    auto target = test::state();
    SECTION("Original workspace exists") { }
    SECTION("Original output disappeared") { test::outputs(target, { test::output("replacement") }); }
    SECTION("Saved workspace no longer exists")
    {
        for (auto& record : snapshot->clients)
            if (record.id == 2)
                std::get<FloatingMode>(record.mode).tile_slot->workspace = 99;
    }
    // Client records are in recency order, so find the floating client by ID.
    auto record = *snapshot->find(2);
    add(target, 1);
    add(target, 3);
    Client client;
    client.id = 2;
    client.mode =
        FloatingMode{ std::get<FloatingMode>(record.mode).geometry, std::get<FloatingMode>(record.mode).tile_slot };
    target.insert(client);
    target.floating(2, false);
    bool valid = target.monitors()[0].name == "M0" && std::get<FloatingMode>(record.mode).tile_slot->workspace == 0;
    CHECK(
        target.monitors()[0].current().windows
        == (valid ? std::vector<xcb_window_t>{ 1, 2, 3 } : std::vector<xcb_window_t>{ 1, 3, 2 })
    );
}

TEST_CASE("State snapshots restore workspaces, order, recency and scratchpads", "[restart][state]")
{
    auto source = test::state(2);
    test::configure(source, [](Config& config) { config.scratchpads = { { .name = "term" } }; });
    add(source, 1);
    add(source, 2);
    add(source, 3);
    add_floating(source, 4, 1, 2);
    source.swap_tiles(0, 0, 2);
    source.layout(0, LayoutStrategy::Monocle);
    source.ratio(0, SplitAddress{ 0 }, 0.3);
    source.switch_workspace(1, 2);
    test::focus(source, 1);
    test::focus(source, 4);
    source.claim_scratchpad(2, ScratchpadConfig{ .name = "term" });
    source.pool_scratchpad(3);
    source.skip_pager(4, true);
    auto snapshot = restart::decode(restart::encode(source.snapshot()));
    REQUIRE(snapshot);

    auto target = test::state(2);
    test::configure(target, [](Config& config) { config.scratchpads = { { .name = "term" } }; });
    // Observation order differs from saved membership and registration order.
    target.adopt(
        { test::observe(source.require(3)), test::observe(source.require(2)),
          test::observe(source.require(1)), test::observe(source.require(4)) },
        &*snapshot
    );
    auto const& saved = *snapshot->find(4);

    auto const& workspace = target.monitors()[0].workspaces[0];
    CHECK(workspace.windows == source.monitors()[0].workspaces[0].windows);
    CHECK(workspace.layout_strategy == LayoutStrategy::Monocle);
    CHECK(workspace.split_ratios.at(SplitAddress{ 0 }) == 0.3);
    CHECK(workspace.preferred_tile == XCB_NONE);
    CHECK(target.monitors()[1].current_workspace == 2);
    CHECK(target.monitors()[1].previous_workspace == 0);
    CHECK(target.scratchpad_claim(2)->name == "term");
    CHECK(target.scratchpad_pool() == std::vector<xcb_window_t>{ 3 });
    for (auto const& [id, client] : source.clients()) CHECK(target.require(id).mru_order == client.mru_order);
    CHECK(saved.preferences.skip_pager == true);
}

TEST_CASE("Restart wire schema directly represents domain values", "[restart][codec]")
{
    // Independent literal input: no production encoder constructs this fixture.
    auto document = nlohmann::json::parse(R"({
        "focused_monitor": 0, "active": 0, "showing_desktop": false,
        "monitors": [{"name": "M", "geometry": {"x": 0, "y": 0, "width": 100, "height": 80},
            "current_workspace": 0, "previous_workspace": 0, "workspaces": [{"layout_strategy": "MasterStack", "split_ratios": [],
            "windows": [7], "preferred_tile": 0}]}],
        "clients": [{"id": 7, "monitor": 0, "workspace": 0,
            "mode": {"TiledMode": {"floating": null}},
            "preferences": {"floating": null, "skip_taskbar": null, "skip_pager": null, "layer": null},
            "urgency": {"sources": 0}, "borderless": false, "desktop_pinned": false, "fullscreen_monitors": null, "mru_order": 0, "order": 0}],
        "fixtures": [], "named_scratchpads": [], "pool": [], "fullscreen_claims": [7]
    })");
    auto decoded = restart::decode(pack_json(document.dump()));
    REQUIRE(decoded);
    CHECK(decoded->fullscreen_claims == std::vector<xcb_window_t>{ 7 });
    CHECK(unpack_json(restart::encode(*decoded)) == document);
}

TEST_CASE("Restart restores claim history independently of focus and adoption order", "[restart][state]")
{
    auto source = test::state();
    for (xcb_window_t id : { 1, 2, 3 }) add(source, id);
    for (xcb_window_t id : { 2, 3, 1 }) source.fullscreen(id, true);
    source.iconic(3, true);
    test::focus(source, 2); // Focus recency is deliberately not fullscreen claim order.
    source.switch_workspace(0, 1);
    auto snapshot = source.snapshot();
    CHECK(snapshot.fullscreen_claims == std::vector<xcb_window_t>{ 2, 3, 1 });

    auto target = test::state();
    std::vector<WindowObservation> observed{
        test::observe(source.require(3)), test::observe(source.require(1)), test::observe(source.require(2))
    };
    SECTION("hidden and minimized candidates retain their order")
    {
        target.adopt(observed, &snapshot);
        CHECK(target.fullscreen_owners().at(0) == XCB_NONE);
        target.switch_workspace(0, 0);
        CHECK(target.fullscreen_owners().at(0) == 1);
        target.fullscreen(1, false);
        CHECK(target.fullscreen_owners().at(0) == 2);
        target.iconic(3, false);
        CHECK(target.fullscreen_owners().at(0) == 3);
        target.request_fullscreen(2);
        CHECK(target.fullscreen_owners().at(0) == 2);
    }
    SECTION("missing clients are skipped and new arrivals retain newer claims")
    {
        std::erase_if(observed, [](auto const& client) { return client.id == 1; });
        observed.front().states.set(WindowState::Fullscreen, false); // The application withdrew this saved claim.
        target.adopt(observed, &snapshot);
        add(target, 4);
        target.fullscreen(4, true);
        target.switch_workspace(0, 0);
        CHECK(target.fullscreen_owners().at(0) == 4);
        target.fullscreen(4, false);
        CHECK(target.fullscreen_owners().at(0) == 2);
        target.fullscreen(2, false);
        CHECK(target.fullscreen_owners().at(0) == XCB_NONE);
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
    test::focus(source, 2);
    source.floating(2, true);
    test::focus(source, 3);
    test::focus(source, 4);
    source.fullscreen_monitors(6, FullscreenMonitors{ 0, 2, 0, 2 });
    auto snapshot = restart::decode(restart::encode(source.snapshot()));
    REQUIRE(snapshot);

    std::vector<Topology::Output> discovered;
    size_t workspaces = 3;
    SECTION("Reordered outputs")
    {
        discovered = { test::output("M2"), test::output("M0", 1000), test::output("M1", 2000) };
    }
    SECTION("Removed first output") { discovered = { test::output("M1"), test::output("M2", 1000) }; }
    SECTION("Removed non-first outputs") { discovered = { test::output("M0") }; }
    SECTION("All outputs replaced") { discovered = { test::output("new") }; }
    SECTION("New output before survivors")
    {
        discovered = { test::output("new"), test::output("M0", 1000), test::output("M1", 2000), test::output("M2", 3000) };
    }
    SECTION("Fewer workspaces")
    {
        discovered = { test::output("M1"), test::output("M0", 1000) };
        workspaces = 1;
    }
    SECTION("Additional workspaces")
    {
        discovered = { test::output("M1"), test::output("M0", 1000) };
        workspaces = 4;
    }
    // Both paths see the same dock reservation. A different new-workspace
    // default ensures removed outputs cannot replace fresh workspaces.
    auto prepare = [&](State& state, size_t count)
    {
        test::configure(state, [&](Config& config) {
            config.workspaces.count = count;
            config.layout.strategy = LayoutStrategy::Monocle;
        });
        test::outputs(state, discovered);
    };
    // The live workspace count is fixed, so live reconciliation uses the saved count.
    source.insert_fixture(99, Fixture::Role::Dock, DockStrut{ .top = { 40 } });
    prepare(source, 3);

    State restored;
    prepare(restored, workspaces);
    // Observation order deliberately differs from saved tile order. Private
    // placement is restored even though these observations carry new indices.
    std::vector<WindowObservation> observed{ { .id = 99, .type = WindowType::Dock, .strut = { .top = { 40 } } } };
    for (auto it = snapshot->clients.rbegin(); it != snapshot->clients.rend(); ++it)
        observed.push_back(test::observe(source.require(it->id)));
    restored.adopt(observed, &*snapshot);
    CHECK(restored.focused_monitor() == source.focused_monitor());
    for (auto const& monitor : restored.monitors())
        CHECK(monitor.strut.top == 40);
    if (workspaces == 1)
    {
        // Retain the survivor's order, fold higher workspaces in order, then
        // append displaced output members.
        CHECK(restored.monitors()[0].current().windows == std::vector<xcb_window_t>{ 4, 5 });
        CHECK(restored.monitors()[1].current().windows == std::vector<xcb_window_t>{ 1, 3 });
        CHECK(restored.require(4).monitor == 0);
        CHECK(restored.require(5).monitor == 0);
        CHECK(restored.require(1).monitor == 1);
        for (auto const& [id, client] : restored.clients()) CHECK(client.workspace == 0);
        // The tile slot's workspace no longer exists, so the client appends.
        restored.floating(2, false);
        CHECK(restored.monitors()[1].current().windows == std::vector<xcb_window_t>{ 1, 3, 2 });
        return;
    }
    for (size_t m = 0; m < source.monitors().size(); ++m)
    {
        auto const& expected = source.monitors()[m];
        auto const& actual = restored.monitors()[m];
        CHECK(actual.name == expected.name);
        CHECK(actual.current_workspace == expected.current_workspace);
        CHECK(actual.previous_workspace == expected.previous_workspace);
        REQUIRE(actual.workspaces.size() == workspaces);
        for (size_t w = 0; w < expected.workspaces.size(); ++w)
        {
            CHECK(actual.workspaces[w].windows == expected.workspaces[w].windows);
            CHECK(actual.workspaces[w].preferred_tile == expected.workspaces[w].preferred_tile);
            CHECK(actual.workspaces[w].layout_strategy == expected.workspaces[w].layout_strategy);
            CHECK(actual.workspaces[w].split_ratios == expected.workspaces[w].split_ratios);
        }
        for (size_t w = expected.workspaces.size(); w < workspaces; ++w)
        {
            CHECK(actual.workspaces[w].windows.empty());
            CHECK(actual.workspaces[w].layout_strategy == LayoutStrategy::Monocle);
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
    source.fullscreen_monitors(1, FullscreenMonitors{});
    auto snapshot = source.snapshot();
    auto restored = test::state();
    restored.adopt({ test::observe(source.require(1)) }, &snapshot);
    CHECK(restored.snapshot().clients == snapshot.clients);
}

TEST_CASE("Restart preserves pending requests only for surviving scratchpad names", "[restart][state][scratchpad]")
{
    auto source = test::state();
    test::configure(source, [](Config& config) { config.scratchpads = { { .name = "pending" }, { .name = "removed" }, { .name = "claimed" }, { .name = "empty" } }; });
    source.scratchpad_pending("pending", true);
    source.scratchpad_pending("removed", true);
    add(source, 1);
    source.claim_scratchpad(1, ScratchpadConfig{ .name = "claimed" });
    auto snapshot = restart::decode(restart::encode(source.snapshot()));
    REQUIRE(snapshot);
    auto restored = test::state();
    test::configure(restored, [](Config& config) { config.scratchpads = { { .name = "pending" }, { .name = "claimed" }, { .name = "empty" }, { .name = "new" } }; });
    std::vector<WindowObservation> observed;
    SECTION("Claimed client survives") { observed.push_back(test::observe(source.require(1))); }
    SECTION("Claimed client disappeared") { }
    restored.adopt(observed, &*snapshot);
    CHECK(restored.named_scratchpad("pending")->pending_launch());
    CHECK_FALSE(restored.named_scratchpad("removed"));
    CHECK_FALSE(restored.named_scratchpad("empty")->pending_launch());
    CHECK_FALSE(restored.named_scratchpad("new")->pending_launch());
    CHECK_FALSE(restored.named_scratchpad("claimed")->pending_launch());
    CHECK(restored.named_scratchpad("claimed")->claimed_window() == (restored.find(1) ? 1 : XCB_NONE));
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

TEST_CASE("Restart rejects ambiguous or missing registration identities", "[restart][codec][registration]")
{
    auto snapshot = sample();
    SECTION("Duplicate rank") { snapshot.clients[0].order = snapshot.clients[1].order; }
    SECTION("Zero identity") { snapshot.fixtures.front().id = XCB_NONE; }
    SECTION("Client and fixture identity overlap") { snapshot.fixtures.front().id = 0x100; }
    SECTION("Duplicate client") { snapshot.clients.push_back(snapshot.clients.front()); }
    SECTION("Duplicate fixture") { snapshot.fixtures.push_back(snapshot.fixtures.front()); }
    SECTION("Fixture and client rank overlap") { snapshot.fixtures.front().order = snapshot.clients.front().order; }
    SECTION("Unbounded rank") { snapshot.clients.front().order = UINT64_MAX; }
    SECTION("Unbounded fixture rank") { snapshot.fixtures.front().order = UINT64_MAX; }
    SECTION("Rank leaves no room for newcomers") { snapshot.fixtures.front().order = UINT64_MAX - 1; }
    CHECK_FALSE(restart::decode(restart::encode(snapshot)));
}

TEST_CASE("Admission preserves shared registration ranks independently of scan order", "[restart][state][registration]")
{
    auto source = test::state();
    add(source, 1);
    source.insert_fixture(2, Fixture::Role::Dock);
    add(source, 3);
    source.insert_fixture(4, Fixture::Role::Desktop);
    test::focus(source, 1);
    source.swap_tiles(0, 0, 1);
    auto snapshot = restart::decode(restart::encode(source.snapshot()));
    REQUIRE(snapshot);
    REQUIRE(snapshot->find(3)->order == 2);
    REQUIRE(snapshot->find_fixture(2)->role == Fixture::Role::Dock);

    auto target = test::state();
    // Fixture observations precede client restoration; newcomer 5 arrives first.
    target.adopt({ { .id = 5, .type = WindowType::Dock }, { .id = 4, .type = WindowType::Desktop },
                   { .id = 2, .type = WindowType::Dock }, { .id = 6 }, { .id = 3 } }, &*snapshot);
    CHECK(target.require(3).order == 2);
    CHECK(target.find_fixture(2)->order == 1);
    CHECK(target.find_fixture(4)->order == 3);
    CHECK(target.find_fixture(5)->order == 4);
    CHECK(target.require(6).order > target.find_fixture(5)->order);
    add(target, 7);
    CHECK(target.require(7).order > target.require(6).order);
    CHECK_FALSE(invariants::validate(target));
}

TEST_CASE("Scene registration follows observation order across roles and handoff gaps", "[state][restart][registration]")
{
    auto source = test::state();
    source.adopt({ { .id = 1 }, { .id = 2, .type = WindowType::Dock },
                   { .id = 9, .type = WindowType::Tooltip }, { .id = 3 },
                   { .id = 4, .type = WindowType::Desktop } }, nullptr);
    CHECK(source.require(1).order == 0);
    CHECK(source.find_fixture(2)->order == 1);
    CHECK(source.require(3).order == 2);
    CHECK(source.find_fixture(4)->order == 3);
    auto snapshot = restart::decode(restart::encode(source.snapshot()));
    REQUIRE(snapshot);
    auto target = test::state();
    target.adopt({ { .id = 6 }, { .id = 2, .type = WindowType::Dock },
                   { .id = 5, .type = WindowType::Dock }, { .id = 3 },
                   { .id = 7 }, { .id = 8, .type = WindowType::Desktop } }, &*snapshot);
    CHECK(target.find_fixture(2)->order == 1);
    CHECK(target.require(3).order == 2);
    CHECK(target.require(6).order == 4); // Vanished identities still reserve their saved ranks.
    CHECK(target.require(6).order < target.find_fixture(5)->order);
    CHECK(target.find_fixture(5)->order < target.require(7).order);
    CHECK(target.require(7).order < target.find_fixture(8)->order);
    CHECK_FALSE(invariants::validate(target));
    CHECK(restart::decode(restart::encode(target.snapshot())));
}

TEST_CASE("Restart rejects empty frames before they reach geometry projection", "[restart][codec][geometry]")
{
    auto state = test::state();
    add_floating(state, 1);
    auto snapshot = state.snapshot();
    REQUIRE(restart::decode(restart::encode(snapshot)));
    SECTION("Output") { snapshot.monitors[0].geometry.width = 0; }
    SECTION("Floating frame") { std::get<FloatingMode>(snapshot.clients[0].mode).geometry.height = 0; }
    SECTION("Remembered floating frame") { snapshot.clients[0].mode = TiledMode{ Geometry{ 0, 0, 0, 10 } }; snapshot.monitors[0].workspaces[0].windows = { 1 }; }
    CHECK_FALSE(restart::decode(restart::encode(snapshot)));
}

TEST_CASE("Restart decoder rejects malformed typed values before narrowing or defaulting", "[restart][codec]")
{
    auto original = unpack_json(restart::encode(sample()));
    REQUIRE(restart::decode(pack_json(original.dump())) == sample());
    auto invalid_role = original;
    invalid_role["fixtures"][0]["role"] = "Popup";
    CHECK_FALSE(restart::decode(pack_json(invalid_role.dump())));
    for (auto const& [path, value] : std::vector<std::pair<std::string, nlohmann::json>>{
             {                 "/clients/0/id",                                uint64_t{ 1 } << 32 },
             {                "/clients/0/monitor",                                                 -1 },
             {              "/clients/0/mru_order",                                                 -1 },
             {                "/clients/0/urgency/sources",                                                256 },
             {                "/clients/0/urgency/sources",                                                  4 },
             {             "/clients/0/borderless",                                                  1 },
             {            "/monitors/0/geometry/x",                                              32768 },
             {            "/monitors/0/geometry/y",                                             -32769 },
             {        "/monitors/0/geometry/width",                                              65536 },
             {       "/monitors/0/geometry/height",                                                 -1 },
             {        "/monitors/0/geometry/width",                                                1.0 },
             { "/monitors/0/workspaces/0/layout_strategy",                                          "Unknown" },
             {      "/clients/0/preferences/layer",                                          "Unknown" },
             {                   "/clients/0/mode",                           nlohmann::json::object() },
             {                   "/clients/0/mode", { { "UnknownMode", { { "floating", nullptr } } } } },
             {                         "/monitors",                                        4294967295u },
    })
    {
        CAPTURE(path, value);
        auto damaged = original;
        damaged[nlohmann::json::json_pointer(path)] = value;
        CHECK_FALSE(restart::decode(pack_json(damaged.dump())));
    }
    SECTION("Every incomplete JSON payload is rejected with an accurate envelope")
    {
        auto payload = original.dump();
        for (size_t size = 0; size < payload.size(); ++size)
        {
            CAPTURE(size);
            CHECK_FALSE(restart::decode(pack_json(payload.substr(0, size))));
        }
    }
    SECTION("Missing optional fields are damage, not defaults")
    {
        original["clients"][0]["preferences"].erase("floating");
        CHECK_FALSE(restart::decode(pack_json(original.dump())));
    }
    SECTION("Unknown nested fields are rejected")
    {
        original["clients"][0]["preferences"]["unknown"] = true;
        CHECK_FALSE(restart::decode(pack_json(original.dump())));
    }
    SECTION("Variant tag must match its payload")
    {
        original["clients"][0]["mode"] = {
            { "FloatingMode", { { "floating", nullptr } } }
        };
        CHECK_FALSE(restart::decode(pack_json(original.dump())));
    }
    SECTION("A valid mode cannot hide an extra tag")
    {
        original["clients"][0]["mode"]["UnknownMode"] = nullptr;
        CHECK_FALSE(restart::decode(pack_json(original.dump())));
    }
    SECTION("Two valid mode tags cannot choose one by their order")
    {
        original["clients"][0]["mode"]["FloatingMode"] = original["clients"][1]["mode"]["FloatingMode"];
        CHECK_FALSE(restart::decode(pack_json(original.dump())));
    }
    SECTION("Duplicate fields cannot overwrite decoded values")
    {
        auto text = original.dump();
        text.insert(1, "\"focused_monitor\":0,");
        CHECK_FALSE(restart::decode(pack_json(text)));
    }
    SECTION("Length and padding cannot hide trailing data")
    {
        auto text = original.dump();
        while (text.size() % 4 == 0) text += ' ';
        auto words = pack_json(text);
        reinterpret_cast<char*>(words.data() + 2)[text.size()] = 'x';
        CHECK_FALSE(restart::decode(words));
        words = pack_json(text);
        words[1] = UINT32_MAX;
        CHECK_FALSE(restart::decode(words));
        CHECK_FALSE(restart::decode(pack_json(text + "{}")));
    }
}

TEST_CASE("Restart preserves full-width recency and opaque output names", "[restart][codec]")
{
    auto source = sample();
    source.clients[0].mru_order = UINT64_MAX - 1;
    source.monitors[0].name = std::string("output\0", 7) + char(0xff);
    CHECK(restart::decode(restart::encode(source)) == source);
}

TEST_CASE("Restart rejects inconsistent workspace graphs before restoration", "[restart][codec]")
{
    auto snapshot = sample();
    auto& workspace = snapshot.monitors[0].workspaces[0];
    SECTION("Duplicate tile") { workspace.windows.push_back(0x100); }
    SECTION("Unregistered tile") { workspace.windows.push_back(0x999); }
    SECTION("Fixture in tiled membership") { workspace.windows.push_back(0x400); }
    SECTION("Floating client in tiled membership") { snapshot.monitors[1].workspaces[0].windows.push_back(0x200); }
    SECTION("Missing tile") { workspace.windows.clear(); workspace.preferred_tile = XCB_NONE; }
    SECTION("Wrong placement") { snapshot.clients[0].workspace = 1; }
    SECTION("Preference outside membership") { workspace.preferred_tile = 0x200; }
    CHECK_FALSE(restart::decode(restart::encode(snapshot)));
}

TEST_CASE("Graph restoration separates saved intent from live observations and new claims", "[restart][state]")
{
    auto source = test::state(2);
    add(source, 1, { .monitor = 1, .workspace = 2 });
    add(source, 2, { .monitor = 1, .workspace = 2 });
    add(source, 3, { .monitor = 1, .workspace = 2 });
    source.switch_workspace(1, 2);
    source.focus_monitor(1);
    source.fullscreen(1, true);
    test::configure(source, [](Config& config) {
        WindowRuleConfig rule{ .actions = { .borderless = true } };
        rule.match.title_regex.emplace("apply private preference");
        config.rules.push_back(std::move(rule));
    });
    source.title(2, "apply private preference");
    source.skip_pager(2, false);
    auto snapshot = source.snapshot();

    // Fresh property reads carry no saved placement or private preferences.
    WindowObservation first, saved, newcomer, pinned;
    first.id = 1;
    first.states.set(WindowState::Fullscreen);
    saved.id = 2;
    saved.name = "new title";
    saved.transient_for = 1;
    saved.accepts_input = false;
    saved.supports_take_focus = true;
    saved.states.set(WindowState::Fullscreen);
    saved.states.set(WindowState::MaximizedHorz);
    newcomer.id = 4;
    newcomer.states.set(WindowState::Fullscreen);
    pinned.id = 5;
    pinned.desktop = 0;
    auto target = test::state(2);
    // Reverse the outputs. A concrete desktop hint uses discovered indices;
    // saved placement instead follows output identity, and defaults follow focus.
    test::outputs(target, { test::output("M1"), test::output("M0", 1000) });
    SECTION("Newcomer requested fullscreen before the saved client")
    {
        target.adopt({ first, newcomer, saved, pinned }, &snapshot);
        CHECK(target.fullscreen_owners().at(0) == 2);
    }
    SECTION("Saved client requested fullscreen before the newcomer")
    {
        target.adopt({ first, saved, newcomer, pinned }, &snapshot);
        CHECK(target.fullscreen_owners().at(0) == 4);
    }
    auto const& restored = target.require(2);
    CHECK(restored.monitor == 0);
    CHECK(restored.workspace == 2);
    CHECK(restored.borderless);
    CHECK(restored.preferences.skip_pager == false);
    CHECK(restored.name == "new title");
    CHECK(restored.transient_for == 1);
    CHECK_FALSE(restored.accepts_input);
    CHECK(restored.supports_take_focus);
    CHECK_FALSE(restored.maximized_horz);
    CHECK(target.require(4).monitor == 0);
    CHECK(target.require(4).workspace == 2);
    CHECK(target.require(5).workspace == 0);
    CHECK(target.monitors()[0].workspaces[2].windows == std::vector<xcb_window_t>{ 1, 2, 4 });
    CHECK_FALSE(target.find(3));
    REQUIRE(target.fullscreen_claims().size() == 3);
    CHECK(target.fullscreen_claims().front() == 1);
    CHECK_FALSE(invariants::validate(target));
}

TEST_CASE("Persistent graph validation rejects scratchpad ownership before reconciliation", "[restart][codec][invariants][scratchpad]")
{
    auto graph = sample();
    REQUIRE_FALSE(invariants::validate(graph));
    REQUIRE(restart::decode(restart::encode(graph)) == graph);
    SECTION("Unmanaged pool member") { graph.pool.push_back(999); }
    SECTION("Fixture registration is not a pool client")
    {
        graph.pool.push_back(0x400);
    }
    SECTION("Duplicate pool member") { graph.pool.push_back(graph.pool.front()); }
    SECTION("Named and pooled claims overlap") { graph.pool.push_back(0x200); }
    SECTION("Zero is not a pooled client") { graph.pool.push_back(XCB_NONE); }
    SECTION("Empty named identity") { graph.named_scratchpads.front().name.clear(); }
    SECTION("Active registration is not a client")
    {
        graph.active = 0x400;
    }
    SECTION("Unmanaged active window") { graph.active = 999; }
    REQUIRE(invariants::validate(graph));
    CHECK_FALSE(restart::decode(restart::encode(graph)));
}

TEST_CASE("Valid ownership survives changed observations and filters vanished clients", "[restart][state][scratchpad]")
{
    // Metadata updates below must not satisfy the pending launch.
    auto scratchpads = [](Config& config)
    {
        config.scratchpads = { { .name = "named" }, { .name = "pending" } };
        for (auto& scratchpad : config.scratchpads) scratchpad.match.class_regex.emplace("unmatched");
    };
    auto source = test::state();
    test::configure(source, scratchpads);
    for (xcb_window_t id : { 1, 2, 3, 4 }) add(source, id);
    source.claim_scratchpad(1, ScratchpadConfig{ .name = "named" });
    source.scratchpad_pending("pending", true);
    source.pool_scratchpad(2);
    source.pool_scratchpad(3);
    source.iconic(2, true);
    source.window_type(4, WindowType::Dialog);
    auto graph = restart::decode(restart::encode(source.snapshot()));
    REQUIRE(graph);
    auto target = test::state();
    test::configure(target, scratchpads);
    std::vector<WindowObservation> observed;
    for (auto id : { 1, 3, 4 })
    {
        observed.push_back({
            .id = static_cast<xcb_window_t>(id),
            .type = id == 4 ? WindowType::Normal : WindowType::Utility,
            .geometry = Geometry{ 1, 2, 30, 40 },
        });
    }
    target.adopt(observed, &*graph);
    CHECK(target.require(1).kind() == Client::Kind::Tiled);
    CHECK(target.require(3).kind() == Client::Kind::Tiled);
    CHECK(target.require(4).kind() == Client::Kind::Floating);
    CHECK_FALSE(target.require(4).preferences.floating);
    CHECK(target.scratchpad_claim(1)->name == "named");
    CHECK(target.named_scratchpad("pending")->pending_launch());
    CHECK(target.scratchpad_pool() == std::vector<xcb_window_t>{ 3 });
    CHECK_FALSE(target.find(2));
    CHECK_FALSE(invariants::validate(target));
    // Restoration preserves saved representation. A subsequent metadata update
    // still follows defaults for an ordinary client, while claims retain mode.
    target.window_type(4, WindowType::Utility);
    target.window_type(4, WindowType::Normal);
    CHECK(target.require(4).kind() == Client::Kind::Tiled);
    target.window_type(1, WindowType::Dialog);
    target.window_type(3, WindowType::Dialog);
    CHECK(target.require(1).kind() == Client::Kind::Tiled);
    CHECK(target.require(3).kind() == Client::Kind::Tiled);
    CHECK_FALSE(invariants::validate(target));
}

TEST_CASE("Restoration invalidates orphaned tile return slots even with identical outputs", "[restart][state][tile-slot]")
{
    auto source = test::state();
    add_floating(source, 1);
    auto snapshot = source.snapshot();
    std::get<FloatingMode>(snapshot.clients.front().mode).tile_slot = TileSlot{ 0, "vanished", 0 };
    auto target = test::state();
    target.adopt({ test::observe(source.require(1)) }, &snapshot);
    CHECK_FALSE(floating_mode(target.require(1))->tile_slot);
    CHECK_FALSE(invariants::validate(target));
}
