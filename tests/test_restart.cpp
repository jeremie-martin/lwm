#include "lwm/core/restart.hpp"
#include <bit>
#include <catch2/catch_test_macros.hpp>
using namespace lwm;

namespace {
// Fixed wire words, independent of the encoder and enum ordinals. Coordinates
// are unsigned 16-bit encodings; 65436 represents -100. Reserved slots stay zero.
constexpr std::array<uint32_t, 33> floating_record{
    0,     1,                    // retired overlay flag, borderless
    65436, 25,    600, 400,      // normal rectangle
    1,     65436, 25,  600, 400, // legacy fullscreen restore rectangle
    0,     0,     0,   0,   0,   // legacy maximize restore rectangle
    0,     0,     0,   0,   0,   // prior floating rectangle
    0,     0,     2,             // reserved, hidden pool kind, floating kind
    2,     9,     3,   1,        // urgency, skip-taskbar + below, restore-below, desktop pin
    1,     0,     2,   0,   3    // preference version, unset mode, taskbar, unset pager, below
};
}

TEST_CASE("Restart client encoder preserves the cross-binary wire contract", "[restart][codec]")
{
    Client client;
    client.state = FloatingState{
        { -100, 25, 600, 400 }
    };
    client.borderless = true;
    client.desktop_pinned = true;
    client.fullscreen = true;
    client.preferences.layer = LayerHint::Below;
    client.preferences.skip_taskbar = true;
    client.urgency.add(UrgencySource::App);
    CHECK(restart::encode_client(client) == floating_record);
}

TEST_CASE("Restart client decoder interprets historical extensions independently", "[restart][codec]")
{
    // The oldest record has 24 words. Later writers appended urgency (initially
    // boolean), then app preferences, restore layer, and finally desktop pinning.
    for (size_t length = 24; length <= 28; ++length)
    {
        CAPTURE(length);
        std::vector<uint32_t> words(floating_record.begin(), floating_record.begin() + length);
        if (length == 25)
            words[24] = 1; // old boolean urgency belongs to the WM, not the app
        auto result = restart::decode_client(words);
        REQUIRE(result);
        CHECK(result->borderless);
        CHECK(result->floating == Geometry{ -100, 25, 600, 400 });
        CHECK_FALSE(result->prior_floating);
        CHECK(result->hidden_pool_kind == 0);
        CHECK(result->kind == Client::Kind::Floating);
        if (length == 24)
            CHECK_FALSE(result->urgency);
        else
            CHECK(
                result->urgency == static_cast<uint8_t>(length == 25 ? UrgencySource::WmInitiated : UrgencySource::App)
            );
        CHECK(result->preferences.has_value() == (length >= 26));
        if (result->preferences)
        {
            CHECK(result->preferences->skip_taskbar);
            CHECK_FALSE(result->preferences->skip_pager);
            CHECK(result->preferences->layer == LayerHint::Below);
        }
        CHECK(result->desktop_pinned == (length >= 28));
    }
}

TEST_CASE("Restart client decoder rejects malformed records", "[restart][codec]")
{
    REQUIRE(restart::decode_client(floating_record));
    for (size_t length = 0; length < 24; ++length)
        CHECK_FALSE(restart::decode_client(std::span(floating_record).first(length)));
    for (size_t offset : { 2u, 7u, 12u, 17u })
    {
        auto words = floating_record;
        if (offset != 2)
            words[offset - 1] = 1;
        REQUIRE(restart::decode_client(words));
        for (size_t field = offset; field < offset + 4; ++field)
        {
            CAPTURE(offset, field);
            auto invalid = words;
            invalid[field] = 65536;
            CHECK_FALSE(restart::decode_client(invalid));
        }
    }
    for (size_t field : { 22u, 23u })
    {
        auto invalid = floating_record;
        invalid[field] = 3;
        CHECK_FALSE(restart::decode_client(invalid));
    }
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

TEST_CASE("Restart decoder folds legacy restore rectangles into normal geometry", "[restart][codec]")
{
    std::array<uint32_t, restart::client_words> words{ };
    words[23] = 2;
    auto rectangle = [&](size_t offset, uint32_t x)
    {
        words[offset] = x;
        words[offset + 1] = 20;
        words[offset + 2] = 300;
        words[offset + 3] = 200;
    };
    rectangle(2, 10);
    rectangle(7, 30);
    rectangle(12, 50);
    for (auto [fullscreen, maximize, expected] : {
             std::array<uint32_t, 3>{ 0, 0, 10 },
             { 1, 0, 30 },
             { 0, 1, 50 },
             { 1, 1, 50 }
    })
    {
        words[6] = fullscreen;
        words[11] = maximize;
        auto decoded = restart::decode_client(words);
        REQUIRE(decoded);
        CHECK(decoded->floating.x == expected);
    }
}
