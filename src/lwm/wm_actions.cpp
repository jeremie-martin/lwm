#include "lwm/core/overloaded.hpp"
#include "lwm/core/log.hpp"
#include "wm.hpp"
#include <algorithm>

namespace lwm {

using Result = std::expected<void, std::string>;

// The one executor for key bindings and IPC. Success is silent; IPC callers
// read outcomes from the published state.
Result WindowManager::execute(Action const& action, std::string_view source)
{
    using namespace lwm::action;
    xcb_window_t active = state_.active_window();
    size_t monitor = state_.focused_monitor();
    auto const& focused = state_.monitors()[monitor];
    auto fail = [](std::string message) -> Result { return std::unexpected(std::move(message)); };
    auto on_active = [&](auto operation) -> Result
    {
        if (!state_.find(active))
            return fail("no active window");
        operation();
        return { };
    };
    auto restart = [&](std::string binary) -> Result
    {
        LWM_LOG_INFO("Restart requested: source={} binary={}", source, binary.empty() ? "current" : binary);
        restart_binary_ = std::move(binary);
        stop_ = RunResult::Restart;
        return { };
    };
    return std::visit(
        Overloaded{
            [&](Kill const&) { return on_active([&] { close_window(active); }); },
            [&](ReloadConfig const&) { return reload_config(source); },
            [&](Restart const&) { return restart({ }); },
            [&](Exec const& exec) { return restart(exec.binary); },
            [&](Spawn const& spawn) -> Result
            {
                if (!launch_program(spawn.argv, source))
                    return fail("launch failed");
                return { };
            },
            [&](ToggleFullscreen const&)
            { return on_active([&] { state_.fullscreen(active, !state_.require(active).fullscreen); }); },
            [&](ToggleFloat const&) { return on_active([&] { state_.toggle_floating(active); }); },
            [&](FocusCycle const& cycle) -> Result
            {
                if (!state_.cycle_focus(cycle.forward))
                    return fail("no focus candidates");
                return { };
            },
            [&](FocusWindow const& focus) -> Result
            {
                auto const* client = state_.find(focus.window);
                if (!client)
                    return fail("unknown window");
                if (!State::accepts_focus(*client))
                    return fail("window not focusable");
                state_.focus(focus.window);
                if (state_.active_window() != focus.window)
                    return fail("focus request refused");
                return { };
            },
            [&](FocusMonitor const& focus) -> Result
            {
                if (state_.focus_adjacent_monitor(focus.direction))
                    warp_to_monitor(state_.monitors()[state_.focused_monitor()]);
                return { };
            },
            [&](MoveToMonitor const& move)
            {
                return on_active([&] {
                    if (state_.move_to_monitor(move.direction))
                        warp_to_monitor(state_.monitors()[state_.require(active).monitor]);
                });
            },
            [&](SwitchWorkspace const& target) -> Result
            {
                if (target.workspace >= focused.workspaces.size())
                    return fail("workspace out of range");
                state_.switch_workspace(monitor, target.workspace);
                return { };
            },
            [&](ToggleWorkspace const&) -> Result
            {
                state_.toggle_workspace();
                return { };
            },
            [&](CycleWorkspace const& cycle) -> Result
            {
                state_.cycle_workspace(cycle.step);
                return { };
            },
            [&](MoveToWorkspace const& target) -> Result
            {
                if (target.workspace >= focused.workspaces.size())
                    return fail("workspace out of range");
                return on_active([&] { state_.relocate(active, state_.require(active).monitor, target.workspace); });
            },
            [&](SwapTile const& swap) -> Result
            {
                state_.swap_tile(swap.offset);
                return { };
            },
            [&](SetLayout const& layout) -> Result
            {
                state_.layout(monitor, layout.strategy);
                return { };
            },
            [&](SetRatio const& ratio) -> Result
            {
                if (!state_.set_ratio(ratio.value))
                {
                    double min = config().layout.min_ratio;
                    return fail("ratio out of range [" + std::to_string(min) + ", " + std::to_string(1.0 - min) + "]");
                }
                return { };
            },
            [&](AdjustRatio const& adjust) -> Result
            {
                state_.adjust_ratio(adjust.delta);
                return { };
            },
            [&](ResetRatios const&) -> Result
            {
                state_.reset_ratios(monitor);
                return { };
            },
            [&](ScratchpadStash const&) { return on_active([&] { state_.stash(active); }); },
            [&](ScratchpadCycle const&) -> Result
            {
                state_.cycle_scratchpad_pool();
                return { };
            },
            // A pending launch begins only after process creation succeeds.
            [&](ScratchpadToggle const& toggle) -> Result
            {
                auto launch = state_.toggle_scratchpad(toggle.name);
                if (!launch)
                    return fail(launch.error());
                if (*launch && !launch_program((*launch)->spawn, "scratchpad"))
                    return fail("launch failed");
                if (*launch)
                    state_.scratchpad_pending(toggle.name, true);
                return { };
            },
            [&](ScratchpadCancelLaunch const& cancel) -> Result
            {
                if (!state_.named_scratchpad(cancel.name))
                    return fail("unknown scratchpad: " + cancel.name);
                state_.scratchpad_pending(cancel.name, false);
                return { };
            },
            // The active window already has the user's attention.
            [&](NotifyAttention const& attention) -> Result
            {
                if (!state_.find(attention.window))
                    return fail("unknown window");
                if (attention.window != active)
                    state_.urgency(attention.window, UrgencySource::WmInitiated, true);
                return { };
            },
        },
        action
    );
}

void WindowManager::warp_to_monitor(Monitor const& monitor)
{
    if (!config().focus.warp_cursor_on_monitor_change)
        return;
    xcb_warp_pointer(
        conn_.get(),
        XCB_NONE,
        conn_.screen()->root,
        0,
        0,
        0,
        0,
        static_cast<int16_t>(center(monitor.geometry).first),
        static_cast<int16_t>(center(monitor.geometry).second)
    );
}

} // namespace lwm
