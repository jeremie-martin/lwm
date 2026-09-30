#include "lwm/core/focus.hpp"
#include "lwm/core/invariants.hpp"
#include "state_fixture.hpp"
#include <catch2/catch_test_macros.hpp>
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
    state.iconic(1, true);
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
    SECTION("Unmanaged") { state.focus(99); }
    SECTION("Iconic") { state.iconic(1, true); }
    SECTION("Hidden workspace") { state.switch_workspace(0, 1); }
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
// consistent. Only focus needs the completion step's repair, emulated here.
TEST_CASE("Generated State operation sequences preserve model invariants", "[invariants][sequence]")
{
    std::mt19937 random(12345);
    auto pick = [&](size_t bound) { return bound ? std::uniform_int_distribution<size_t>(0, bound - 1)(random) : 0; };
    auto state = test::state(2);
    state.configure_scratchpads(std::vector<std::string>{ "a", "b" });
    std::vector<xcb_window_t> windows;
    xcb_window_t next = 1;
    std::vector<std::string> trace;
    for (int step = 0; step < 4000; ++step)
    {
        auto const& monitors = state.monitors();
        size_t monitor = pick(monitors.size());
        size_t workspace = pick(monitors[monitor].workspaces.size());
        xcb_window_t window = windows.empty() ? XCB_NONE : windows[pick(windows.size())];
        switch (pick(windows.empty() ? 1 : 18))
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
                state.iconic(window, pick(2) == 0);
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
                    state.claim_scratchpad(name, window);
                trace.push_back("claim");
                break;
            case 12:
                state.configure_scratchpads(pick(2) ? std::vector<std::string>{ "a" } : std::vector<std::string>{ "a", "b" });
                trace.push_back("configure");
                break;
            case 13:
            {
                std::vector<Monitor> outputs;
                for (size_t i = 0, count = 1 + pick(3); i < count; ++i)
                    outputs.push_back(test::monitor("M" + std::to_string(pick(3)), static_cast<int16_t>(i * 1000)));
                std::ranges::sort(outputs, { }, &Monitor::name);
                auto [first, last] = std::ranges::unique(outputs, { }, &Monitor::name);
                outputs.erase(first, last);
                state.replace_monitors(std::move(outputs));
                trace.push_back("topology");
                break;
            }
            case 14:
                state.show_desktop(pick(2) == 0);
                trace.push_back("desktop");
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
                state.remember_focus(window);
                trace.push_back("remember");
                break;
        }
        // Completion repairs focus that no longer holds.
        if (auto const* active = state.find(state.active_window()); active && !state.focusable(*active))
            state.focus(focus::fallback(state, state.focused_monitor()));
        else if (state.active_window() != XCB_NONE && !active)
            state.focus(XCB_NONE);
        if (auto violation = invariants::validate(state))
        {
            std::string recent;
            for (size_t i = trace.size() > 12 ? trace.size() - 12 : 0; i < trace.size(); ++i) recent += trace[i] + " ";
            FAIL("step " << step << ": " << violation->message << " (" << violation->window << ") after " << recent);
        }
    }
}
