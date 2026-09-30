#pragma once

#include "lwm/core/command.hpp"
#include "lwm/core/types.hpp"
#include <string>
#include <string_view>
#include <variant>

namespace lwm {

// Every WM operation a user can trigger. Key bindings and IPC parse into this
// one type and share one executor; names are the configuration keys and the
// `key_action` event values.
namespace action {
struct Kill
{
    bool operator==(Kill const&) const = default;
};
struct ReloadConfig
{
    bool operator==(ReloadConfig const&) const = default;
};
struct Restart
{
    bool operator==(Restart const&) const = default;
};
struct Exec
{
    std::string binary;
    bool operator==(Exec const&) const = default;
};
struct Spawn
{
    CommandConfig command;
    bool operator==(Spawn const&) const = default;
};
struct ToggleFullscreen
{
    bool operator==(ToggleFullscreen const&) const = default;
};
struct ToggleFloat
{
    bool operator==(ToggleFloat const&) const = default;
};
// Steps through recently used windows; forward is the next older one.
struct FocusCycle
{
    bool forward = true;
    bool operator==(FocusCycle const&) const = default;
};
struct FocusWindow
{
    uint32_t window = 0;
    bool operator==(FocusWindow const&) const = default;
};
struct FocusMonitor
{
    int direction = 1;
    bool operator==(FocusMonitor const&) const = default;
};
struct MoveToMonitor
{
    int direction = 1;
    bool operator==(MoveToMonitor const&) const = default;
};
struct SwitchWorkspace
{
    size_t workspace = 0;
    bool operator==(SwitchWorkspace const&) const = default;
};
struct ToggleWorkspace
{
    bool operator==(ToggleWorkspace const&) const = default;
};
// Switches to the adjacent workspace with wraparound.
struct CycleWorkspace
{
    int step = 1;
    bool operator==(CycleWorkspace const&) const = default;
};
struct MoveToWorkspace
{
    size_t workspace = 0;
    bool operator==(MoveToWorkspace const&) const = default;
};
// Swaps the active tile with the one `offset` positions away, with wraparound.
struct SwapTile
{
    int offset = 1;
    bool operator==(SwapTile const&) const = default;
};
struct SetLayout
{
    LayoutStrategy strategy = LayoutStrategy::MasterStack;
    bool operator==(SetLayout const&) const = default;
};
struct SetRatio
{
    double value = 0.5;
    bool operator==(SetRatio const&) const = default;
};
struct AdjustRatio
{
    double delta = 0;
    bool operator==(AdjustRatio const&) const = default;
};
struct ResetRatios
{
    bool operator==(ResetRatios const&) const = default;
};
struct ScratchpadStash
{
    bool operator==(ScratchpadStash const&) const = default;
};
struct ScratchpadCycle
{
    bool operator==(ScratchpadCycle const&) const = default;
};
struct ScratchpadToggle
{
    std::string name;
    bool operator==(ScratchpadToggle const&) const = default;
};
struct ScratchpadCancelLaunch
{
    std::string name;
    bool operator==(ScratchpadCancelLaunch const&) const = default;
};
struct NotifyAttention
{
    uint32_t window = 0;
    bool operator==(NotifyAttention const&) const = default;
};
} // namespace action

using Action = std::variant<
    action::Kill,
    action::ReloadConfig,
    action::Restart,
    action::Exec,
    action::Spawn,
    action::ToggleFullscreen,
    action::ToggleFloat,
    action::FocusCycle,
    action::FocusWindow,
    action::FocusMonitor,
    action::MoveToMonitor,
    action::SwitchWorkspace,
    action::ToggleWorkspace,
    action::CycleWorkspace,
    action::MoveToWorkspace,
    action::SwapTile,
    action::SetLayout,
    action::SetRatio,
    action::AdjustRatio,
    action::ResetRatios,
    action::ScratchpadStash,
    action::ScratchpadCycle,
    action::ScratchpadToggle,
    action::ScratchpadCancelLaunch,
    action::NotifyAttention>;

/// Canonical name: the configuration key, extended with a direction for monitor actions.
std::string_view action_name(Action const& action);

} // namespace lwm
