#include "lwm/core/window_rules.hpp"
#include <catch2/catch_test_macros.hpp>

using namespace lwm;

namespace {

// Helper to create a minimal Monitor with a name
Monitor make_monitor(std::string name)
{
    Monitor m;
    m.name = std::move(name);
    return m;
}

} // namespace

TEST_CASE("Empty rules return no match", "[rules]")
{
    std::vector<WindowRuleConfig> rules = { };

    WindowMatchInfo info{ .wm_class = "Firefox",
                          .wm_class_name = "Navigator",
                          .title = "Test",
                          .ewmh_type = WindowType::Normal,
                          .is_transient = false };

    auto result = match_window_rules(rules, info, { }, { });

    REQUIRE_FALSE(result.matched);
}

TEST_CASE("Exact class name matching", "[rules]")
{
    WindowRuleConfig cfg;
    cfg.match.class_regex.emplace("Firefox");
    cfg.floating = true;

    std::vector<WindowRuleConfig> rules = { cfg };

    SECTION("Exact match succeeds")
    {
        WindowMatchInfo info{ .wm_class = "Firefox",
                              .wm_class_name = "Navigator",
                              .title = "Test",
                              .ewmh_type = WindowType::Normal,
                              .is_transient = false };

        auto result = match_window_rules(rules, info, { }, { });

        REQUIRE(result.matched);
        REQUIRE(result.floating.has_value());
        REQUIRE(*result.floating == true);
    }

    SECTION("Exact class does not match substring")
    {
        WindowMatchInfo info{ .wm_class = "Firefox Developer Edition",
                              .wm_class_name = "Navigator",
                              .title = "Test",
                              .ewmh_type = WindowType::Normal,
                              .is_transient = false };

        auto result = match_window_rules(rules, info, { }, { });

        REQUIRE_FALSE(result.matched);
    }

    SECTION("Wildcard pattern matches substring")
    {
        WindowRuleConfig wildcard_cfg;
        wildcard_cfg.match.class_regex.emplace("Firefox.*");
        wildcard_cfg.floating = true;

        std::vector<WindowRuleConfig> wildcard_rules = { wildcard_cfg };

        WindowMatchInfo info{ .wm_class = "Firefox Developer Edition",
                              .wm_class_name = "Navigator",
                              .title = "Test",
                              .ewmh_type = WindowType::Normal,
                              .is_transient = false };

        auto result = match_window_rules(wildcard_rules, info, { }, { });

        REQUIRE(result.matched);
    }

    SECTION("Non-matching class fails")
    {
        WindowMatchInfo info{ .wm_class = "Chrome",
                              .wm_class_name = "Navigator",
                              .title = "Test",
                              .ewmh_type = WindowType::Normal,
                              .is_transient = false };

        auto result = match_window_rules(rules, info, { }, { });

        REQUIRE_FALSE(result.matched);
    }
}

TEST_CASE("Regex pattern matching", "[rules]")
{
    WindowRuleConfig cfg;
    cfg.match.title_regex.emplace(".*YouTube.*");
    cfg.floating = true;

    std::vector<WindowRuleConfig> rules = { cfg };

    SECTION("Regex matches")
    {
        WindowMatchInfo info{ .wm_class = "Firefox",
                              .wm_class_name = "Navigator",
                              .title = "Watching YouTube Videos",
                              .ewmh_type = WindowType::Normal,
                              .is_transient = false };

        auto result = match_window_rules(rules, info, { }, { });

        REQUIRE(result.matched);
    }

    SECTION("Regex does not match")
    {
        WindowMatchInfo info{ .wm_class = "Firefox",
                              .wm_class_name = "Navigator",
                              .title = "GitHub - Code Repository",
                              .ewmh_type = WindowType::Normal,
                              .is_transient = false };

        auto result = match_window_rules(rules, info, { }, { });

        REQUIRE_FALSE(result.matched);
    }
}

