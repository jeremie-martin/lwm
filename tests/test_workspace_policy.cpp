#include <catch2/catch_test_macros.hpp>
#include "lwm/core/policy.hpp"
#include <unordered_set>

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

TEST_CASE("Workspace switch updates current and previous", "[workspace][policy]")
{
    Monitor monitor = make_monitor(3);
    monitor.current_workspace = 1;
    monitor.previous_workspace = 0;

    auto result = workspace_policy::apply_workspace_switch(monitor, 2);

    REQUIRE(result);
    REQUIRE(result->old_workspace == 1);
    REQUIRE(result->new_workspace == 2);
    REQUIRE(monitor.previous_workspace == 1);
    REQUIRE(monitor.current_workspace == 2);
}

TEST_CASE("Workspace switch rejects invalid targets", "[workspace][policy]")
{
    Monitor monitor = make_monitor(2);
    monitor.current_workspace = 0;
    monitor.previous_workspace = 1;

    auto same = workspace_policy::apply_workspace_switch(monitor, 0);
    REQUIRE_FALSE(same);
    REQUIRE(monitor.current_workspace == 0);
    REQUIRE(monitor.previous_workspace == 1);

    auto out_of_range = workspace_policy::apply_workspace_switch(monitor, 5);
    REQUIRE_FALSE(out_of_range);
    REQUIRE(monitor.current_workspace == 0);
    REQUIRE(monitor.previous_workspace == 1);

    auto negative = workspace_policy::apply_workspace_switch(monitor, -1);
    REQUIRE_FALSE(negative);
    REQUIRE(monitor.current_workspace == 0);
    REQUIRE(monitor.previous_workspace == 1);
}

TEST_CASE("Move tiled window updates focus fallback", "[workspace][policy]")
{
    Monitor monitor = make_monitor(3);
    monitor.current_workspace = 0;
    monitor.workspaces[0].windows = { 0x1000, 0x2000, 0x3000 };
    monitor.workspaces[0].focused_window = 0x3000;
    monitor.workspaces[1].windows = { 0x4000 };

    std::unordered_set<xcb_window_t> iconic = { 0x2000 };
    auto is_iconic = [&](xcb_window_t window) { return iconic.find(window) != iconic.end(); };

    bool moved = workspace_policy::move_tiled_window(monitor, 0x3000, 1, is_iconic);

    REQUIRE(moved);
    REQUIRE(monitor.workspaces[0].windows == std::vector<xcb_window_t>{ 0x1000, 0x2000 });
    REQUIRE(monitor.workspaces[0].focused_window == 0x1000);
    REQUIRE(monitor.workspaces[1].windows == std::vector<xcb_window_t>{ 0x4000, 0x3000 });
    REQUIRE(monitor.workspaces[1].focused_window == 0x3000);
}

TEST_CASE("Moving non-focused window preserves focus", "[workspace][policy]")
{
    Monitor monitor = make_monitor(2);
    monitor.current_workspace = 0;
    monitor.workspaces[0].windows = { 0x1000, 0x2000 };
    monitor.workspaces[0].focused_window = 0x2000;

    auto is_iconic = [](xcb_window_t) { return false; };

    bool moved = workspace_policy::move_tiled_window(monitor, 0x1000, 1, is_iconic);

    REQUIRE(moved);
    REQUIRE(monitor.workspaces[0].focused_window == 0x2000);
    REQUIRE(monitor.workspaces[1].windows == std::vector<xcb_window_t>{ 0x1000 });
    REQUIRE(monitor.workspaces[1].focused_window == 0x1000);
}

TEST_CASE("Move tiled window fails when target is current workspace", "[workspace][policy]")
{
    Monitor monitor = make_monitor(2);
    monitor.current_workspace = 0;
    monitor.workspaces[0].windows = { 0x1000 };
    monitor.workspaces[0].focused_window = 0x1000;

    auto is_iconic = [](xcb_window_t) { return false; };

    bool moved = workspace_policy::move_tiled_window(monitor, 0x1000, 0, is_iconic);

    REQUIRE_FALSE(moved);
    REQUIRE(monitor.workspaces[0].windows == std::vector<xcb_window_t>{ 0x1000 });
    REQUIRE(monitor.workspaces[0].focused_window == 0x1000);
    REQUIRE(monitor.workspaces[1].windows.empty());
}
