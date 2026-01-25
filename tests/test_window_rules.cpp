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
    WindowRules rules;
    rules.load_rules({});

    WindowMatchInfo info{ .wm_class = "Firefox",
                          .wm_class_name = "Navigator",
                          .title = "Test",
                          .ewmh_type = WindowType::Normal,
                          .is_transient = false };

    auto result = rules.match(info, {}, {});

    REQUIRE_FALSE(result.matched);
}

TEST_CASE("Exact class name matching", "[rules]")
{
    WindowRuleConfig cfg;
    cfg.class_pattern = "Firefox";
    cfg.floating = true;

    WindowRules rules;
    rules.load_rules({ cfg });

    SECTION("Exact match succeeds")
    {
        WindowMatchInfo info{ .wm_class = "Firefox",
                              .wm_class_name = "Navigator",
                              .title = "Test",
                              .ewmh_type = WindowType::Normal,
                              .is_transient = false };

        auto result = rules.match(info, {}, {});

        REQUIRE(result.matched);
        REQUIRE(result.floating.has_value());
        REQUIRE(*result.floating == true);
    }

    SECTION("Partial match succeeds (regex search)")
    {
        WindowMatchInfo info{ .wm_class = "Firefox Developer Edition",
                              .wm_class_name = "Navigator",
                              .title = "Test",
                              .ewmh_type = WindowType::Normal,
                              .is_transient = false };

        auto result = rules.match(info, {}, {});

        REQUIRE(result.matched);
    }

    SECTION("Non-matching class fails")
    {
        WindowMatchInfo info{ .wm_class = "Chrome",
                              .wm_class_name = "Navigator",
                              .title = "Test",
                              .ewmh_type = WindowType::Normal,
                              .is_transient = false };

        auto result = rules.match(info, {}, {});

        REQUIRE_FALSE(result.matched);
    }
}

TEST_CASE("Regex pattern matching", "[rules]")
{
    WindowRuleConfig cfg;
    cfg.title_pattern = ".*YouTube.*";
    cfg.floating = true;

    WindowRules rules;
    rules.load_rules({ cfg });

    SECTION("Regex matches")
    {
        WindowMatchInfo info{ .wm_class = "Firefox",
                              .wm_class_name = "Navigator",
                              .title = "Watching YouTube Videos",
                              .ewmh_type = WindowType::Normal,
                              .is_transient = false };

        auto result = rules.match(info, {}, {});

        REQUIRE(result.matched);
    }

    SECTION("Regex does not match")
    {
        WindowMatchInfo info{ .wm_class = "Firefox",
                              .wm_class_name = "Navigator",
                              .title = "GitHub - Code Repository",
                              .ewmh_type = WindowType::Normal,
                              .is_transient = false };

        auto result = rules.match(info, {}, {});

        REQUIRE_FALSE(result.matched);
    }
}

TEST_CASE("AND logic - all criteria must match", "[rules]")
{
    WindowRuleConfig cfg;
    cfg.class_pattern = "Firefox";
    cfg.title_pattern = ".*YouTube.*";
    cfg.floating = true;

    WindowRules rules;
    rules.load_rules({ cfg });

    SECTION("Both class and title match")
    {
        WindowMatchInfo info{ .wm_class = "Firefox",
                              .wm_class_name = "Navigator",
                              .title = "YouTube - Music",
                              .ewmh_type = WindowType::Normal,
                              .is_transient = false };

        auto result = rules.match(info, {}, {});

        REQUIRE(result.matched);
    }

    SECTION("Class matches but title does not")
    {
        WindowMatchInfo info{ .wm_class = "Firefox",
                              .wm_class_name = "Navigator",
                              .title = "GitHub",
                              .ewmh_type = WindowType::Normal,
                              .is_transient = false };

        auto result = rules.match(info, {}, {});

        REQUIRE_FALSE(result.matched);
    }

    SECTION("Title matches but class does not")
    {
        WindowMatchInfo info{ .wm_class = "Chrome",
                              .wm_class_name = "chrome",
                              .title = "YouTube",
                              .ewmh_type = WindowType::Normal,
                              .is_transient = false };

        auto result = rules.match(info, {}, {});

        REQUIRE_FALSE(result.matched);
    }
}