TEST_CASE("AND logic - all criteria must match", "[rules]")
{
    WindowRuleConfig cfg;
    cfg.match.class_regex.emplace("Firefox");
    cfg.match.title_regex.emplace(".*YouTube.*");
    cfg.floating = true;

    std::vector<WindowRuleConfig> rules = { cfg };

    SECTION("Both class and title match")
    {
        WindowMatchInfo info{ .wm_class = "Firefox",
                              .wm_class_name = "Navigator",
                              .title = "YouTube - Music",
                              .ewmh_type = WindowType::Normal,
                              .is_transient = false };

        auto result = match_window_rules(rules, info, { }, { });

        REQUIRE(result.matched);
    }

    SECTION("Class matches but title does not")
    {
        WindowMatchInfo info{ .wm_class = "Firefox",
                              .wm_class_name = "Navigator",
                              .title = "GitHub",
                              .ewmh_type = WindowType::Normal,
                              .is_transient = false };

        auto result = match_window_rules(rules, info, { }, { });

        REQUIRE_FALSE(result.matched);
    }

    SECTION("Title matches but class does not")
    {
        WindowMatchInfo info{ .wm_class = "Chrome",
                              .wm_class_name = "chrome",
                              .title = "YouTube",
                              .ewmh_type = WindowType::Normal,
                              .is_transient = false };

        auto result = match_window_rules(rules, info, { }, { });

        REQUIRE_FALSE(result.matched);
    }
}

TEST_CASE("First match wins", "[rules]")
{
    WindowRuleConfig rule1;
    rule1.match.class_regex.emplace("Firefox");
    rule1.floating = true;
    rule1.workspace = 5;

    WindowRuleConfig rule2;
    rule2.match.class_regex.emplace("Firefox");
    rule2.floating = false;
    rule2.workspace = 3;

    std::vector<WindowRuleConfig> rules = { rule1, rule2 };

    WindowMatchInfo info{ .wm_class = "Firefox",
                          .wm_class_name = "Navigator",
                          .title = "Test",
                          .ewmh_type = WindowType::Normal,
                          .is_transient = false };

    std::vector<std::string> workspace_names = { "1", "2", "3", "4", "5", "6" };
    auto result = match_window_rules(rules, info, { }, workspace_names);

    REQUIRE(result.matched);
    REQUIRE(result.floating.has_value());
    REQUIRE(*result.floating == true); // From first rule
    REQUIRE(result.target_workspace.has_value());
    REQUIRE(*result.target_workspace == 5); // From first rule
}

TEST_CASE("Window type matching", "[rules]")
{
    WindowRuleConfig cfg;
    cfg.type = WindowType::Dialog;
    cfg.floating = true;

    std::vector<WindowRuleConfig> rules = { cfg };

    SECTION("Dialog type matches")
    {
        WindowMatchInfo info{ .wm_class = "Firefox",
                              .wm_class_name = "Navigator",
                              .title = "Preferences",
                              .ewmh_type = WindowType::Dialog,
                              .is_transient = false };

        auto result = match_window_rules(rules, info, { }, { });

        REQUIRE(result.matched);
    }

    SECTION("Normal type does not match dialog rule")
    {
        WindowMatchInfo info{ .wm_class = "Firefox",
                              .wm_class_name = "Navigator",
                              .title = "Preferences",
                              .ewmh_type = WindowType::Normal,
                              .is_transient = false };

        auto result = match_window_rules(rules, info, { }, { });

        REQUIRE_FALSE(result.matched);
    }
}

TEST_CASE("Transient flag matching", "[rules]")
{
    WindowRuleConfig cfg;
    cfg.transient = true;
    cfg.floating = true;

    std::vector<WindowRuleConfig> rules = { cfg };

    SECTION("Transient window matches")
    {
        WindowMatchInfo info{ .wm_class = "Firefox",
                              .wm_class_name = "Navigator",
                              .title = "Dialog",
                              .ewmh_type = WindowType::Normal,
                              .is_transient = true };

        auto result = match_window_rules(rules, info, { }, { });

        REQUIRE(result.matched);
    }

    SECTION("Non-transient window does not match")
    {
        WindowMatchInfo info{ .wm_class = "Firefox",
                              .wm_class_name = "Navigator",
                              .title = "Main Window",
                              .ewmh_type = WindowType::Normal,
                              .is_transient = false };

        auto result = match_window_rules(rules, info, { }, { });

        REQUIRE_FALSE(result.matched);
    }
}

