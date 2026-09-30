#pragma once

#include "lwm/core/command.hpp"
#include "lwm/core/types.hpp"
#include <cstddef>
#include <cstdint>
#include <expected>
#include <map>
#include <optional>
#include <regex>
#include <string>
#include <vector>

namespace lwm {

struct RuleGeometry
{
    std::optional<int16_t> x;
    std::optional<int16_t> y;
    std::optional<uint16_t> width;
    std::optional<uint16_t> height;
};

// Configuration owns the compiled patterns used by rules and scratchpads.
struct WindowMatcher
{
    std::optional<std::regex> class_regex;
    std::optional<std::regex> instance_regex;
    std::optional<std::regex> title_regex;

    bool empty() const { return !class_regex && !instance_regex && !title_regex; }
    bool matches(std::string const& wm_class, std::string const& instance, std::string const& title) const
    {
        return (!class_regex || std::regex_match(wm_class, *class_regex))
            && (!instance_regex || std::regex_match(instance, *instance_regex))
            && (!title_regex || std::regex_match(title, *title_regex));
    }
};

struct WindowRuleConfig
{
    // Matching criteria (all optional, AND logic - all specified must match)
    WindowMatcher match;
    std::optional<WindowType> type;
    std::optional<bool> transient;               // Require transient (true) or non-transient (false)

    // Actions
    std::optional<bool> floating;              // Force floating (true) or tiled (false)
    std::optional<int> workspace;              // Target workspace (index)
    std::optional<std::string> workspace_name; // Target workspace (by name)
    std::optional<int> monitor;                // Target monitor (index)
    std::optional<std::string> monitor_name;   // Target monitor (by name like "HDMI-1")
    std::optional<bool> fullscreen;            // Fullscreen state override
    std::optional<bool> above;                 // Above-layer preference
    std::optional<bool> below;                 // Below-layer preference
    std::optional<bool> sticky;                // Visible on all workspaces of the owning monitor
    std::optional<bool> skip_taskbar;          // Exclude from taskbar
    std::optional<bool> skip_pager;            // Exclude from pager
    std::optional<bool> borderless;            // Zero border for tiled or floating windows
    std::optional<RuleGeometry> geometry;      // Floating geometry
    std::optional<bool> center;
    std::optional<std::string> scratchpad; ///< Assign to named scratchpad
};

enum class MouseAction
{
    DragWindow,
    ResizeFloating,
    ToggleFloat
};
struct MousebindConfig
{
    uint16_t modifier = 0;
    uint8_t button = 0;
    MouseAction action = MouseAction::DragWindow;
};

struct AppearanceConfig
{
    uint32_t padding = 10;
    uint32_t border_width = 2;
    uint32_t border_color = 0xFF0000;
    uint32_t urgent_border_color = 0xFFA500;
};

struct FocusConfig
{
    bool warp_cursor_on_monitor_change = false;
};

struct WorkspacesConfig
{
    size_t count = 10;
    std::vector<std::string> names;

    /// Display name for a per-monitor workspace index: configured name, or the
    /// 1-based index as fallback. Shared by _NET_DESKTOP_NAMES and IPC JSON.
    std::string display_name(size_t index) const
    {
        return index < names.size() ? names[index] : std::to_string(index + 1);
    }
};

struct LayoutConfig
{
    std::string strategy = "master-stack";
    double default_ratio = 0.5;
    double min_ratio = 0.1;
    uint32_t resize_grab_threshold = 8; // pixels from split border to trigger resize
};

struct AutostartConfig
{
    std::vector<CommandConfig> commands;
};

struct ScratchpadConfig
{
    std::string name;
    CommandConfig spawn;
    WindowMatcher match;
    double width = 0.8;   ///< Fraction of monitor working area
    double height = 0.7;
};

struct Config
{
    AppearanceConfig appearance;
    LayoutConfig layout;
    FocusConfig focus;
    std::map<std::string, CommandConfig> commands;
    WorkspacesConfig workspaces;
    AutostartConfig autostart;
    std::map<KeyBinding, Action> keybinds;
    std::vector<MousebindConfig> mousebinds;
    std::vector<WindowRuleConfig> rules;
    std::vector<ScratchpadConfig> scratchpads;
};

using ConfigLoadResult = std::expected<Config, std::string>;

ConfigLoadResult load_config_result(std::string const& path);
Config default_config();

} // namespace lwm
