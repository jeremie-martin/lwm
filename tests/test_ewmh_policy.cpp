#include <catch2/catch_test_macros.hpp>
#include "lwm/core/policy.hpp"

using namespace lwm;

// ─────────────────────────────────────────────────────────────────────────────
// Desktop index encoding tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("EWMH desktop index is linear", "[ewmh][policy]")
{
    REQUIRE(ewmh_policy::desktop_index(2, 3, 10) == 23u);
    REQUIRE(ewmh_policy::desktop_index(0, 0, 5) == 0u);
}

TEST_CASE("EWMH desktop index for first monitor workspaces", "[ewmh][policy]")
{
    // First monitor (index 0) workspaces 0-9
    for (size_t ws = 0; ws < 10; ++ws)
    {
        REQUIRE(ewmh_policy::desktop_index(0, ws, 10) == ws);
    }
}

TEST_CASE("EWMH desktop index for second monitor workspaces", "[ewmh][policy]")
{
    // Second monitor (index 1) workspaces 0-9 -> desktops 10-19
    for (size_t ws = 0; ws < 10; ++ws)
    {
        REQUIRE(ewmh_policy::desktop_index(1, ws, 10) == 10 + ws);
    }
}

TEST_CASE("EWMH desktop index with single workspace per monitor", "[ewmh][policy]")
{
    REQUIRE(ewmh_policy::desktop_index(0, 0, 1) == 0u);
    REQUIRE(ewmh_policy::desktop_index(1, 0, 1) == 1u);
    REQUIRE(ewmh_policy::desktop_index(5, 0, 1) == 5u);
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
    auto indices = ewmh_policy::desktop_to_indices(15, 10);

    REQUIRE(indices);
    REQUIRE(indices->first == 1);
    REQUIRE(indices->second == 5);
    REQUIRE(ewmh_policy::desktop_index(indices->first, indices->second, 10) == 15u);
}

TEST_CASE("EWMH desktop index decodes desktop 0", "[ewmh][policy]")
{
    auto indices = ewmh_policy::desktop_to_indices(0, 10);

    REQUIRE(indices);
    REQUIRE(indices->first == 0);
    REQUIRE(indices->second == 0);
}

TEST_CASE("EWMH desktop index decodes last workspace of first monitor", "[ewmh][policy]")
{
    auto indices = ewmh_policy::desktop_to_indices(9, 10);

    REQUIRE(indices);
    REQUIRE(indices->first == 0);
    REQUIRE(indices->second == 9);
}

TEST_CASE("EWMH desktop index decodes first workspace of second monitor", "[ewmh][policy]")
{
    auto indices = ewmh_policy::desktop_to_indices(10, 10);

    REQUIRE(indices);
    REQUIRE(indices->first == 1);
    REQUIRE(indices->second == 0);
}

TEST_CASE("EWMH desktop index decodes with single workspace per monitor", "[ewmh][policy]")
{
    for (uint32_t desktop = 0; desktop < 5; ++desktop)
    {
        auto indices = ewmh_policy::desktop_to_indices(desktop, 1);
        REQUIRE(indices);
        REQUIRE(indices->first == desktop);
        REQUIRE(indices->second == 0);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Round-trip tests (encode then decode)
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("EWMH desktop index round-trip encode/decode", "[ewmh][policy]")
{
    constexpr size_t workspaces_per_monitor = 10;

    for (size_t monitor = 0; monitor < 4; ++monitor)
    {
        for (size_t workspace = 0; workspace < workspaces_per_monitor; ++workspace)
        {
            uint32_t desktop = ewmh_policy::desktop_index(monitor, workspace, workspaces_per_monitor);
            auto decoded = ewmh_policy::desktop_to_indices(desktop, workspaces_per_monitor);

            REQUIRE(decoded);
            REQUIRE(decoded->first == monitor);
            REQUIRE(decoded->second == workspace);
        }
    }
}

TEST_CASE("EWMH desktop index round-trip with varying workspace counts", "[ewmh][policy]")
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
