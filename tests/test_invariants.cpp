#include "lwm/core/focus.hpp"
#include "lwm/core/invariants.hpp"
#include "state_fixture.hpp"
#include <catch2/catch_test_macros.hpp>
#include <cstdlib>
#include <random>

using namespace lwm;
using test::add;
using test::add_floating;

TEST_CASE("Model validation accepts reachable states with independent fullscreen and iconic state", "[invariants]")
{
    auto state = test::state(2);
    add(state, 1);
    add_floating(state, 2);
    add(state, 3, { .monitor = 1, .workspace = 2 });
    state.insert_fixture(4, Fixture::Role::Dock);
    state.insert_fixture(5, Fixture::Role::Desktop);
    state.fullscreen(1, true);
    test::iconic(state, 1, true);
    state.fullscreen(2, true);
    state.pool_scratchpad(3);
    state.focus(2);
    CHECK_FALSE(invariants::validate(state));
}

TEST_CASE("Model validation rejects focus that completion must repair", "[invariants]")
{
    auto state = test::state();
    add(state, 1);
    state.focus(1);
    REQUIRE_FALSE(invariants::validate(state));
    SECTION("No input protocol") { state.focus_hints(1, false, false); }
    SECTION("Suppressed by fullscreen")
    {
        add(state, 2);
        state.fullscreen(2, true);
    }
    auto violation = invariants::validate(state);
    REQUIRE(violation);
    CHECK(violation->window == state.active_window());
}

