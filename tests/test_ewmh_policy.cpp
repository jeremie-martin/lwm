#include <catch2/catch_test_macros.hpp>
#include "lwm/core/policy.hpp"

using namespace lwm;

TEST_CASE("EWMH desktop index is linear", "[ewmh][policy]")
{
    REQUIRE(ewmh_policy::desktop_index(2, 3, 10) == 23u);
    REQUIRE(ewmh_policy::desktop_index(0, 0, 5) == 0u);
}

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
