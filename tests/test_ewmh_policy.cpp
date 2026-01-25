#include "lwm/core/policy.hpp"
#include <catch2/catch_test_macros.hpp>

using namespace lwm;

// ─────────────────────────────────────────────────────────────────────────────
// Desktop index encoding tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("EWMH desktop index is linear", "[ewmh][policy]")
{
    REQUIRE(ewmh_policy::desktop_index(2, 3, 10) == 23u);
    REQUIRE(ewmh_policy::desktop_index(0, 0, 5) == 0u);
}

TEST_CASE("EWMH desktop index with many workspaces", "[ewmh][policy]")
{
    // 100 workspaces per monitor
    REQUIRE(ewmh_policy::desktop_index(0, 50, 100) == 50u);
    REQUIRE(ewmh_policy::desktop_index(1, 0, 100) == 100u);
    REQUIRE(ewmh_policy::desktop_index(2, 99, 100) == 299u);
}

// ─────────────────────────────────────────────────────────────────────────────
// Desktop index decoding tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("EWMH desktop index decoding handles zero workspaces", "[ewmh][policy]")
{
    auto indices = ewmh_policy::desktop_to_indices(7, 0);
    REQUIRE_FALSE(indices);
}

TEST_CASE("EWMH desktop index decodes to monitor and workspace", "[ewmh][policy]")
{
    auto decode_and_verify = [](uint32_t desktop, size_t ws_count, size_t expected_mon, size_t expected_ws)
    {
        auto indices = ewmh_policy::desktop_to_indices(desktop, ws_count);
        REQUIRE(indices);
        REQUIRE(indices->first == expected_mon);
        REQUIRE(indices->second == expected_ws);
    };

    decode_and_verify(0, 10, 0, 0);
    decode_and_verify(9, 10, 0, 9);
    decode_and_verify(10, 10, 1, 0);
    decode_and_verify(15, 10, 1, 5);
}

// ─────────────────────────────────────────────────────────────────────────────
// Round-trip tests (encode then decode)
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("EWMH desktop index round-trip encode/decode", "[ewmh][policy]")
{
    for (size_t ws_count = 1; ws_count <= 20; ++ws_count)
    {
        for (size_t monitor = 0; monitor < 3; ++monitor)
        {
            for (size_t workspace = 0; workspace < ws_count; ++workspace)
            {
                uint32_t desktop = ewmh_policy::desktop_index(monitor, workspace, ws_count);
                auto decoded = ewmh_policy::desktop_to_indices(desktop, ws_count);

                REQUIRE(decoded);
                REQUIRE(decoded->first == monitor);
                REQUIRE(decoded->second == workspace);
            }
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Large value tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("EWMH desktop index handles large monitor counts", "[ewmh][policy]")
{
    // 1000 monitors with 10 workspaces each
    uint32_t desktop = ewmh_policy::desktop_index(999, 9, 10);
    REQUIRE(desktop == 9999u);

    auto decoded = ewmh_policy::desktop_to_indices(desktop, 10);
    REQUIRE(decoded);
    REQUIRE(decoded->first == 999);
    REQUIRE(decoded->second == 9);
}

TEST_CASE("EWMH desktop index handles large workspace counts", "[ewmh][policy]")
{
    // 100 workspaces per monitor
    uint32_t desktop = ewmh_policy::desktop_index(5, 99, 100);
    REQUIRE(desktop == 599u);

    auto decoded = ewmh_policy::desktop_to_indices(desktop, 100);
    REQUIRE(decoded);
    REQUIRE(decoded->first == 5);
    REQUIRE(decoded->second == 99);
}
