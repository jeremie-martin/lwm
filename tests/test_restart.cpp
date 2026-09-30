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
    snapshot.monitors = { { 1,
                            0,
                            { { LayoutStrategy::Monocle, { { SplitAddress{ 0 }, 0.25 }, { SplitAddress{ 40 }, 0.75 } }, { 0x100, 0x300 }, 0x300 },
                              { } } },
                          { 0, 0, { { } } } };
    snapshot.clients = { { 0x100, 0, 0, Client::Kind::Tiled, { 1, 2, 3, 4 }, Geometry{ -5, -6, 70, 80 }, { true, false, std::nullopt, LayerHint::Below }, 3, true, false },
                         { 0x200, 1, 0, Client::Kind::Floating, { -32768, 32767, 65535, 1 }, std::nullopt, { }, 0, false, true } };
    snapshot.named_scratchpads = { { "tëxt with spaces", 0x200 }, { "", 0x100 } };
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
    other[0] = restart::format + 1;
    CHECK_FALSE(restart::decode(other));
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
        5, 0, 0,   0,  0, // format, focus, active, desktop, monitor count
        1,                // client count
        7, 0, 0,   0,     // id, monitor, workspace, tiled
        0, 0, 100, 80,    // normal geometry
        0, 0, 0,   0,  0, // absent remembered floating geometry
        0, 0, 0,   0,     // unset preferences
        0, 0, 0,          // urgency, borderless, pinned
        0, 0,             // named slots, pool
        1, 7              // oldest-to-newest fullscreen claims
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
        target.fullscreen(2, true);
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