TEST_CASE("First match wins", "[rules]")
{
    WindowRuleConfig rule1;
    rule1.class_pattern = "Firefox";
    rule1.floating = true;
    rule1.workspace = 5;

    WindowRuleConfig rule2;
    rule2.class_pattern = "Firefox";
    rule2.floating = false;
    rule2.workspace = 3;

    WindowRules rules;
    rules.load_rules({ rule1, rule2 });

    WindowMatchInfo info{ .wm_class = "Firefox",
                          .wm_class_name = "Navigator",
                          .title = "Test",
                          .ewmh_type = WindowType::Normal,
                          .is_transient = false };

    std::vector<std::string> workspace_names = { "1", "2", "3", "4", "5", "6" };
    auto result = rules.match(info, {}, workspace_names);

    REQUIRE(result.matched);
    REQUIRE(result.floating.has_value());
    REQUIRE(*result.floating == true); // From first rule
    REQUIRE(result.target_workspace.has_value());
    REQUIRE(*result.target_workspace == 5); // From first rule
}

TEST_CASE("Window type matching", "[rules]")
{
    WindowRuleConfig cfg;
    cfg.type = "dialog";
    cfg.floating = true;

    WindowRules rules;
    rules.load_rules({ cfg });

    SECTION("Dialog type matches")
    {
        WindowMatchInfo info{ .wm_class = "Firefox",
                              .wm_class_name = "Navigator",
                              .title = "Preferences",
                              .ewmh_type = WindowType::Dialog,
                              .is_transient = false };

        auto result = rules.match(info, {}, {});

        REQUIRE(result.matched);
    }

    SECTION("Normal type does not match dialog rule")
    {
        WindowMatchInfo info{ .wm_class = "Firefox",
                              .wm_class_name = "Navigator",
                              .title = "Preferences",
                              .ewmh_type = WindowType::Normal,
                              .is_transient = false };

        auto result = rules.match(info, {}, {});

        REQUIRE_FALSE(result.matched);
    }
}

TEST_CASE("Transient flag matching", "[rules]")
{
    WindowRuleConfig cfg;
    cfg.transient = true;
    cfg.floating = true;

    WindowRules rules;
    rules.load_rules({ cfg });

    SECTION("Transient window matches")
    {
        WindowMatchInfo info{ .wm_class = "Firefox",
                              .wm_class_name = "Navigator",
                              .title = "Dialog",
                              .ewmh_type = WindowType::Normal,
                              .is_transient = true };

        auto result = rules.match(info, {}, {});

        REQUIRE(result.matched);
    }

    SECTION("Non-transient window does not match")
    {
        WindowMatchInfo info{ .wm_class = "Firefox",
                              .wm_class_name = "Navigator",
                              .title = "Main Window",
                              .ewmh_type = WindowType::Normal,
                              .is_transient = false };

        auto result = rules.match(info, {}, {});

        REQUIRE_FALSE(result.matched);
    }
}

TEST_CASE("Instance name matching", "[rules]")
{
    WindowRuleConfig cfg;
    cfg.instance_pattern = "Navigator";
    cfg.floating = true;

    WindowRules rules;
    rules.load_rules({ cfg });

    SECTION("Instance name matches")
    {
        WindowMatchInfo info{ .wm_class = "Firefox",
                              .wm_class_name = "Navigator",
                              .title = "Test",
                              .ewmh_type = WindowType::Normal,
                              .is_transient = false };

        auto result = rules.match(info, {}, {});

        REQUIRE(result.matched);
    }

    SECTION("Different instance name does not match")
    {
        WindowMatchInfo info{ .wm_class = "Firefox",
                              .wm_class_name = "Toolbox",
                              .title = "Test",
                              .ewmh_type = WindowType::Normal,
                              .is_transient = false };

        auto result = rules.match(info, {}, {});

        REQUIRE_FALSE(result.matched);
    }
}

