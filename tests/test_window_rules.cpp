#include "lwm/core/window_rules.hpp"
#include "state_fixture.hpp"
#include <catch2/catch_test_macros.hpp>

using namespace lwm;

namespace {

Client info(std::string wm_class, std::string title = "Title", WindowType type = WindowType::Normal, bool transient = false)
{
    Client client;
    client.wm_class = std::move(wm_class);
    client.wm_class_name = "instance";
    client.name = std::move(title);
    client.ewmh_type = type;
    client.transient_for = transient ? 1 : XCB_NONE;
    return client;
}

WindowRuleConfig rule(std::optional<std::string> wm_class, bool floating)
{
    WindowRuleConfig config;
    if (wm_class)
        config.match.class_regex.emplace(*wm_class);
    config.actions.floating = floating;
    return config;
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
    std::vector monitors{ test::monitor("DP-1"), test::monitor("HDMI-1"), test::monitor("HDMI-1") };
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

TEST_CASE("Metadata compares rule actions while reload deliberately reapplies them", "[state][rules]")
{
    auto state = test::state();
    test::add(state, 1);
    test::add(state, 2);
    Config config;
    auto first = rule(std::nullopt, true);
    first.match.title_regex.emplace("first");
    first.actions.geometry = RuleGeometry{ .position = std::pair<int16_t, int16_t>{ 70, 80 }, .width = 200, .height = 100 }; // A window inside a 2px border
    first.actions.fullscreen = true;
    auto second = first;
    second.match.title_regex.emplace("second");
    config.rules = { first, second };
    test::configure(state, [&](Config& installed) { installed.rules = config.rules; });

    state.title(1, "first");
    REQUIRE(floating_mode(state.require(1)));
    CHECK(floating_mode(state.require(1))->geometry == Geometry{ 70, 80, 204, 104 });
    state.geometry(1, { 10, 20, 300, 150 });
    state.fullscreen(2, true);
    state.title(1, "second");
    CHECK(floating_mode(state.require(1))->geometry == Geometry{ 10, 20, 300, 150 });
    CHECK(state.fullscreen_owners().at(0) == 2);

    test::configure(state, [](Config&) { });
    CHECK(floating_mode(state.require(1))->geometry == Geometry{ 70, 80, 204, 104 });
    CHECK(state.fullscreen_owners().at(0) == 2);
    state.title(1, "unmatched");
    CHECK_FALSE(state.require(1).rule);
    CHECK(state.require(1).fullscreen());
    CHECK(floating_mode(state.require(1))->geometry == Geometry{ 70, 80, 204, 104 });
}

TEST_CASE("Transient metadata places against the parent's resulting presentation", "[state][rules][placement]")
{
    auto state = test::state();
    test::configure(state, [](Config& config) { config.appearance = { .padding = 0, .border_width = 0 }; });
    test::add(state, 1);
    test::add(state, 2);
    REQUIRE(state.frame(state.require(1)) == Geometry{ 0, 0, 500, 800 });
    state.transient(2, 1);
    CHECK(state.frame(state.require(1)) == Geometry{ 0, 0, 1000, 800 });
    REQUIRE(floating_mode(state.require(2)));
    CHECK(floating_mode(state.require(2))->geometry == Geometry{ 250, 0, 500, 800 });

    SECTION("A tiled drag preview is the parent's presentation")
    {
        state.transient(2, XCB_NONE);
        state.begin_drag(State::Grip{ 1, floating::ResizeEdge::None }, 500, 400, 1);
        REQUIRE(state.drag());
        state.drag_to(400, 500);
        REQUIRE(state.frame(state.require(1)) == Geometry{ -100, 100, 500, 800 });
        state.transient(2, 1);
        CHECK(floating_mode(state.require(2))->geometry == Geometry{ 0, 0, 500, 800 });
    }
    SECTION("Explicit placement rules follow parent relocation")
    {
        test::configure(state, [](Config& config) { config.rules.push_back({ .transient = true, .actions = { .workspace = 1 } }); });
        state.transient(2, XCB_NONE);
        state.transient(2, 1);
        CHECK(state.require(2).workspace == 1);
    }
}

TEST_CASE("Pending metadata claims precede rule actions but reload does not claim", "[state][rules][scratchpad]")
{
    auto state = test::state(2);
    test::add(state, 1);
    Config config;
    config.scratchpads.push_back({ .name = "term", .width = 0.5, .height = 0.5 });
    auto matched = rule(std::nullopt, false);
    matched.match.title_regex.emplace("ready");
    matched.actions.workspace = 2;
    matched.actions.geometry = RuleGeometry{ .position = std::pair<int16_t, int16_t>{ 1, 2 }, .width = 300, .height = 200 };
    config.scratchpads.back().match = matched.match;
    config.rules.push_back(matched);
    test::configure(state, [&](Config& installed) {
        installed.scratchpads = config.scratchpads;
        installed.rules = config.rules;
    });
    state.scratchpad_pending("term", true);
    state.focus_monitor(1);

    state.title(1, "ready");
    CHECK(state.named_scratchpad("term")->claimed_window() == 1);
    CHECK(state.require(1).monitor == 1);
    CHECK(state.require(1).workspace == 0);
    CHECK(state.active_window() == 1);
    REQUIRE(floating_mode(state.require(1)));
    CHECK(floating_mode(state.require(1))->geometry == Geometry{ 1250, 200, 500, 400 });

    auto reloaded = test::state();
    test::add(reloaded, 1);
    reloaded.title(1, "ready");
    test::configure(reloaded, [&](Config& installed) { installed.scratchpads = config.scratchpads; });
    reloaded.scratchpad_pending("term", true);
    test::configure(reloaded, [&](Config& installed) { installed.rules = config.rules; });
    CHECK(reloaded.named_scratchpad("term")->pending_launch());
    CHECK_FALSE(reloaded.scratchpad_claim(1));
    CHECK(reloaded.require(1).workspace == 2);
    CHECK_FALSE(floating_mode(reloaded.require(1)));
    // A repeated title/class observation is inert; type notifications still reconcile.
    reloaded.title(1, "ready");
    CHECK(reloaded.named_scratchpad("term")->pending_launch());
    reloaded.window_type(1, WindowType::Normal);
    CHECK(reloaded.named_scratchpad("term")->claimed_window() == 1);
}

TEST_CASE("Named scratchpad operations refuse unmanaged clients and suppress pending launches", "[state][scratchpad]")
{
    auto state = test::state();
    test::configure(state, [](Config& config) { config.scratchpads = { { .name = "term" } }; });
    state.claim_scratchpad(999, state.config().scratchpads.front());
    CHECK(state.named_scratchpad("term")->claimed_window() == XCB_NONE);
    CHECK(state.toggle_scratchpad("term") == &state.config().scratchpads.front());
    state.scratchpad_pending("term", true);
    CHECK(state.toggle_scratchpad("term") == nullptr);
    CHECK_FALSE(state.toggle_scratchpad("unknown"));
}
