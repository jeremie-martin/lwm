#include "lwm/core/window_rules.hpp"
#include <catch2/catch_test_macros.hpp>

using namespace lwm;

namespace {

WindowMatchInfo info(std::string wm_class, std::string title = "Title", WindowType type = WindowType::Normal, bool transient = false)
{
    return { std::move(wm_class), "instance", std::move(title), type, transient };
}

WindowRuleConfig rule(std::optional<std::string> wm_class, bool floating)
{
    WindowRuleConfig config;
    if (wm_class)
        config.match.class_regex.emplace(*wm_class);
    config.actions.floating = floating;
    return config;
}

Monitor named(std::string name)
{
    Monitor monitor;
    monitor.name = std::move(name);
    return monitor;
}

} // namespace

TEST_CASE("Rules match whole values and every specified criterion", "[rules]")
{
    CHECK_FALSE(match_window_rules({ }, info("Firefox")));

    auto exact = rule("Firefox", true);
    std::vector rules{ exact };
    CHECK(match_window_rules(rules, info("Firefox")) == &rules[0].actions);
    CHECK_FALSE(match_window_rules(rules, info("Firefox Developer Edition")));

    auto strict = rule("Firefox", true);
    strict.match.title_regex.emplace(".*Video.*");
    strict.type = WindowType::Dialog;
    strict.transient = true;
    std::vector strict_rules{ strict };
    CHECK(match_window_rules(strict_rules, info("Firefox", "A Video", WindowType::Dialog, true)));
    CHECK_FALSE(match_window_rules(strict_rules, info("Firefox", "Music", WindowType::Dialog, true)));
    CHECK_FALSE(match_window_rules(strict_rules, info("Firefox", "A Video", WindowType::Normal, true)));
    CHECK_FALSE(match_window_rules(strict_rules, info("Firefox", "A Video", WindowType::Dialog, false)));

    // A rule without criteria matches every window.
    std::vector catch_all{ rule(std::nullopt, false) };
    CHECK(match_window_rules(catch_all, info("Anything")));
}

TEST_CASE("The first matching rule supplies all actions", "[rules]")
{
    std::vector rules{ rule("Term.*", true), rule("Terminal", false) };
    auto const* matched = match_window_rules(rules, info("Terminal"));
    REQUIRE(matched == &rules[0].actions);
    CHECK(matched->floating == true);
}

TEST_CASE("Rule monitors resolve against current outputs when applied", "[rules]")
{
    std::vector monitors{ named("DP-1"), named("HDMI-1"), named("HDMI-1") };
    RuleActions actions;
    CHECK_FALSE(resolve_rule_monitor(actions, monitors));
    actions.monitor = size_t{ 1 };
    CHECK(resolve_rule_monitor(actions, monitors) == 1);
    actions.monitor = size_t{ 3 };
    CHECK_FALSE(resolve_rule_monitor(actions, monitors));
    actions.monitor = std::string("HDMI-1");
    CHECK(resolve_rule_monitor(actions, monitors) == 1);
    actions.monitor = std::string("eDP-1");
    CHECK_FALSE(resolve_rule_monitor(actions, monitors));
}