TEST_CASE("Workspace index resolution", "[rules]")
{
    WindowRuleConfig cfg;
    cfg.class_pattern = "Test";
    cfg.workspace = 2;

    WindowRules rules;
    rules.load_rules({ cfg });

    std::vector<std::string> workspace_names = { "1", "2", "3", "4", "5" };

    WindowMatchInfo info{ .wm_class = "Test",
                          .wm_class_name = "test",
                          .title = "Test",
                          .ewmh_type = WindowType::Normal,
                          .is_transient = false };

    auto result = rules.match(info, {}, workspace_names);

    REQUIRE(result.matched);
    REQUIRE(result.target_workspace.has_value());
    REQUIRE(*result.target_workspace == 2);
}

TEST_CASE("Workspace name resolution", "[rules]")
{
    WindowRuleConfig cfg;
    cfg.class_pattern = "Test";
    cfg.workspace_name = "dev";

    WindowRules rules;
    rules.load_rules({ cfg });

    std::vector<std::string> workspace_names = { "main", "web", "dev", "chat" };

    WindowMatchInfo info{ .wm_class = "Test",
                          .wm_class_name = "test",
                          .title = "Test",
                          .ewmh_type = WindowType::Normal,
                          .is_transient = false };

    auto result = rules.match(info, {}, workspace_names);

    REQUIRE(result.matched);
    REQUIRE(result.target_workspace.has_value());
    REQUIRE(*result.target_workspace == 2); // "dev" is at index 2
}

TEST_CASE("Monitor index resolution", "[rules]")
{
    WindowRuleConfig cfg;
    cfg.class_pattern = "Test";
    cfg.monitor = 1;

    WindowRules rules;
    rules.load_rules({ cfg });

    std::vector<Monitor> monitors = { make_monitor("DP-1"), make_monitor("HDMI-1") };

    WindowMatchInfo info{ .wm_class = "Test",
                          .wm_class_name = "test",
                          .title = "Test",
                          .ewmh_type = WindowType::Normal,
                          .is_transient = false };

    auto result = rules.match(info, monitors, {});

    REQUIRE(result.matched);
    REQUIRE(result.target_monitor.has_value());
    REQUIRE(*result.target_monitor == 1);
}

TEST_CASE("Monitor name resolution", "[rules]")
{
    WindowRuleConfig cfg;
    cfg.class_pattern = "Test";
    cfg.monitor_name = "HDMI-1";

    WindowRules rules;
    rules.load_rules({ cfg });

    std::vector<Monitor> monitors = { make_monitor("DP-1"), make_monitor("HDMI-1") };

    WindowMatchInfo info{ .wm_class = "Test",
                          .wm_class_name = "test",
                          .title = "Test",
                          .ewmh_type = WindowType::Normal,
                          .is_transient = false };

    auto result = rules.match(info, monitors, {});

    REQUIRE(result.matched);
    REQUIRE(result.target_monitor.has_value());
    REQUIRE(*result.target_monitor == 1); // "HDMI-1" is at index 1
}

TEST_CASE("Invalid monitor/workspace returns nullopt", "[rules]")
{
    WindowRuleConfig cfg;
    cfg.class_pattern = "Test";
    cfg.workspace = 99; // Out of range
    cfg.monitor = 99;   // Out of range

    WindowRules rules;
    rules.load_rules({ cfg });

    std::vector<Monitor> monitors = { make_monitor("DP-1") };
    std::vector<std::string> workspace_names = { "1", "2", "3" };

    WindowMatchInfo info{ .wm_class = "Test",
                          .wm_class_name = "test",
                          .title = "Test",
                          .ewmh_type = WindowType::Normal,
                          .is_transient = false };

    auto result = rules.match(info, monitors, workspace_names);

    REQUIRE(result.matched);
    REQUIRE_FALSE(result.target_workspace.has_value());
    REQUIRE_FALSE(result.target_monitor.has_value());
}

TEST_CASE("Geometry preservation", "[rules]")
{
    WindowRuleConfig cfg;
    cfg.class_pattern = "Test";

    RuleGeometry geo;
    geo.x = 100;
    geo.y = 200;
    geo.width = 800;
    geo.height = 600;
    cfg.geometry = geo;

    WindowRules rules;
    rules.load_rules({ cfg });

    WindowMatchInfo info{ .wm_class = "Test",
                          .wm_class_name = "test",
                          .title = "Test",
                          .ewmh_type = WindowType::Normal,
                          .is_transient = false };

    auto result = rules.match(info, {}, {});

    REQUIRE(result.matched);
    REQUIRE(result.geometry.has_value());
    REQUIRE(result.geometry->x == 100);
    REQUIRE(result.geometry->y == 200);
    REQUIRE(result.geometry->width == 800);
    REQUIRE(result.geometry->height == 600);
}

