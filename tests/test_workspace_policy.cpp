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

TEST_CASE("Move tiled window clears focus when remaining windows are iconic", "[workspace][policy]")
{
    Monitor monitor = make_monitor(2);
    monitor.current_workspace = 0;
    monitor.workspaces[0].windows = { 0x1000, 0x2000 };
    monitor.workspaces[0].focused_window = 0x2000;

    std::unordered_set<xcb_window_t> iconic = { 0x1000 };
    auto is_iconic = [&](xcb_window_t window) { return iconic.contains(window); };

    bool moved = workspace_policy::move_tiled_window(monitor, 0x2000, 1, is_iconic);

    REQUIRE(moved);
    REQUIRE(monitor.workspaces[0].windows == std::vector<xcb_window_t>{ 0x1000 });
    REQUIRE(monitor.workspaces[0].focused_window == XCB_NONE);
    REQUIRE(monitor.workspaces[1].windows == std::vector<xcb_window_t>{ 0x2000 });
    REQUIRE(monitor.workspaces[1].focused_window == 0x2000);
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

// ─────────────────────────────────────────────────────────────────────────────
// Edge case tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("Workspace switch with empty workspace list", "[workspace][policy][edge]")
{
    Monitor monitor = make_monitor(0); // No workspaces
    monitor.current_workspace = 0;

    auto result = workspace_policy::apply_workspace_switch(monitor, 0);
    REQUIRE_FALSE(result);
    REQUIRE(monitor.current_workspace == 0);
}

TEST_CASE("Workspace switch with very large workspace count", "[workspace][policy][edge]")
{
    Monitor monitor = make_monitor(10000); // Very large workspace count
    monitor.current_workspace = 5000;

    auto result = workspace_policy::apply_workspace_switch(monitor, 9999);
    REQUIRE(result);
    REQUIRE(result->old_workspace == 5000);
    REQUIRE(result->new_workspace == 9999);
}

TEST_CASE("Workspace switch at boundaries (0 and max)", "[workspace][policy][edge]")
{
    Monitor monitor = make_monitor(100);
    monitor.current_workspace = 50;

    // Switch to first workspace
    auto r1 = workspace_policy::apply_workspace_switch(monitor, 0);
    REQUIRE(r1);
    REQUIRE(r1->new_workspace == 0);

    // Switch to last workspace
    auto r2 = workspace_policy::apply_workspace_switch(monitor, 99);
    REQUIRE(r2);
    REQUIRE(r2->new_workspace == 99);
}

TEST_CASE("Move tiled window with empty workspace list", "[workspace][policy][edge]")
{
    Monitor monitor = make_monitor(0); // No workspaces

    auto is_iconic = [](xcb_window_t) { return false; };

    bool moved = workspace_policy::move_tiled_window(monitor, 0x1000, 0, is_iconic);
    REQUIRE_FALSE(moved);
}

TEST_CASE("Move tiled window to out-of-range workspace", "[workspace][policy][edge]")
{
    Monitor monitor = make_monitor(3);
    monitor.current_workspace = 0;
    monitor.workspaces[0].windows = { 0x1000 };
    monitor.workspaces[0].focused_window = 0x1000;

    auto is_iconic = [](xcb_window_t) { return false; };

    // Target workspace 99 doesn't exist
    bool moved = workspace_policy::move_tiled_window(monitor, 0x1000, 99, is_iconic);
    REQUIRE_FALSE(moved);
    REQUIRE(monitor.workspaces[0].windows == std::vector<xcb_window_t>{ 0x1000 });
}

TEST_CASE("Move tiled window when all windows in source are iconic", "[workspace][policy][edge]")
{
    Monitor monitor = make_monitor(2);
    monitor.current_workspace = 0;
    monitor.workspaces[0].windows = { 0x1000, 0x2000, 0x3000 };
    monitor.workspaces[0].focused_window = 0x2000;

    // All windows are iconic
    std::unordered_set<xcb_window_t> iconic = { 0x1000, 0x2000, 0x3000 };
    auto is_iconic = [&](xcb_window_t window) { return iconic.contains(window); };

    bool moved = workspace_policy::move_tiled_window(monitor, 0x2000, 1, is_iconic);
    REQUIRE(moved);

    // After moving focused window, remaining windows are all iconic
    REQUIRE(monitor.workspaces[0].windows == std::vector<xcb_window_t>{ 0x1000, 0x3000 });
    REQUIRE(monitor.workspaces[0].focused_window == XCB_NONE); // Should be cleared
    REQUIRE(monitor.workspaces[1].windows == std::vector<xcb_window_t>{ 0x2000 });
    REQUIRE(monitor.workspaces[1].focused_window == 0x2000);
}

TEST_CASE("Move tiled window that doesn't exist", "[workspace][policy][edge]")
{
    Monitor monitor = make_monitor(2);
    monitor.current_workspace = 0;
    monitor.workspaces[0].windows = { 0x1000 };

    auto is_iconic = [](xcb_window_t) { return false; };

    // Try to move window that's not in current workspace
    bool moved = workspace_policy::move_tiled_window(monitor, 0x9999, 1, is_iconic);
    REQUIRE_FALSE(moved);
}

TEST_CASE("Workspace switch previous_workspace update", "[workspace][policy][edge]")
{
    Monitor monitor = make_monitor(5);
    monitor.current_workspace = 2;
    monitor.previous_workspace = 3;

    // Switch to 0, previous should become 2
    auto r1 = workspace_policy::apply_workspace_switch(monitor, 0);
    REQUIRE(r1);
    REQUIRE(monitor.previous_workspace == 2);

    // Switch to 4, previous should become 0
    auto r2 = workspace_policy::apply_workspace_switch(monitor, 4);
    REQUIRE(r2);
    REQUIRE(monitor.previous_workspace == 0);

    // Switch back to 2, previous should become 4
    auto r3 = workspace_policy::apply_workspace_switch(monitor, 2);
    REQUIRE(r3);
    REQUIRE(monitor.previous_workspace == 4);
}
