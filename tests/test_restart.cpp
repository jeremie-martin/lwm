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
    words[0] = 11;
    words[1] = static_cast<uint32_t>(text.size());
    std::memcpy(words.data() + 2, text.data(), text.size());
    return words;
}

nlohmann::json unpack_json(std::vector<uint32_t> const& words)
{
    REQUIRE(words.size() >= 2);
    REQUIRE(words[0] == 11);
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
    snapshot.monitors = {
        { "M0",
         { 0, 0, 1000, 800 },
         1, 0,
         { { LayoutStrategy::Monocle,
         { { SplitAddress{ 0 }, 0.25 }, { SplitAddress{ 40 }, 0.75 } },
         { 0x100, 0x300 },
         0x300 },
         {} }                                       },
        { "M1", { 1000, 0, 1000, 800 }, 0, 0, { {} } }
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
        {                  "",        0x100 },
        {           "pending", std::nullopt }
    };
    snapshot.clients[1].fullscreen_monitors = FullscreenMonitors{ 0, 1, 0, 1 };
    snapshot.pool = { 0x300 };
    snapshot.registration_order = { 0x200, 0x300, 0x100 };
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
    auto invalid_recency = sample();
    invalid_recency.clients[0].mru_order = invalid_recency.clients[1].mru_order;
    CHECK_FALSE(restart::decode(restart::encode(invalid_recency)));
    invalid_recency.clients[0].mru_order = UINT64_MAX;
    CHECK_FALSE(restart::decode(restart::encode(invalid_recency)));
    // Out-of-range enumerations and ratios are rejected.
    auto snapshot = sample();
    snapshot.monitors[0].workspaces[0].ratios[SplitAddress{ 1 }] = 1.5;
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
    SECTION("Original output disappeared") { target.replace_monitors({ test::monitor("replacement") }); }
    SECTION("Saved workspace no longer exists")
    {
        for (auto& record : snapshot->clients)
            if (record.window == 2)
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
    source.configure_scratchpads(std::vector<std::string>{ "term" });
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
    add(target,
        4,
        { .monitor = saved.monitor,
          .workspace = saved.workspace,
          .floating = true,
          .geometry = std::get<FloatingMode>(saved.mode).geometry });
    target.restore_membership(*snapshot);

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

TEST_CASE("Restart claim order is explicit in the wire format", "[restart][codec]")
{
    // Independent literal input: no production encoder constructs this fixture.
    auto document = nlohmann::json::parse(R"({
        "focused_monitor": 0, "active": 0, "showing_desktop": false,
        "monitors": [{"name": "M", "geometry": {"x": 0, "y": 0, "width": 100, "height": 80},
            "current": 0, "previous": 0, "workspaces": [{"strategy": "MasterStack", "ratios": [],
            "tiles": [], "preferred_tile": 0}]}],
        "clients": [{"window": 7, "monitor": 0, "workspace": 0,
            "mode": {"TiledMode": {"floating": null}},
            "preferences": {"floating": null, "skip_taskbar": null, "skip_pager": null, "layer": null},
            "urgency": 0, "borderless": false, "desktop_pinned": false, "fullscreen_monitors": null, "mru_order": 0}],
        "registration_order": [7], "named_scratchpads": [], "pool": [], "fullscreen_claims": [7]
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
    test::focus(source, 2);
    source.floating(2, true);
    test::focus(source, 3);
    test::focus(source, 4);
    source.fullscreen_monitors(6, FullscreenMonitors{ 0, 2, 0, 2 });
    auto snapshot = restart::decode(restart::encode(source.snapshot()));
    REQUIRE(snapshot);

    std::vector<Monitor> discovered;
    bool fewer_workspaces = false;
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
    SECTION("Fewer workspaces")
    {
        discovered = { test::monitor("M1", 0, 1), test::monitor("M0", 1000, 1) };
        fewer_workspaces = true;
    }
    SECTION("Additional workspaces") { discovered = { test::monitor("M1", 0, 4), test::monitor("M0", 1000, 4) }; }
    for (auto& monitor : discovered) monitor.strut.top = 40;
    // Different new-workspace defaults ensure removed outputs cannot replace them.
    discovered[0].workspaces[0].layout_strategy = LayoutStrategy::Monocle;
    source.replace_monitors(discovered);
    if (fewer_workspaces)
    {
        // Both restoration and live reconciliation use preserve_workspaces.
        // Anchor their agreement to the contract: retain the survivor's order,
        // fold higher workspaces in order, then append displaced output members.
        CHECK(source.monitors()[0].current().windows == std::vector<xcb_window_t>{ 4, 5 });
        CHECK(source.monitors()[1].current().windows == std::vector<xcb_window_t>{ 1, 3 });
        CHECK(source.require(4).monitor == 0);
        CHECK(source.require(5).monitor == 0);
        CHECK(source.require(1).monitor == 1);
        for (auto const& [id, client] : source.clients()) CHECK(client.workspace == 0);
    }

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
        client.mode = it->mode;
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
            CHECK(actual.workspaces[w].preferred_tile == expected.workspaces[w].preferred_tile);
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
    source.fullscreen_monitors(1, FullscreenMonitors{});
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

TEST_CASE("Restart rejects ambiguous or missing registration identities", "[restart][codec][registration]")
{
    auto snapshot = sample();
    SECTION("Duplicate registration") { snapshot.registration_order.push_back(0x100); }
    SECTION("Zero registration") { snapshot.registration_order.push_back(XCB_NONE); }
    SECTION("Missing client") { std::erase(snapshot.registration_order, 0x100); }
    SECTION("Duplicate client") { snapshot.clients.push_back(snapshot.clients.front()); }
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
    REQUIRE(snapshot->registration_order == std::vector<xcb_window_t>{ 1, 2, 3, 4 });

    auto target = test::state();
    auto const& order = snapshot->registration_order;
    // A new fixture arrives first; client 1 disappeared during handoff.
    target.insert_fixture(5, Fixture::Role::Dock, order);
    target.insert_fixture(4, Fixture::Role::Desktop, order);
    target.insert_fixture(2, Fixture::Role::Dock, order);
    Client newcomer;
    newcomer.id = 6;
    target.insert(newcomer, order);
    Client survivor;
    survivor.id = 3;
    target.insert(survivor, order);
    CHECK(target.require(3).order == 2);
    CHECK(target.find_fixture(2)->order == 1);
    CHECK(target.find_fixture(4)->order == 3);
    CHECK(target.find_fixture(5)->order == 4);
    CHECK(target.require(6).order == 5);
    add(target, 7);
    CHECK(target.require(7).order == 6);
    CHECK(target.snapshot().registration_order == std::vector<xcb_window_t>{ 2, 3, 4, 5, 6, 7 });
}

TEST_CASE("Restart decoder rejects malformed typed values before narrowing or defaulting", "[restart][codec]")
{
    auto original = unpack_json(restart::encode(sample()));
    REQUIRE(restart::decode(pack_json(original.dump())) == sample());
    for (auto const& [path, value] : std::vector<std::pair<std::string, nlohmann::json>>{
             {                 "/clients/0/window",                                uint64_t{ 1 } << 32 },
             {                "/clients/0/monitor",                                                 -1 },
             {              "/clients/0/mru_order",                                                 -1 },
             {                "/clients/0/urgency",                                                256 },
             {                "/clients/0/urgency",                                                  4 },
             {             "/clients/0/borderless",                                                  1 },
             {            "/monitors/0/geometry/x",                                              32768 },
             {            "/monitors/0/geometry/y",                                             -32769 },
             {        "/monitors/0/geometry/width",                                              65536 },
             {       "/monitors/0/geometry/height",                                                 -1 },
             {        "/monitors/0/geometry/width",                                                1.0 },
             { "/monitors/0/workspaces/0/strategy",                                          "Unknown" },
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
