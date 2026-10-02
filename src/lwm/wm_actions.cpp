#include "lwm/core/focus.hpp"
#include "lwm/core/overloaded.hpp"
#include "lwm/core/log.hpp"
#include "lwm/core/policy.hpp"
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
            [&](Kill const&) -> Result
            {
                if (!has_active)
                    return no_active;
                kill_window(active);
                return "";
            },
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
            [&](ToggleFullscreen const&) -> Result
            {
                if (!has_active)
                    return no_active;
                state_.fullscreen(active, !state_.require(active).fullscreen);
                return "";
            },
            [&](ToggleFloat const&) -> Result
            {
                if (!has_active)
                    return no_active;
                toggle_float(active);
                return "";
            },
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
                focus_adjacent_monitor(focus.direction);
                return "";
            },
            [&](MoveToMonitor const& move) -> Result
            {
                if (!has_active)
                    return no_active;
                move_active_to_monitor(move.direction);
                return "";
            },
            [&](SwitchWorkspace const& target) -> Result
            {
                if (target.workspace >= focused.workspaces.size())
                    return std::unexpected("workspace out of range");
                state_.switch_workspace(monitor, target.workspace);
                return std::to_string(target.workspace);
            },
            [&](ToggleWorkspace const&) -> Result
            {
                if (focused.previous_workspace != focused.current_workspace)
                    state_.switch_workspace(monitor, focused.previous_workspace);
                return std::to_string(state_.monitors()[monitor].current_workspace);
            },
            [&](CycleWorkspace const& cycle) -> Result
            {
                auto count = static_cast<int>(focused.workspaces.size());
                auto target = static_cast<size_t>(
                    ((static_cast<int>(focused.current_workspace) + cycle.step) % count + count) % count
                );
                state_.switch_workspace(monitor, target);
                return std::to_string(target);
            },
            [&](MoveToWorkspace const& target) -> Result
            {
                if (target.workspace >= focused.workspaces.size())
                    return std::unexpected("workspace out of range");
                if (!has_active)
                    return no_active;
                state_.relocate(active, state_.require(active).monitor, target.workspace);
                return "";
            },
            [&](SwapTile const& swap) -> Result
            {
                swap_active_tile(swap.offset);
                layout_changed(action);
                return "";
            },
            [&](SetLayout const& layout) -> Result
            {
                std::string name = layout_strategy_str(layout.strategy);
                state_.layout(monitor, layout.strategy);
                drain_requested_ = true;
                layout_changed(action, name);
                return "layout set to " + name;
            },
            [&](SetRatio const& ratio) -> Result
            {
                double min = config_.layout.min_ratio;
                if (!config_.layout.accepts_ratio(ratio.value))
                    return std::unexpected(
                        "ratio out of range [" + std::to_string(min) + ", " + std::to_string(1.0 - min) + "]"
                    );
                state_.ratio(monitor, SplitAddress{ 0 }, ratio.value);
                layout_changed(action, ratio.value);
                return "ratio set";
            },
            [&](AdjustRatio const& adjust) -> Result
            {
                auto const& ratios = focused.current().split_ratios;
                auto it = ratios.find(SplitAddress{ 0 });
                double current = it == ratios.end() ? config_.layout.default_ratio : it->second;
                double adjusted = config_.layout.clamp_ratio(current + adjust.delta);
                if (adjusted == current)
                    return "ratio unchanged";
                state_.ratio(monitor, SplitAddress{ 0 }, adjusted);
                layout_changed(action, std::nullopt, adjust.delta);
                return "ratio adjusted";
            },
            [&](ResetRatios const&) -> Result
            {
                state_.reset_ratios(monitor);
                layout_changed(action);
                return "ratios reset";
            },
            [&](ScratchpadStash const&) -> Result
            {
                if (!has_active)
                    return no_active;
                stash_window(active);
                return "";
            },
            [&](ScratchpadCycle const&) -> Result
            {
                cycle_scratchpad_pool();
                return "";
            },
            [&](ScratchpadToggle const& toggle) -> Result
            {
                if (auto result = toggle_scratchpad(toggle.name); !result)
                    return std::unexpected(result.error());
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
    queue_event(event::LayoutChange{ action_name(action), std::move(value), delta });
}

