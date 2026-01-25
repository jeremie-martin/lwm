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

// ─────────────────────────────────────────────────────────────────────────────
// Overflow and boundary tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("EWMH desktop index overflow with large values", "[ewmh][policy][edge]")
{
    // Test near uint32_t max (~4.3B)
    // monitor_idx * workspaces_per_monitor should not overflow
    size_t ws_per_mon = 10000;
    size_t monitor_idx = 400000; // 400000 * 10000 = 4,000,000,000 (under uint32_t max)

    uint32_t desktop = ewmh_policy::desktop_index(monitor_idx, 9999, ws_per_mon);
    REQUIRE(desktop == 4000009999u);

    // This would overflow: 429497 * 10000 = 4,294,970,000 > UINT32_MAX
    // Result truncates to uint32_t - this is documented behavior
    size_t overflow_monitor = 429497;
    uint32_t overflow_desktop = ewmh_policy::desktop_index(overflow_monitor, 9999, ws_per_mon);
    // Should wrap around due to overflow
    REQUIRE(overflow_desktop < 1000000u); // Wrapped to small value
}

TEST_CASE("EWMH desktop_to_indices with max uint32_t desktop", "[ewmh][policy][edge]")
{
    // Decode UINT32_MAX
    auto decoded = ewmh_policy::desktop_to_indices(UINT32_MAX, 100);
    REQUIRE(decoded);
    // UINT32_MAX = 4294967295
    // monitor_idx = 42949672, workspace_idx = 95
    REQUIRE(decoded->first == 42949672u);
    REQUIRE(decoded->second == 95u);
}

TEST_CASE("EWMH desktop_to_indices with single workspace per monitor", "[ewmh][policy][edge]")
{
    // Special case: 1 workspace per monitor
    auto decoded1 = ewmh_policy::desktop_to_indices(0, 1);
    REQUIRE(decoded1);
    REQUIRE(decoded1->first == 0);
    REQUIRE(decoded1->second == 0);

    auto decoded2 = ewmh_policy::desktop_to_indices(99999, 1);
    REQUIRE(decoded2);
    REQUIRE(decoded2->first == 99999);
    REQUIRE(decoded2->second == 0);
}

TEST_CASE("EWMH desktop_index round-trip with large values", "[ewmh][policy][edge]")
{
    // Test round-trip encode/decode with large values that fit in uint32_t
    size_t ws_per_mon = 50000;
    size_t monitor_idx = 80000; // 80000 * 50000 = 4,000,000,000 (fits)
    size_t workspace_idx = 49999;

    uint32_t desktop = ewmh_policy::desktop_index(monitor_idx, workspace_idx, ws_per_mon);
    auto decoded = ewmh_policy::desktop_to_indices(desktop, ws_per_mon);

    REQUIRE(decoded);
    REQUIRE(decoded->first == monitor_idx);
    REQUIRE(decoded->second == workspace_idx);
}

TEST_CASE("EWMH desktop_to_indices with zero workspaces_per_monitor", "[ewmh][policy][edge]")
{
    // Already tested above, but document as edge case
    auto result = ewmh_policy::desktop_to_indices(100, 0);
    REQUIRE_FALSE(result);
}

TEST_CASE("EWMH desktop_index with zero workspace_idx", "[ewmh][policy][edge]")
{
    uint32_t desktop = ewmh_policy::desktop_index(100, 0, 50);
    REQUIRE(desktop == 5000u);

    auto decoded = ewmh_policy::desktop_to_indices(desktop, 50);
    REQUIRE(decoded);
    REQUIRE(decoded->first == 100);
    REQUIRE(decoded->second == 0);
}

TEST_CASE("EWMH desktop_to_indices with desktop larger than typical range", "[ewmh][policy][edge]")
{
    // Test decoding a desktop index that would correspond to
    // a very large monitor index
    auto decoded = ewmh_policy::desktop_to_indices(1000000000, 1000); // 1B with 1000 ws/monitor
    REQUIRE(decoded);
    REQUIRE(decoded->first == 1000000u);
    REQUIRE(decoded->second == 0);
}