TEST_CASE("Instance name matching", "[rules]")
{
    WindowRuleConfig cfg;
    cfg.match.instance_regex.emplace("Navigator");
    cfg.floating = true;

    std::vector<WindowRuleConfig> rules = { cfg };

    SECTION("Instance name matches")
    {
        WindowMatchInfo info{ .wm_class = "Firefox",
                              .wm_class_name = "Navigator",
                              .title = "Test",
                              .ewmh_type = WindowType::Normal,
                              .is_transient = false };

        auto result = match_window_rules(rules, info, { }, { });

        REQUIRE(result.matched);
    }

    SECTION("Different instance name does not match")
    {
        WindowMatchInfo info{ .wm_class = "Firefox",
                              .wm_class_name = "Toolbox",
                              .title = "Test",
                              .ewmh_type = WindowType::Normal,
                              .is_transient = false };

        auto result = match_window_rules(rules, info, { }, { });

        REQUIRE_FALSE(result.matched);
    }
}

TEST_CASE("Workspace index resolution", "[rules]")
{
    WindowRuleConfig cfg;
    cfg.match.class_regex.emplace("Test");
    cfg.workspace = 2;

    std::vector<WindowRuleConfig> rules = { cfg };

    std::vector<std::string> workspace_names = { "1", "2", "3", "4", "5" };

    WindowMatchInfo info{ .wm_class = "Test",
                          .wm_class_name = "test",
                          .title = "Test",
                          .ewmh_type = WindowType::Normal,
                          .is_transient = false };

    auto result = match_window_rules(rules, info, { }, workspace_names);

    REQUIRE(result.matched);
    REQUIRE(result.target_workspace.has_value());
    REQUIRE(*result.target_workspace == 2);
}

TEST_CASE("Workspace name resolution", "[rules]")
{
    WindowRuleConfig cfg;
    cfg.match.class_regex.emplace("Test");
    cfg.workspace_name = "dev";

    std::vector<WindowRuleConfig> rules = { cfg };

    std::vector<std::string> workspace_names = { "main", "web", "dev", "chat" };

    WindowMatchInfo info{ .wm_class = "Test",
                          .wm_class_name = "test",
                          .title = "Test",
                          .ewmh_type = WindowType::Normal,
                          .is_transient = false };

    auto result = match_window_rules(rules, info, { }, workspace_names);

    REQUIRE(result.matched);
    REQUIRE(result.target_workspace.has_value());
    REQUIRE(*result.target_workspace == 2); // "dev" is at index 2
}

TEST_CASE("Monitor index resolution", "[rules]")
{
    WindowRuleConfig cfg;
    cfg.match.class_regex.emplace("Test");
    cfg.monitor = 1;

    std::vector<WindowRuleConfig> rules = { cfg };

    std::vector<Monitor> monitors = { make_monitor("DP-1"), make_monitor("HDMI-1") };

    WindowMatchInfo info{ .wm_class = "Test",
                          .wm_class_name = "test",
                          .title = "Test",
                          .ewmh_type = WindowType::Normal,
                          .is_transient = false };

    auto result = match_window_rules(rules, info, monitors, { });

    REQUIRE(result.matched);
    REQUIRE(result.target_monitor.has_value());
    REQUIRE(*result.target_monitor == 1);
}

TEST_CASE("Monitor name resolution", "[rules]")
{
    WindowRuleConfig cfg;
    cfg.match.class_regex.emplace("Test");
    cfg.monitor_name = "HDMI-1";

    std::vector<WindowRuleConfig> rules = { cfg };

    std::vector<Monitor> monitors = { make_monitor("DP-1"), make_monitor("HDMI-1") };

    WindowMatchInfo info{ .wm_class = "Test",
                          .wm_class_name = "test",
                          .title = "Test",
                          .ewmh_type = WindowType::Normal,
                          .is_transient = false };

    auto result = match_window_rules(rules, info, monitors, { });

    REQUIRE(result.matched);
    REQUIRE(result.target_monitor.has_value());
    REQUIRE(*result.target_monitor == 1); // "HDMI-1" is at index 1
}