TEST_CASE("State flags are preserved", "[rules]")
{
    WindowRuleConfig cfg;
    cfg.class_pattern = "Test";
    cfg.fullscreen = true;
    cfg.above = true;
    cfg.sticky = true;
    cfg.skip_taskbar = true;

    WindowRules rules;
    rules.load_rules({ cfg });

    WindowMatchInfo info{ .wm_class = "Test",
                          .wm_class_name = "test",
                          .title = "Test",
                          .ewmh_type = WindowType::Normal,
                          .is_transient = false };

    auto result = rules.match(info, {}, {});

    REQUIRE(result.matched);
    REQUIRE(result.fullscreen.has_value());
    REQUIRE(*result.fullscreen == true);
    REQUIRE(result.above.has_value());
    REQUIRE(*result.above == true);
    REQUIRE(result.sticky.has_value());
    REQUIRE(*result.sticky == true);
    REQUIRE(result.skip_taskbar.has_value());
    REQUIRE(*result.skip_taskbar == true);
}

TEST_CASE("Center flag is preserved", "[rules]")
{
    WindowRuleConfig cfg;
    cfg.class_pattern = "Test";
    cfg.center = true;

    WindowRules rules;
    rules.load_rules({ cfg });

    WindowMatchInfo info{ .wm_class = "Test",
                          .wm_class_name = "test",
                          .title = "Test",
                          .ewmh_type = WindowType::Normal,
                          .is_transient = false };

    auto result = rules.match(info, {}, {});

    REQUIRE(result.matched);
    REQUIRE(result.center == true);
}

TEST_CASE("Rule count is tracked", "[rules]")
{
    WindowRules rules;
    REQUIRE(rules.rule_count() == 0);

    WindowRuleConfig cfg1, cfg2, cfg3;
    cfg1.class_pattern = "Test1";
    cfg2.class_pattern = "Test2";
    cfg3.class_pattern = "Test3";

    rules.load_rules({ cfg1, cfg2, cfg3 });
    REQUIRE(rules.rule_count() == 3);

    rules.load_rules({});
    REQUIRE(rules.rule_count() == 0);
}

TEST_CASE("Window type string parsing is case-insensitive", "[rules]")
{
    WindowRuleConfig cfg;
    cfg.type = "DIALOG"; // uppercase
    cfg.floating = true;

    WindowRules rules;
    rules.load_rules({ cfg });

    WindowMatchInfo info{ .wm_class = "Test",
                          .wm_class_name = "test",
                          .title = "Test",
                          .ewmh_type = WindowType::Dialog,
                          .is_transient = false };

    auto result = rules.match(info, {}, {});

    REQUIRE(result.matched);
}

TEST_CASE("No criteria matches all windows", "[rules]")
{
    // Rule with no criteria should match everything
    WindowRuleConfig cfg;
    cfg.floating = true;

    WindowRules rules;
    rules.load_rules({ cfg });

    WindowMatchInfo info{ .wm_class = "AnyClass",
                          .wm_class_name = "any",
                          .title = "Any Title",
                          .ewmh_type = WindowType::Normal,
                          .is_transient = false };

    auto result = rules.match(info, {}, {});

    REQUIRE(result.matched);
    REQUIRE(result.floating.has_value());
    REQUIRE(*result.floating == true);
}