// Every sequence of State operations keeps membership, placement and claims
// consistent. Focus is resolved through the same completion operation as the WM.
TEST_CASE("Generated State operation sequences preserve model invariants", "[invariants][sequence]")
{
    uint32_t seed = 12345;
    size_t steps = 4000;
    if (auto* value = std::getenv("LWM_TEST_SEQUENCE_SEED"))
        seed = static_cast<uint32_t>(std::stoul(value));
    if (auto* value = std::getenv("LWM_TEST_SEQUENCE_STEPS"))
        steps = std::stoul(value);
    REQUIRE(steps > 0);
    CAPTURE(seed, steps);
    std::mt19937 random(seed);
    std::string choices;
    auto pick = [&](size_t bound)
    {
        auto value = bound ? std::uniform_int_distribution<size_t>(0, bound - 1)(random) : 0;
        choices += std::to_string(value) + "/" + std::to_string(bound) + " ";
        return value;
    };
    auto state = test::state(2);
    test::configure(state, [](Config& config) { config.scratchpads = { { .name = "a" }, { .name = "b" } }; });
    std::vector<xcb_window_t> windows;
    xcb_window_t next = 1;
    std::vector<std::string> trace;
    for (size_t step = 0; step < steps; ++step)
    {
        choices.clear();
        auto const& monitors = state.monitors();
        size_t monitor = pick(monitors.size());
        size_t workspace = pick(monitors[monitor].workspaces.size());
        xcb_window_t window = windows.empty() ? XCB_NONE : windows[pick(windows.size())];
        switch (pick(windows.empty() ? 1 : 19))
        {
            case 0:
                add(state, next, { .monitor = monitor, .workspace = workspace, .floating = pick(2) == 0 });
                windows.push_back(next++);
                trace.push_back("insert");
                break;
            case 1:
                state.erase(window);
                std::erase(windows, window);
                trace.push_back("erase");
                break;
            case 2:
                state.relocate(window, monitor, workspace, State::RelocationGeometry(pick(3)), pick(4));
                trace.push_back("relocate");
                break;
            case 3:
                state.floating(window, pick(2) == 0);
                trace.push_back("floating");
                break;
            case 4:
                test::iconic(state, window, pick(2) == 0);
                trace.push_back("iconic");
                break;
            case 5:
                state.sticky(window, pick(2) == 0);
                trace.push_back("sticky");
                break;
            case 6:
                state.fullscreen(window, pick(2) == 0);
                trace.push_back("fullscreen");
                break;
            case 7:
                state.switch_workspace(monitor, workspace);
                trace.push_back("switch");
                break;
            case 8:
                state.focus(window);
                trace.push_back("focus");
                break;
            case 9:
                if (auto const& ws = monitors[monitor].current().windows; ws.size() >= 2)
                    state.swap_tiles(monitor, pick(ws.size()), pick(ws.size()));
                trace.push_back("swap");
                break;
            case 10:
                state.pool_scratchpad(window);
                trace.push_back("pool");
                break;
            case 11:
                if (auto const* name = pick(2) ? "a" : "b"; state.named_scratchpad(name))
                    state.claim_scratchpad(window, ScratchpadConfig{ .name = name });
                trace.push_back("claim");
                break;
            case 12:
                test::configure(state, [&](Config& config) {
                    config.scratchpads = pick(2) ? std::vector<ScratchpadConfig>{ { .name = "a" } }
                                                 : std::vector<ScratchpadConfig>{ { .name = "a" }, { .name = "b" } };
                });
                trace.push_back("configure");
                break;
            case 13:
            {
                std::vector<Topology::Output> outputs;
                for (size_t i = 0, count = 1 + pick(3); i < count; ++i)
                    outputs.push_back(test::output("M" + std::to_string(pick(3)), static_cast<int16_t>(i * 1000)));
                std::ranges::sort(outputs, { }, &Topology::Output::name);
                auto [first, last] = std::ranges::unique(outputs, { }, &Topology::Output::name);
                outputs.erase(first, last);
                test::outputs(state, std::move(outputs));
                trace.push_back("topology");
                break;
            }
            case 14:
                state.stash(window);
                trace.push_back("stash");
                break;
            case 15:
                state.window_type(window, pick(2) ? WindowType::Dialog : WindowType::Normal);
                trace.push_back("type");
                break;
            case 16:
                state.transient(window, pick(2) ? windows[pick(windows.size())] : XCB_NONE);
                trace.push_back("transient");
                break;
            case 17:
                state.cycle_focus(pick(2) == 0);
                trace.push_back("cycle focus");
                break;
            case 18:
                state.request_fullscreen(window);
                trace.push_back("request fullscreen");
                break;
        }
        trace.back() += " draws(value/bound): " + choices;
        state.settle();
        CAPTURE(step);
        // The registry must retain exactly the independently tracked live IDs.
        // Internal consistency alone would accept accidentally losing a client.
        REQUIRE(state.clients().size() == windows.size());
        for (auto id : windows) REQUIRE(state.find(id));
        if (auto violation = invariants::validate(state))
        {
            std::string replay;
            for (size_t i = 0; i < trace.size(); ++i)
                replay += std::to_string(i) + ": " + trace[i] + "\n";
            FAIL(violation->message << " (" << violation->window << ")\n" << replay);
        }
    }
}

TEST_CASE("Live ownership uses the same persistent graph validation as restart", "[invariants][scratchpad]")
{
    auto state = test::state();
    test::configure(state, [](Config& config) { config.scratchpads = { { .name = "named" } }; });
    add(state, 1);
    state.insert_fixture(2, Fixture::Role::Dock);
    REQUIRE_FALSE(invariants::validate(state));
    SECTION("Pool cannot own a fixture") { state.pool_scratchpad(2); }
    SECTION("Named claim refuses an unmanaged ID; malformed snapshots still reject it")
    {
        state.claim_scratchpad(999, ScratchpadConfig{ .name = "named" });
        CHECK_FALSE(invariants::validate(state));
        auto snapshot = state.snapshot();
        snapshot.named_scratchpads.push_back({ "named", 999 });
        CHECK(invariants::validate(snapshot));
        CHECK_FALSE(restart::decode(restart::encode(snapshot)));
        return;
    }
    CHECK(invariants::validate(state));
    CHECK(invariants::validate(state.snapshot()));
    CHECK_FALSE(restart::decode(restart::encode(state.snapshot())));
}
