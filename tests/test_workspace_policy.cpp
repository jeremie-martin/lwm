#include "lwm/core/policy.hpp"
#include <catch2/catch_test_macros.hpp>

using namespace lwm;

namespace {

Monitor make_monitor(size_t workspaces)
{
    Monitor monitor;
    monitor.workspaces.assign(workspaces, Workspace{});
    monitor.current_workspace = 0;
    monitor.previous_workspace = 0;
    return monitor;
}

} // namespace

TEST_CASE("validate_workspace_switch returns decision without mutating monitor", "[workspace][policy]")
{
    Monitor monitor = make_monitor(3);
    monitor.current_workspace = 1;
    monitor.previous_workspace = 0;

    SECTION("Valid switch returns old and new workspace")
    {
        auto result = workspace_policy::validate_workspace_switch(monitor, 2);
        REQUIRE(result);
        REQUIRE(result->old_workspace == 1);
        REQUIRE(result->new_workspace == 2);
        REQUIRE(monitor.current_workspace == 1);
        REQUIRE(monitor.previous_workspace == 0);
    }

    SECTION("Same workspace switch rejected")
    {
        auto same = workspace_policy::validate_workspace_switch(monitor, 1);
        REQUIRE_FALSE(same);
        REQUIRE(monitor.current_workspace == 1);
        REQUIRE(monitor.previous_workspace == 0);
    }

    SECTION("Out of range switch rejected")
    {
        auto out_of_range = workspace_policy::validate_workspace_switch(monitor, 5);
        REQUIRE_FALSE(out_of_range);
        REQUIRE(monitor.current_workspace == 1);
        REQUIRE(monitor.previous_workspace == 0);
    }
}