TEST_CASE("Empty pattern is treated as no filter", "[rules][edge]")
{
    SECTION("Empty class pattern")
    {
        WindowRuleConfig cfg;
        cfg.class_pattern = "";
        cfg.floating = true;

        WindowRules rules;
        rules.load_rules({ cfg });

        WindowMatchInfo info{ .wm_class = "AnyClass",
                              .wm_class_name = "any",
                              .title = "Any Title",
                              .ewmh_type = WindowType::Normal,
                              .is_transient = false };

        auto result = rules.match(info, {}, {});
        REQUIRE(result.matched);
    }

    SECTION("Empty title pattern")
    {
        WindowRuleConfig cfg;
        cfg.title_pattern = "";
        cfg.floating = true;

        WindowRules rules;
        rules.load_rules({ cfg });

        WindowMatchInfo info{ .wm_class = "AnyClass",
                              .wm_class_name = "any",
                              .title = "Any Title",
                              .ewmh_type = WindowType::Normal,
                              .is_transient = false };

        auto result = rules.match(info, {}, {});
        REQUIRE(result.matched);
    }
}

TEST_CASE("Malformed regex pattern falls back to literal match", "[rules][edge]")
{
    WindowRuleConfig cfg;
    cfg.class_pattern = "[invalid(regex"; // Invalid regex
    cfg.floating = true;

    WindowRules rules;
    rules.load_rules({ cfg });

    // Should match the literal string "[invalid(regex"
    WindowMatchInfo info{ .wm_class = "[invalid(regex",
                          .wm_class_name = "any",
                          .title = "Any Title",
                          .ewmh_type = WindowType::Normal,
                          .is_transient = false };

    auto result = rules.match(info, {}, {});

    REQUIRE(result.matched);
}

TEST_CASE("Unknown window type string is treated as no filter", "[rules][edge]")
{
    WindowRuleConfig cfg;
    cfg.type = "not_a_real_type"; // Invalid type string
    cfg.floating = true;

    WindowRules rules;
    rules.load_rules({ cfg });

    // Rule should load without crashing, and invalid type is treated as no constraint
    REQUIRE(rules.rule_count() == 1);

    WindowMatchInfo info{ .wm_class = "Test",
                          .wm_class_name = "test",
                          .title = "Test",
                          .ewmh_type = WindowType::Normal,
                          .is_transient = false };

    auto result = rules.match(info, {}, {});

    // Rule matches because invalid type doesn't act as a constraint
    REQUIRE(result.matched);
    REQUIRE(*result.floating == true);
}

TEST_CASE("Duplicate names resolve to first occurrence", "[rules][edge]")
{
    SECTION("Duplicate workspace names")
    {
        WindowRuleConfig cfg;
        cfg.class_pattern = "Test";
        cfg.workspace_name = "dev";

        WindowRules rules;
        rules.load_rules({ cfg });

        std::vector<std::string> workspace_names = { "dev", "main", "dev" };

        WindowMatchInfo info{ .wm_class = "Test",
                              .wm_class_name = "test",
                              .title = "Test",
                              .ewmh_type = WindowType::Normal,
                              .is_transient = false };

        auto result = rules.match(info, {}, workspace_names);
        REQUIRE(result.matched);
        REQUIRE(result.target_workspace.has_value());
        REQUIRE(*result.target_workspace == 0);
    }

    SECTION("Duplicate monitor names")
    {
        WindowRuleConfig cfg;
        cfg.class_pattern = "Test";
        cfg.monitor_name = "DP-1";

        WindowRules rules;
        rules.load_rules({ cfg });

        std::vector<Monitor> monitors = { make_monitor("DP-1"), make_monitor("HDMI-1"), make_monitor("DP-1") };

        WindowMatchInfo info{ .wm_class = "Test",
                              .wm_class_name = "test",
                              .title = "Test",
                              .ewmh_type = WindowType::Normal,
                              .is_transient = false };

        auto result = rules.match(info, monitors, {});
        REQUIRE(result.matched);
        REQUIRE(result.target_monitor.has_value());
        REQUIRE(*result.target_monitor == 0);
    }
}

TEST_CASE("Rule matching with empty monitor list", "[rules][edge]")
{
    WindowRuleConfig cfg;
    cfg.class_pattern = "Test";
    cfg.monitor = 0;

    WindowRules rules;
    rules.load_rules({ cfg });

    std::vector<Monitor> monitors; // Empty list

    WindowMatchInfo info{ .wm_class = "Test",
                          .wm_class_name = "test",
                          .title = "Test",
                          .ewmh_type = WindowType::Normal,
                          .is_transient = false };

    auto result = rules.match(info, monitors, {});

    REQUIRE(result.matched);
    // Monitor index out of range should return nullopt
    REQUIRE_FALSE(result.target_monitor.has_value());
}

