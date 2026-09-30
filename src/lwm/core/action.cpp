#include "action.hpp"

namespace lwm {

namespace {
template <class... Ts> struct Overloaded : Ts...
{
    using Ts::operator()...;
};
}

std::string_view action_name(Action const& action)
{
    using namespace action;
    return std::visit(
        Overloaded{
            [](Kill const&) -> std::string_view { return "kill"; },
            [](ReloadConfig const&) -> std::string_view { return "reload_config"; },
            [](Restart const&) -> std::string_view { return "restart"; },
            [](Exec const&) -> std::string_view { return "exec"; },
            [](Spawn const&) -> std::string_view { return "spawn"; },
            [](ToggleFullscreen const&) -> std::string_view { return "toggle_fullscreen"; },
            [](ToggleFloat const&) -> std::string_view { return "toggle_float"; },
            [](FocusNext const&) -> std::string_view { return "focus_next"; },
            [](FocusPrev const&) -> std::string_view { return "focus_prev"; },
            [](FocusWindow const&) -> std::string_view { return "focus_window"; },
            [](FocusMonitor const& a) -> std::string_view
            { return a.direction < 0 ? "focus_monitor_left" : "focus_monitor_right"; },
            [](MoveToMonitor const& a) -> std::string_view
            { return a.direction < 0 ? "move_to_monitor_left" : "move_to_monitor_right"; },
            [](SwitchWorkspace const&) -> std::string_view { return "switch_workspace"; },
            [](ToggleWorkspace const&) -> std::string_view { return "toggle_workspace"; },
            [](NextWorkspace const&) -> std::string_view { return "next_workspace"; },
            [](PrevWorkspace const&) -> std::string_view { return "prev_workspace"; },
            [](MoveToWorkspace const&) -> std::string_view { return "move_to_workspace"; },
            [](SwapNext const&) -> std::string_view { return "swap_next"; },
            [](SwapPrev const&) -> std::string_view { return "swap_prev"; },
            [](SetLayout const&) -> std::string_view { return "set_layout"; },
            [](SetRatio const&) -> std::string_view { return "set_ratio"; },
            [](AdjustRatio const&) -> std::string_view { return "adjust_ratio"; },
            [](ResetRatios const&) -> std::string_view { return "reset_ratios"; },
            [](ScratchpadStash const&) -> std::string_view { return "scratchpad_stash"; },
            [](ScratchpadCycle const&) -> std::string_view { return "scratchpad_cycle"; },
            [](ScratchpadToggle const&) -> std::string_view { return "toggle_scratchpad"; },
            [](ScratchpadCancelLaunch const&) -> std::string_view { return "cancel_scratchpad_launch"; },
            [](NotifyAttention const&) -> std::string_view { return "notify_attention"; },
        },
        action
    );
}

} // namespace lwm
