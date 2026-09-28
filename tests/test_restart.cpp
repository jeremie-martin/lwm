#include "lwm/core/restart.hpp"
#include <bit>
#include <catch2/catch_test_macros.hpp>
using namespace lwm;

TEST_CASE("Restart client codec preserves state and accepts older record lengths", "[restart][codec]")
{
    Client client;
    set_floating_state(client, { -100, 25, 600, 400 });
    client.borderless = true;
    client.desktop_pinned = true;
    client.fullscreen_restore = Geometry{ -200, -50, 800, 600 };
    client.fullscreen_restore_layer_hint = LayerHint::Below;
    client.app_prefs.skip_taskbar = true;
    client.urgency.add(UrgencySource::App);
    auto words = restart::encode_client(client);
    auto result = restart::decode_client(words);
    REQUIRE(result);
    CHECK(result->floating == floating_geometry(client));
    CHECK(result->fullscreen_restore == client.fullscreen_restore);
    CHECK(result->restore_layer == LayerHint::Below);
    CHECK(result->desktop_pinned);
    REQUIRE(result->app_prefs);
    CHECK(result->app_prefs->skip_taskbar);
    CHECK(result->urgency == client.urgency.sources);
    for (size_t length = 0; length < 24; ++length) CHECK_FALSE(restart::decode_client(std::span(words).first(length)));
    for (size_t length = 24; length <= words.size(); ++length)
        CHECK(restart::decode_client(std::span(words).first(length)));
    words[4] = 65536;
    CHECK_FALSE(restart::decode_client(words));
}
TEST_CASE("Restart global decoder rejects incomplete monitor records before mutation", "[restart][codec]")
{
    std::vector<uint32_t> words{ 3, 1, 42, 0, 2, 0, 1, 2, 0 };
    REQUIRE(restart::decode_global(words));
    for (size_t n = 0; n < words.size(); ++n) CHECK_FALSE(restart::decode_global(std::span(words).first(n)));
    words[4] = UINT32_MAX;
    CHECK_FALSE(restart::decode_global(words));
}
TEST_CASE("Restart layouts retain complete records and reject partial workspaces", "[restart][codec]")
{
    auto bits = std::bit_cast<uint64_t>(0.7);
    std::vector<uint32_t> words{ 3, 1, 2, 0, 1, 0, 0, static_cast<uint32_t>(bits), static_cast<uint32_t>(bits >> 32),
                                 1, 0 };
    auto records = restart::decode_layouts(words);
    REQUIRE(records.size() == 2);
    CHECK(records[0].ratios.at(SplitAddress{ 0 }) == 0.7);
    CHECK(records[1].strategy == LayoutStrategy::Monocle);
    words.pop_back();
    records = restart::decode_layouts(words);
    REQUIRE(records.size() == 1);
    words.resize(8);
    CHECK(restart::decode_layouts(words).empty());
    // A bad ratio is discarded independently of the containing workspace.
    words = { 1, 1, 1, 1, 0, 0, 0x7ff80000 };
    records = restart::decode_layouts(words);
    REQUIRE(records.size() == 1);
    CHECK(records[0].ratios.empty());
    for (uint32_t version : { 1u, 2u, 3u })
    {
        std::vector<uint32_t> hostile{ version, UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX };
        CHECK(restart::decode_layouts(hostile).empty());
    }
}

TEST_CASE("Restart ratio decoder accepts historical packed and depth-path addresses", "[restart][codec]")
{
    auto bits = std::bit_cast<uint64_t>(0.3);
    for (uint32_t version : { 1u, 2u, 3u })
    {
        std::vector<uint32_t> words{ version, 1, 1 };
        if (version == 3)
            words.push_back(0);
        words.push_back(1);
        if (version == 1)
            words.push_back((7 << 8) | 3);
        else
        {
            words.push_back(3);
            words.push_back(7);
        }
        words.push_back(static_cast<uint32_t>(bits));
        words.push_back(static_cast<uint32_t>(bits >> 32));
        auto layouts = restart::decode_layouts(words);
        REQUIRE(layouts.size() == 1);
        CHECK(layouts[0].ratios.at(SplitAddress{ 3 }) == 0.3);
        for (size_t end = 0; end < words.size(); ++end)
            CHECK(restart::decode_layouts(std::span(words).first(end)).empty());
    }
}