TEST_CASE("Rule matching with empty workspace list", "[rules][edge]")
{
    WindowRuleConfig cfg;
    cfg.class_pattern = "Test";
    cfg.workspace = 0;

    WindowRules rules;
    rules.load_rules({ cfg });

    std::vector<std::string> workspace_names; // Empty list

    WindowMatchInfo info{ .wm_class = "Test",
                          .wm_class_name = "test",
                          .title = "Test",
                          .ewmh_type = WindowType::Normal,
                          .is_transient = false };

    auto result = rules.match(info, {}, workspace_names);

    REQUIRE(result.matched);
    // Workspace index out of range should return nullopt
    REQUIRE_FALSE(result.target_workspace.has_value());
}

TEST_CASE("Rule matching with negative workspace index", "[rules][edge]")
{
    WindowRuleConfig cfg;
    cfg.class_pattern = "Test";
    cfg.workspace = -1; // Negative index

    WindowRules rules;
    rules.load_rules({ cfg });

    std::vector<std::string> workspace_names = { "1", "2", "3" };

    WindowMatchInfo info{ .wm_class = "Test",
                          .wm_class_name = "test",
                          .title = "Test",
                          .ewmh_type = WindowType::Normal,
                          .is_transient = false };

    auto result = rules.match(info, {}, workspace_names);

    REQUIRE(result.matched);
    // Negative index should be rejected (fails *index >= 0 check)
    REQUIRE_FALSE(result.target_workspace.has_value());
}

TEST_CASE("Rule matching with negative monitor index", "[rules][edge]")
{
    WindowRuleConfig cfg;
    cfg.class_pattern = "Test";
    cfg.monitor = -5; // Negative index

    WindowRules rules;
    rules.load_rules({ cfg });

    std::vector<Monitor> monitors = { make_monitor("DP-1"), make_monitor("HDMI-1") };

    WindowMatchInfo info{ .wm_class = "Test",
                          .wm_class_name = "test",
                          .title = "Test",
                          .ewmh_type = WindowType::Normal,
                          .is_transient = false };

    auto result = rules.match(info, monitors, {});

    REQUIRE(result.matched);
    // Negative index should be rejected (fails *index >= 0 check)
    REQUIRE_FALSE(result.target_monitor.has_value());
}

TEST_CASE("Rule matching with pattern containing special regex characters", "[rules][edge]")
{
    WindowRuleConfig cfg;
    cfg.class_pattern = "Firefox.*"; // Contains regex special chars
    cfg.floating = true;

    WindowRules rules;
    rules.load_rules({ cfg });

    // Should match Firefox followed by anything (valid regex)
    WindowMatchInfo info{ .wm_class = "Firefox Developer Edition",
                          .wm_class_name = "Navigator",
                          .title = "Test",
                          .ewmh_type = WindowType::Normal,
                          .is_transient = false };

    auto result = rules.match(info, {}, {});

    REQUIRE(result.matched);
    REQUIRE(*result.floating == true);
}

TEST_CASE("Rule geometry with missing optional fields", "[rules][edge]")
{
    WindowRuleConfig cfg;
    cfg.class_pattern = "Test";

    RuleGeometry geo;
    geo.x = 100;
    // y, width, height not set (nullopt)
    cfg.geometry = geo;

    WindowRules rules;
    rules.load_rules({ cfg });

    WindowMatchInfo info{ .wm_class = "Test",
                          .wm_class_name = "test",
                          .title = "Test",
                          .ewmh_type = WindowType::Normal,
                          .is_transient = false };

    auto result = rules.match(info, {}, {});

    REQUIRE(result.matched);
    REQUIRE(result.geometry.has_value());
    // Missing fields should use defaults (0, 800, 600 from implementation)
    REQUIRE(result.geometry->x == 100);
    REQUIRE(result.geometry->y == 0);        // default
    REQUIRE(result.geometry->width == 800);  // default
    REQUIRE(result.geometry->height == 600); // default
}
