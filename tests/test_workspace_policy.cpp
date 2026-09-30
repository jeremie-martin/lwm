#include "lwm/core/policy.hpp"
#include <catch2/catch_test_macros.hpp>
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

TEST_CASE("push_focus_history evicts oldest entry at capacity", "[workspace][policy][history]")
{
    Workspace ws;
    // Push kFocusHistoryMax + 1 distinct windows
    for (xcb_window_t i = 1; i <= workspace_policy::kFocusHistoryMax + 1; ++i)
        workspace_policy::push_focus_history(ws, i);

    REQUIRE(ws.focus_history.size() == workspace_policy::kFocusHistoryMax);
    // Oldest entry (1) should have been evicted
    REQUIRE(std::ranges::find(ws.focus_history, 1) == ws.focus_history.end());
    // Most recent entry should be at the back
    REQUIRE(ws.focus_history.back() == workspace_policy::kFocusHistoryMax + 1);
    // Second entry (2) should now be the oldest
    REQUIRE(ws.focus_history.front() == 2);
}

TEST_CASE("Minimizing remembered focus repairs focus without removing tiled membership", "[workspace][policy]")
{
    lwm::Workspace workspace;
    workspace.windows = { 0x1000, 0x2000, 0x3000 };
    workspace.focused_window = 0x2000;
    workspace.focus_history = { 0x3000, 0x1000, 0x2000 };
    lwm::workspace_policy::fixup_workspace_focus(
        workspace,
        0x2000,
        [](xcb_window_t window) { return window == 0x2000; }
    );
    REQUIRE(workspace.focused_window == 0x1000);
    REQUIRE(workspace.windows == std::vector<xcb_window_t>{ 0x1000, 0x2000, 0x3000 });

    lwm::workspace_policy::fixup_workspace_focus(workspace, 0x1000, [](xcb_window_t) { return true; });
    REQUIRE(workspace.focused_window == XCB_NONE);
    REQUIRE(workspace.windows.size() == 3);
}
