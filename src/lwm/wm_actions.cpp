#include "lwm/core/log.hpp"
#include "lwm/core/policy.hpp"
#include "wm.hpp"
#include <algorithm>

namespace lwm {

namespace {

template <class... Ts> struct Overloaded : Ts...
{
    using Ts::operator()...;
};

using Result = std::expected<std::string, std::string>;

std::string json_number(double value) { return std::to_string(value); }

} // namespace

// The one executor for key bindings and IPC. Replies are the IPC result text;
// key bindings ignore them.
Result WindowManager::execute(Action const& action, std::string_view source)
{
    using namespace lwm::action;
    xcb_window_t active = state_.active_window();
    size_t monitor = state_.focused_monitor();
    auto const& focused = state_.monitors()[monitor];
    auto require_active = [&]() -> std::expected<void, std::string>
    {
        if (!state_.find(active))
            return std::unexpected("no active window");
        return { };
    };
    return std::visit(
        Overloaded{
            [&](Kill const&) -> Result
            {
                if (auto ok = require_active(); !ok)
                    return std::unexpected(ok.error());
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
            [&](Restart const&) -> Result
            {
                LWM_LOG_INFO("Restart requested ({})", source);
                restarting_ = true;
                restart_binary_.clear();
                running_ = false;
                return "restarting";
            },
            [&](Exec const& exec) -> Result
            {
                LWM_LOG_INFO("Restart with {} requested ({})", exec.binary, source);
                restarting_ = true;
                restart_binary_ = exec.binary;
                running_ = false;
                return "restarting";
            },
            [&](Spawn const& spawn) -> Result
            {
                if (!launch_program(spawn.command, source))
                    return std::unexpected("launch failed");
                return "";
            },
            [&](ToggleFullscreen const&) -> Result
            {
                if (auto ok = require_active(); !ok)
                    return std::unexpected(ok.error());
                set_fullscreen(active, !state_.require(active).fullscreen);
                return "";
            },
            [&](ToggleFloat const&) -> Result
            {
                if (auto ok = require_active(); !ok)
                    return std::unexpected(ok.error());
                toggle_float(active);
                return "";
            },
            [&](FocusNext const&) -> Result
            {
                if (!cycle_focus(true))
                    return std::unexpected("no focus candidates");
                return std::to_string(state_.active_window());
            },
            [&](FocusPrev const&) -> Result
            {
                if (!cycle_focus(false))
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
                focus_window(focus.window);
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
                if (auto ok = require_active(); !ok)
                    return std::unexpected(ok.error());
                move_active_to_monitor(move.direction);
                return "";
            },
            [&](SwitchWorkspace const& target) -> Result
            {
                if (target.workspace >= focused.workspaces.size())
                    return std::unexpected("workspace out of range");
                switch_workspace(target.workspace);
                return std::to_string(target.workspace);
            },
            [&](ToggleWorkspace const&) -> Result
            {
                if (focused.previous_workspace != focused.current_workspace)
                    switch_workspace(focused.previous_workspace);
                return std::to_string(state_.monitors()[monitor].current_workspace);
            },
            [&](NextWorkspace const&) -> Result
            {
                size_t target = (focused.current_workspace + 1) % focused.workspaces.size();
                switch_workspace(target);
                return std::to_string(target);
            },
            [&](PrevWorkspace const&) -> Result
            {
                size_t count = focused.workspaces.size();
                size_t target = (focused.current_workspace + count - 1) % count;
                switch_workspace(target);
                return std::to_string(target);
            },
            [&](MoveToWorkspace const& target) -> Result
            {
                if (target.workspace >= focused.workspaces.size())
                    return std::unexpected("workspace out of range");
                if (auto ok = require_active(); !ok)
                    return std::unexpected(ok.error());
                move_active_to_workspace(target.workspace);
                return "";
            },
            [&](SwapNext const&) -> Result
            {
                swap_active_tile(1);
                layout_changed(action);
                return "";
            },
            [&](SwapPrev const&) -> Result
            {
                swap_active_tile(-1);
                layout_changed(action);
                return "";
            },
            [&](SetLayout const& layout) -> Result
            {
                std::string name = layout_strategy_str(layout.strategy);
                state_.layout(monitor, layout.strategy);
                drain_requested_ = true;
                layout_changed(action, "\"" + name + "\"");
                return "layout set to " + name;
            },
            [&](SetRatio const& ratio) -> Result
            {
                double min = config_.layout.min_ratio;
                if (ratio.value < min || ratio.value > 1.0 - min)
                    return std::unexpected(
                        "ratio out of range [" + std::to_string(min) + ", " + std::to_string(1.0 - min) + "]"
                    );
                state_.ratio(monitor, SplitAddress{ 0 }, ratio.value);
                layout_changed(action, json_number(ratio.value));
                return "ratio set";
            },
            [&](AdjustRatio const& adjust) -> Result
            {
                double min = config_.layout.min_ratio;
                auto const& ratios = focused.current().split_ratios;
                auto it = ratios.find(SplitAddress{ 0 });
                double current = it == ratios.end() ? config_.layout.default_ratio : it->second;
                double adjusted = std::clamp(current + adjust.delta, min, 1.0 - min);
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
                if (auto ok = require_active(); !ok)
                    return std::unexpected(ok.error());
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

void WindowManager::layout_changed(Action const& action, std::optional<std::string> value, std::optional<double> delta)
{
    queue_event(event::LayoutChange{ action_name(action), std::move(value), delta });
}

// ---------------------------------------------------------------------------
// Window state
// ---------------------------------------------------------------------------

void WindowManager::set_fullscreen(xcb_window_t window, bool enabled) { state_.fullscreen(window, enabled); }

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
    focus_window(window);
}

void WindowManager::iconify_window(xcb_window_t window)
{
    auto const* client = state_.find(window);
    if (!client || client->iconic)
        return;
    bool was_in_view = state_.in_view(*client);
    state_.iconic(window, true);
    if (window != state_.active_window())
        return;
    if (client->monitor == state_.focused_monitor() && was_in_view)
        focus_fallback(client->monitor);
    else
        clear_focus();
}

void WindowManager::deiconify_window(xcb_window_t window, bool focus)
{
    auto const* client = state_.find(window);
    if (!client)
        return;
    state_.iconic(window, false);
    if ((focus || client->fullscreen) && client->monitor == state_.focused_monitor() && state_.in_view(*client))
        focus_window(window);
}

// ---------------------------------------------------------------------------
// Workspaces and monitors
// ---------------------------------------------------------------------------

void WindowManager::switch_workspace(size_t workspace)
{
    size_t monitor = state_.focused_monitor();
    if (state_.switch_workspace(monitor, workspace))
        focus_fallback(monitor);
}

// _NET_CURRENT_DESKTOP names a monitor and one of its workspaces.
void WindowManager::switch_to_desktop(uint32_t desktop)
{
    LWM_LOG_DEBUG("_NET_CURRENT_DESKTOP request: desktop={}", desktop);
    auto indices = ewmh_policy::desktop_to_indices(desktop, config_.workspaces.count);
    if (!indices || indices->first >= state_.monitors().size())
        return;
    auto [monitor, workspace] = *indices;
    if (monitor == state_.focused_monitor() && workspace == state_.monitors()[monitor].current_workspace)
        return;
    state_.focus_monitor(monitor);
    state_.switch_workspace(monitor, workspace);
    focus_fallback(monitor);
}

void WindowManager::move_active_to_workspace(size_t workspace)
{
    xcb_window_t window = state_.active_window();
    size_t monitor = state_.require(window).monitor;
    if (workspace == state_.monitors()[monitor].current_workspace || !state_.relocate(window, monitor, workspace))
        return;
    state_.remember_focus(window);
    focus_fallback(monitor);
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
        static_cast<int16_t>(monitor.x + monitor.width / 2),
        static_cast<int16_t>(monitor.y + monitor.height / 2)
    );
}

void WindowManager::focus_adjacent_monitor(int direction)
{
    if (state_.monitors().size() <= 1)
        return;
    size_t target = wrap_monitor(static_cast<int>(state_.focused_monitor()) + direction);
    state_.focus_monitor(target);
    focus_fallback(target);
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
    state_.remember_focus(window);
    state_.focus_monitor(target);
    focus_window(window);
    warp_to_monitor(state_.monitors()[target]);
}

void WindowManager::swap_active_tile(int offset)
{
    size_t monitor = state_.focused_monitor();
    auto const& workspace = state_.monitors()[monitor].current();
    auto it = workspace.find_window(workspace.focused_window);
    size_t count = workspace.windows.size();
    if (it == workspace.windows.end() || count < 2)
        return;
    size_t index = static_cast<size_t>(it - workspace.windows.begin());
    size_t other = static_cast<size_t>((static_cast<int>(index) + offset % static_cast<int>(count) + static_cast<int>(count))
                                       % static_cast<int>(count));
    // Every monocle slot shares one rectangle, so swapping would change nothing
    // visible: focus the adjacent tile instead.
    if (workspace.layout_strategy == LayoutStrategy::Monocle)
        focus_window(workspace.windows[other]);
    else
        state_.swap_tiles(monitor, index, other);
}

} // namespace lwm