TEST_CASE("Invalid monitor/workspace returns nullopt", "[rules][edge]")
{
    WindowMatchInfo info{ .wm_class = "Test",
                          .wm_class_name = "test",
                          .title = "Test",
                          .ewmh_type = WindowType::Normal,
                          .is_transient = false };

    SECTION("Out of range and negative indices")
    {
        WindowRuleConfig cfg;
        cfg.match.class_regex.emplace("Test");
        cfg.workspace = 99;
        cfg.monitor = 99;

        std::vector<WindowRuleConfig> rules = { cfg };

        std::vector<Monitor> monitors = { make_monitor("DP-1") };
        std::vector<std::string> workspace_names = { "1", "2", "3" };

        auto result = match_window_rules(rules, info, monitors, workspace_names);

        REQUIRE(result.matched);
        REQUIRE_FALSE(result.target_workspace.has_value());
        REQUIRE_FALSE(result.target_monitor.has_value());
    }

    SECTION("Empty monitor and workspace lists")
    {
        WindowRuleConfig mon_cfg;
        mon_cfg.match.class_regex.emplace("Test");
        mon_cfg.monitor = 0;

        std::vector<WindowRuleConfig> mon_rules = { mon_cfg };

        std::vector<Monitor> empty_monitors;
        auto mon_result = match_window_rules(mon_rules, info, empty_monitors, { });
        REQUIRE(mon_result.matched);
        REQUIRE_FALSE(mon_result.target_monitor.has_value());

        WindowRuleConfig ws_cfg;
        ws_cfg.match.class_regex.emplace("Test");
        ws_cfg.workspace = 0;

        std::vector<WindowRuleConfig> ws_rules = { ws_cfg };

        std::vector<std::string> empty_workspaces;
        auto ws_result = match_window_rules(ws_rules, info, { }, empty_workspaces);
        REQUIRE(ws_result.matched);
        REQUIRE_FALSE(ws_result.target_workspace.has_value());
    }

    SECTION("Negative indices")
    {
        WindowRuleConfig cfg_ws;
        cfg_ws.match.class_regex.emplace("Test");
        cfg_ws.workspace = -1;

        WindowRuleConfig cfg_mon;
        cfg_mon.match.class_regex.emplace("Test");
        cfg_mon.monitor = -5;

        std::vector<WindowRuleConfig> rules = { cfg_ws, cfg_mon };

        std::vector<std::string> workspace_names = { "1", "2", "3" };
        std::vector<Monitor> monitors = { make_monitor("DP-1"), make_monitor("HDMI-1") };

        auto result_ws = match_window_rules(rules, info, monitors, workspace_names);
        REQUIRE(result_ws.matched);
        REQUIRE_FALSE(result_ws.target_workspace.has_value());

        rules = { cfg_mon };
        auto result_mon = match_window_rules(rules, info, monitors, workspace_names);
        REQUIRE(result_mon.matched);
        REQUIRE_FALSE(result_mon.target_monitor.has_value());
    }
}

TEST_CASE("State flags are preserved", "[rules]")
{
    WindowRuleConfig cfg;
    cfg.match.class_regex.emplace("Test");
    cfg.fullscreen = true;
    cfg.above = true;
    cfg.sticky = true;
    cfg.skip_taskbar = true;

    std::vector<WindowRuleConfig> rules = { cfg };

    WindowMatchInfo info{ .wm_class = "Test",
                          .wm_class_name = "test",
                          .title = "Test",
                          .ewmh_type = WindowType::Normal,
                          .is_transient = false };

    auto result = match_window_rules(rules, info, { }, { });

    REQUIRE(result.matched);
    REQUIRE(result.fullscreen.has_value());
    REQUIRE(*result.fullscreen == true);
    REQUIRE(result.layer_hint.has_value());
    REQUIRE(*result.layer_hint == LayerHint::Above);
    REQUIRE(result.sticky.has_value());
    REQUIRE(*result.sticky == true);
    REQUIRE(result.skip_taskbar.has_value());
    REQUIRE(*result.skip_taskbar == true);
}

