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

    // Adoption registers windows in scan order before the snapshot is applied.
    auto target = test::state(2);
    target.configure_scratchpads(std::vector<std::string>{ "term" });
    for (xcb_window_t id : { 3, 2, 1 }) add(target, id);
    auto const& saved = *snapshot->find(4);
    add(target, 4, { .monitor = saved.monitor, .workspace = saved.workspace, .floating = true, .geometry = saved.geometry });
    target.restore(*snapshot);

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
