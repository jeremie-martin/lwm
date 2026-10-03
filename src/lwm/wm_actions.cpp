#include "lwm/core/overloaded.hpp"
#include "lwm/core/log.hpp"
#include "wm.hpp"
#include <algorithm>

namespace lwm {

namespace {

using Result = std::expected<std::string, std::string>;

} // namespace

// The one executor for key bindings and IPC. Replies are the IPC result text;
// key bindings ignore them.
Result WindowManager::execute(Action const& action, std::string_view source)
{
    using namespace lwm::action;
    xcb_window_t active = state_.active_window();
    size_t monitor = state_.focused_monitor();
    auto const& focused = state_.monitors()[monitor];
    bool has_active = state_.find(active) != nullptr;
    auto const no_active = std::unexpected(std::string("no active window"));
    // Operations on the active client reply with empty success text.
    auto on_active = [&](auto operation) -> Result
    {
        if (!has_active)
            return no_active;
        operation();
        return "";
    };
    auto restart = [&](std::string binary) -> Result
    {
        LWM_LOG_INFO("Restart requested: source={} binary={}", source, binary.empty() ? "current" : binary);
        restarting_ = true;
        restart_binary_ = std::move(binary);
        running_ = false;
        return "restarting";
    };
    return std::visit(
        Overloaded{
            [&](Kill const&) { return on_active([&] { kill_window(active); }); },
            [&](ReloadConfig const&) -> Result
            {
                auto result = reload_config();
                report_reload(result, source);
                if (!result)
                    return std::unexpected(result.error());
                return "reloaded";
            },
            [&](Restart const&) { return restart({ }); },
            [&](Exec const& exec) { return restart(exec.binary); },
            [&](Spawn const& spawn) -> Result
            {
                if (!launch_program(spawn.argv, source))
                    return std::unexpected("launch failed");
                return "";
            },
            [&](ToggleFullscreen const&)
            { return on_active([&] { state_.fullscreen(active, !state_.require(active).fullscreen); }); },
            [&](ToggleFloat const&) { return on_active([&] { state_.toggle_floating(active); }); },
            [&](FocusCycle const& cycle) -> Result
            {
                if (!state_.cycle_focus(cycle.forward))
                    return std::unexpected("no focus candidates");
                return std::to_string(state_.active_window());
            },
            [&](FocusWindow const& focus) -> Result
            {
                auto const* client = state_.find(focus.window);
                if (!client)
                    return std::unexpected("unknown window");
                if (!State::accepts_focus(*client))
                    return std::unexpected("window not focusable");
                state_.focus(focus.window);
                if (state_.active_window() != focus.window)
                    return std::unexpected("focus request refused");
                return std::to_string(focus.window);
            },
            [&](FocusMonitor const& focus) -> Result
            {
                if (state_.focus_adjacent_monitor(focus.direction))
                    warp_to_monitor(state_.monitors()[state_.focused_monitor()]);
                return "";
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
                    return std::unexpected("workspace out of range");
                state_.switch_workspace(monitor, target.workspace);
                return std::to_string(target.workspace);
            },
            [&](ToggleWorkspace const&) -> Result { return std::to_string(state_.toggle_workspace()); },
            [&](CycleWorkspace const& cycle) -> Result { return std::to_string(state_.cycle_workspace(cycle.step)); },
            [&](MoveToWorkspace const& target) -> Result
            {
                if (target.workspace >= focused.workspaces.size())
                    return std::unexpected("workspace out of range");
                return on_active([&] { state_.relocate(active, state_.require(active).monitor, target.workspace); });
            },
            [&](SwapTile const& swap) -> Result
            {
                state_.swap_tile(swap.offset);
                layout_changed(action);
                return "";
            },
            [&](SetLayout const& layout) -> Result
            {
                std::string name = layout_strategy_str(layout.strategy);
                state_.layout(monitor, layout.strategy);
                layout_changed(action, name);
                return "layout set to " + name;
            },
            [&](SetRatio const& ratio) -> Result
            {
                if (!state_.set_ratio(ratio.value))
                {
                    double min = config().layout.min_ratio;
                    return std::unexpected("ratio out of range [" + std::to_string(min) + ", " + std::to_string(1.0 - min) + "]");
                }
                layout_changed(action, ratio.value);
                return "ratio set";
            },
            [&](AdjustRatio const& adjust) -> Result
            {
                if (!state_.adjust_ratio(adjust.delta))
                    return "ratio unchanged";
                layout_changed(action, std::nullopt, adjust.delta);
                return "ratio adjusted";
            },
            [&](ResetRatios const&) -> Result
            {
                state_.reset_ratios(monitor);
                layout_changed(action);
                return "ratios reset";
            },
            [&](ScratchpadStash const&) { return on_active([&] { state_.stash(active); }); },
            [&](ScratchpadCycle const&) -> Result
            {
                state_.cycle_scratchpad_pool();
                return "";
            },
            // A pending launch begins only after process creation succeeds.
            [&](ScratchpadToggle const& toggle) -> Result
            {
                auto launch = state_.toggle_scratchpad(toggle.name);
                if (!launch)
                    return std::unexpected(launch.error());
                if (*launch && launch_program((*launch)->spawn, "scratchpad"))
                    state_.scratchpad_pending(toggle.name, true);
                return "";
            },
            [&](ScratchpadCancelLaunch const& cancel) -> Result
            {
                if (!state_.named_scratchpad(cancel.name))
                    return std::unexpected("unknown scratchpad: " + cancel.name);
                state_.scratchpad_pending(cancel.name, false);
                return "";
            },
            [&](NotifyAttention const& attention) -> Result
            {
                if (!state_.find(attention.window))
                    return "no-match";
                if (attention.window == active)
                    return "skipped-active";
                state_.urgency(attention.window, UrgencySource::WmInitiated, true);
                return std::to_string(attention.window);
            },
        },
        action
    );
}

void WindowManager::layout_changed(
    Action const& action,
    std::optional<event::LayoutValue> value,
    std::optional<double> delta
)
{
    queue_event(event::layout_change{ action_name(action), std::move(value), delta });
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
        static_cast<int16_t>(monitor.geometry.x + monitor.geometry.width / 2),
        static_cast<int16_t>(monitor.geometry.y + monitor.geometry.height / 2)
    );
}

} // namespace lwm