TEST_CASE("Geometry preservation", "[rules]")
{
    WindowRuleConfig cfg;
    cfg.match.class_regex.emplace("Test");

    RuleGeometry geo;
    geo.x = 100;
    geo.y = 200;
    geo.width = 800;
    geo.height = 600;
    cfg.geometry = geo;

    std::vector<WindowRuleConfig> rules = { cfg };

    WindowMatchInfo info{ .wm_class = "Test",
                          .wm_class_name = "test",
                          .title = "Test",
                          .ewmh_type = WindowType::Normal,
                          .is_transient = false };

    auto result = match_window_rules(rules, info, { }, { });

    REQUIRE(result.matched);
    REQUIRE(result.geometry.has_value());
    REQUIRE(result.geometry->x == 100);
    REQUIRE(result.geometry->y == 200);
    REQUIRE(result.geometry->width == 800);
    REQUIRE(result.geometry->height == 600);
}

TEST_CASE("Center flag is preserved", "[rules]")
{
    WindowRuleConfig cfg;
    cfg.match.class_regex.emplace("Test");
    cfg.center = true;

    std::vector<WindowRuleConfig> rules = { cfg };

    WindowMatchInfo info{ .wm_class = "Test",
                          .wm_class_name = "test",
                          .title = "Test",
                          .ewmh_type = WindowType::Normal,
                          .is_transient = false };

    auto result = match_window_rules(rules, info, { }, { });

    REQUIRE(result.matched);
    REQUIRE(result.center == true);
}

TEST_CASE("No criteria matches all windows", "[rules]")
{
    // Rule with no criteria should match everything
    WindowRuleConfig cfg;
    cfg.floating = true;

    std::vector<WindowRuleConfig> rules = { cfg };

    WindowMatchInfo info{ .wm_class = "AnyClass",
                          .wm_class_name = "any",
                          .title = "Any Title",
                          .ewmh_type = WindowType::Normal,
                          .is_transient = false };

    auto result = match_window_rules(rules, info, { }, { });

    REQUIRE(result.matched);
    REQUIRE(result.floating.has_value());
    REQUIRE(*result.floating == true);
}

TEST_CASE("Duplicate names resolve to first occurrence", "[rules][edge]")
{
    WindowMatchInfo info{ .wm_class = "Test",
                          .wm_class_name = "test",
                          .title = "Test",
                          .ewmh_type = WindowType::Normal,
                          .is_transient = false };

    WindowRuleConfig cfg;
    cfg.match.class_regex.emplace("Test");
    cfg.workspace_name = "dev";
    cfg.monitor_name = "DP-1";

    std::vector<WindowRuleConfig> rules = { cfg };

    std::vector<std::string> workspace_names = { "dev", "main", "dev" };
    std::vector<Monitor> monitors = { make_monitor("DP-1"), make_monitor("HDMI-1"), make_monitor("DP-1") };

    SECTION("Duplicate workspace names")
    {
        auto result = match_window_rules(rules, info, { }, workspace_names);
        REQUIRE(result.matched);
        REQUIRE(result.target_workspace.has_value());
        REQUIRE(*result.target_workspace == 0);
    }

    SECTION("Duplicate monitor names")
    {
        auto result = match_window_rules(rules, info, monitors, { });
        REQUIRE(result.matched);
        REQUIRE(result.target_monitor.has_value());
        REQUIRE(*result.target_monitor == 0);
    }
}

TEST_CASE("Rule geometry with missing optional fields", "[rules][edge]")
{
    WindowRuleConfig cfg;
    cfg.match.class_regex.emplace("Test");

    RuleGeometry geo;
    geo.x = 100;
    // y, width, height not set (nullopt)
    cfg.geometry = geo;

    std::vector<WindowRuleConfig> rules = { cfg };

    WindowMatchInfo info{ .wm_class = "Test",
                          .wm_class_name = "test",
                          .title = "Test",
                          .ewmh_type = WindowType::Normal,
                          .is_transient = false };

    auto result = match_window_rules(rules, info, { }, { });

    REQUIRE(result.matched);
    REQUIRE(result.geometry.has_value());
    // Missing fields should use defaults (0, 800, 600 from implementation)
    REQUIRE(result.geometry->x == 100);
    REQUIRE(result.geometry->y == 0);        // default
    REQUIRE(result.geometry->width == 800);  // default
    REQUIRE(result.geometry->height == 600); // default
}
