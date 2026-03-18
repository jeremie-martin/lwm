#include "lwm/core/focus.hpp"
#include "lwm/core/policy.hpp"
#include "lwm/core/types.hpp"
#include <catch2/catch_test_macros.hpp>

using namespace lwm;

namespace {

Monitor make_monitor(
    std::string name, int16_t x, int16_t y, uint16_t width, uint16_t height, size_t workspace_count = 4
)
{
    Monitor monitor;
    monitor.name = std::move(name);
    monitor.x = x;
    monitor.y = y;
    monitor.width = width;
    monitor.height = height;
    monitor.workspaces.assign(workspace_count, Workspace{});
    monitor.current_workspace = 0;
    monitor.previous_workspace = 0;
    return monitor;
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// Monitor removal: window reassignment
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("Windows on removed monitor can be reassigned to remaining monitor", "[hotplug][monitor]")
{
    // Simulate 2 monitors, each with windows on workspace 0
    std::vector<Monitor> monitors;
    monitors.push_back(make_monitor("HDMI-0", 0, 0, 1920, 1080));
    monitors.push_back(make_monitor("DP-0", 1920, 0, 1920, 1080));

    monitors[0].workspaces[0].windows = { 0x1000, 0x2000 };
    monitors[0].workspaces[0].focused_window = 0x1000;
    monitors[1].workspaces[0].windows = { 0x3000, 0x4000 };
    monitors[1].workspaces[0].focused_window = 0x3000;

    // Simulate removing monitor 1: move its windows to monitor 0
    auto& target = monitors[0].workspaces[0];
    for (auto window : monitors[1].workspaces[0].windows)
        target.windows.push_back(window);

    monitors.erase(monitors.begin() + 1);

    REQUIRE(monitors.size() == 1);
    REQUIRE(target.windows.size() == 4);
    REQUIRE(target.windows[0] == 0x1000);
    REQUIRE(target.windows[1] == 0x2000);
    REQUIRE(target.windows[2] == 0x3000);
    REQUIRE(target.windows[3] == 0x4000);
}

TEST_CASE("Windows across multiple workspaces on removed monitor are all relocated", "[hotplug][monitor]")
{
    std::vector<Monitor> monitors;
    monitors.push_back(make_monitor("HDMI-0", 0, 0, 1920, 1080));
    monitors.push_back(make_monitor("DP-0", 1920, 0, 1920, 1080));

    monitors[1].workspaces[0].windows = { 0x1000 };
    monitors[1].workspaces[1].windows = { 0x2000, 0x3000 };
    monitors[1].workspaces[2].windows = { 0x4000 };

    // Relocate all windows from monitor 1 to monitor 0's current workspace
    auto& target = monitors[0].workspaces[0];
    for (auto& ws : monitors[1].workspaces)
    {
        for (auto window : ws.windows)
            target.windows.push_back(window);
    }
    monitors.erase(monitors.begin() + 1);

    REQUIRE(monitors.size() == 1);
    REQUIRE(target.windows.size() == 4);
}

// ─────────────────────────────────────────────────────────────────────────────
// Focused monitor clamping
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("Focused monitor index is clamped when monitors are removed", "[hotplug][monitor]")
{
    std::vector<Monitor> monitors;
    monitors.push_back(make_monitor("HDMI-0", 0, 0, 1920, 1080));
    monitors.push_back(make_monitor("DP-0", 1920, 0, 1920, 1080));
    monitors.push_back(make_monitor("DP-1", 3840, 0, 1920, 1080));

    size_t focused_monitor = 2; // User is focused on monitor 2 (DP-1)

    // Remove monitors 1 and 2
    monitors.erase(monitors.begin() + 1, monitors.end());

    // Clamp focused_monitor to valid range
    if (focused_monitor >= monitors.size())
        focused_monitor = monitors.size() - 1;

    REQUIRE(focused_monitor == 0);
    REQUIRE(monitors.size() == 1);
}

TEST_CASE("Focused monitor index stays valid when a different monitor is removed", "[hotplug][monitor]")
{
    std::vector<Monitor> monitors;
    monitors.push_back(make_monitor("HDMI-0", 0, 0, 1920, 1080));
    monitors.push_back(make_monitor("DP-0", 1920, 0, 1920, 1080));
    monitors.push_back(make_monitor("DP-1", 3840, 0, 1920, 1080));

    size_t focused_monitor = 0;

    // Remove monitor 2 (not the focused one)
    monitors.erase(monitors.begin() + 2);

    // focused_monitor should still be valid
    REQUIRE(focused_monitor < monitors.size());
    REQUIRE(monitors.size() == 2);
}

// ─────────────────────────────────────────────────────────────────────────────
// Workspace index clamping after monitor change
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("Workspace indices are clamped when workspace count decreases", "[hotplug][monitor]")
{
    Monitor mon = make_monitor("HDMI-0", 0, 0, 1920, 1080, 9);
    mon.current_workspace = 7;
    mon.previous_workspace = 8;

    // Simulate workspace count reduction (e.g., config reload)
    size_t new_count = 4;
    mon.workspaces.resize(new_count);

    if (mon.current_workspace >= mon.workspaces.size())
        mon.current_workspace = mon.workspaces.size() - 1;
    if (mon.previous_workspace >= mon.workspaces.size())
        mon.previous_workspace = mon.workspaces.size() - 1;

    REQUIRE(mon.current_workspace == 3);
    REQUIRE(mon.previous_workspace == 3);
}

// ─────────────────────────────────────────────────────────────────────────────
// Fallback monitor creation
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("Fallback monitor has valid default geometry", "[hotplug][monitor]")
{
    // When all monitors are disconnected, a fallback should be usable
    Monitor fallback;
    fallback.name = "fallback";
    fallback.x = 0;
    fallback.y = 0;
    fallback.width = 1024;
    fallback.height = 768;
    fallback.workspaces.assign(1, Workspace{});
    fallback.current_workspace = 0;

    auto area = fallback.working_area();
    REQUIRE(area.x == 0);
    REQUIRE(area.y == 0);
    REQUIRE(area.width == 1024);
    REQUIRE(area.height == 768);
    REQUIRE(fallback.workspaces.size() == 1);
}

// ─────────────────────────────────────────────────────────────────────────────
// Visibility policy after monitor removal
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("Visibility policy handles single-monitor correctly after removal", "[hotplug][visibility]")
{
    std::vector<Monitor> monitors;
    monitors.push_back(make_monitor("HDMI-0", 0, 0, 1920, 1080));
    monitors[0].current_workspace = 0;

    // Window on workspace 0, monitor 0 — should be visible
    bool visible = visibility_policy::is_window_visible(
        false, false, false, 0, 0, monitors
    );
    REQUIRE(visible);

    // Sticky window on monitor 0 — always visible
    bool sticky_visible = visibility_policy::is_window_visible(
        false, false, true, 0, 1, monitors
    );
    REQUIRE(sticky_visible);

    // Iconic window — never visible
    bool iconic_visible = visibility_policy::is_window_visible(
        false, true, false, 0, 0, monitors
    );
    REQUIRE_FALSE(iconic_visible);
}

TEST_CASE("Visibility policy returns false for out-of-range monitor index", "[hotplug][visibility]")
{
    std::vector<Monitor> monitors;
    monitors.push_back(make_monitor("HDMI-0", 0, 0, 1920, 1080));

    // Monitor index 5 doesn't exist
    bool visible = visibility_policy::is_window_visible(
        false, false, false, 5, 0, monitors
    );
    REQUIRE_FALSE(visible);
}

// ─────────────────────────────────────────────────────────────────────────────
// Monitor geometry lookup after changes
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("monitor_index_at_point returns nullopt after monitor removal", "[hotplug][focus]")
{
    std::vector<Monitor> monitors;
    monitors.push_back(make_monitor("HDMI-0", 0, 0, 1920, 1080));

    // Point that was on the removed second monitor
    auto idx = focus::monitor_index_at_point(monitors, 2500, 500);
    REQUIRE_FALSE(idx.has_value());

    // Point on the remaining monitor is still found
    auto idx0 = focus::monitor_index_at_point(monitors, 500, 500);
    REQUIRE(idx0.has_value());
    REQUIRE(*idx0 == 0);
}

TEST_CASE("monitor_index_at_point returns nullopt for empty monitor list", "[hotplug][focus]")
{
    std::vector<Monitor> monitors;
    auto idx = focus::monitor_index_at_point(monitors, 0, 0);
    REQUIRE_FALSE(idx.has_value());
}