// Window state

void WindowManager::toggle_float(xcb_window_t window)
{
    auto const& client = state_.require(window);
    if (client.fullscreen || client.iconic || state_.showing_desktop())
        return;
    bool floating = client.kind() == Client::Kind::Floating;
    // Leaving floating also leaves maximize, which only floating presentation honors.
    if (floating)
        state_.maximize(window, false, false);
    state_.floating(window, !floating);
    state_.focus(window);
}

// _NET_CURRENT_DESKTOP names a monitor and one of its workspaces.
void WindowManager::switch_to_desktop(uint32_t desktop)
{
    LWM_LOG_DEBUG("_NET_CURRENT_DESKTOP request: desktop={}", desktop);
    auto placement = ewmh_policy::desktop_placement(desktop, config_.workspaces.count, state_.monitors().size());
    if (!placement)
        return;
    auto [monitor, workspace] = *placement;
    if (monitor == state_.focused_monitor() && workspace == state_.monitors()[monitor].current_workspace)
        return;
    state_.focus_monitor(monitor);
    state_.switch_workspace(monitor, workspace);
    state_.focus_fallback(monitor);
}

size_t WindowManager::wrap_monitor(int index) const
{
    int size = static_cast<int>(state_.monitors().size());
    return static_cast<size_t>(((index % size) + size) % size);
}

void WindowManager::warp_to_monitor(Monitor const& monitor)
{
    if (!config_.focus.warp_cursor_on_monitor_change)
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

void WindowManager::focus_adjacent_monitor(int direction)
{
    if (state_.monitors().size() <= 1)
        return;
    size_t target = wrap_monitor(static_cast<int>(state_.focused_monitor()) + direction);
    state_.focus_monitor(target);
    state_.focus_fallback(target);
    warp_to_monitor(state_.monitors()[target]);
}

void WindowManager::move_active_to_monitor(int direction)
{
    if (state_.monitors().size() <= 1)
        return;
    xcb_window_t window = state_.active_window();
    size_t target = wrap_monitor(static_cast<int>(state_.require(window).monitor) + direction);
    size_t workspace = state_.monitors()[target].current_workspace;
    if (!state_.relocate(window, target, workspace, State::RelocationGeometry::Center))
        return;
    state_.focus(window);
    warp_to_monitor(state_.monitors()[target]);
}

void WindowManager::swap_active_tile(int offset)
{
    size_t monitor = state_.focused_monitor();
    auto const& workspace = state_.monitors()[monitor].current();
    std::vector<size_t> eligible;
    auto fullscreen = state_.fullscreen_visibility();
    for (size_t i = 0; i < workspace.windows.size(); ++i)
    {
        auto const& client = state_.require(workspace.windows[i]);
        if (!client.fullscreen && state_.visible(client, fullscreen))
            eligible.push_back(i);
    }
    auto tile = focus::tile(state_, monitor, fullscreen);
    auto it = std::ranges::find_if(eligible, [&](auto i) { return workspace.windows[i] == tile; });
    size_t count = eligible.size();
    if (it == eligible.end() || count < 2)
        return;
    size_t index = static_cast<size_t>(it - eligible.begin());
    size_t other = static_cast<size_t>((static_cast<int>(index) + offset % static_cast<int>(count) + static_cast<int>(count))
                                       % static_cast<int>(count));
    // Every monocle slot shares one rectangle, so swapping would change nothing
    // visible: focus the adjacent tile instead.
    if (workspace.layout_strategy == LayoutStrategy::Monocle)
        state_.focus(workspace.windows[eligible[other]]);
    else
        state_.swap_tiles(monitor, eligible[index], eligible[other]);
}

} // namespace lwm
