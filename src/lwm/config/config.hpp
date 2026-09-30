#pragma once

#include "lwm/core/action.hpp"
#include "lwm/core/types.hpp"
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <map>
#include <optional>
#include <regex>
#include <string>
#include <vector>

namespace lwm {

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
    std::optional<bool> transient; // Require transient (true) or non-transient (false)
    RuleActions actions;
};

// Num Lock and Caps Lock never affect bindings: matching strips them, and
// grabs cover each combination of them.
constexpr uint16_t kIgnoredModifiers = XCB_MOD_MASK_2 | XCB_MOD_MASK_LOCK;
constexpr uint16_t kIgnoredModifierCombinations[] = { 0, XCB_MOD_MASK_2, XCB_MOD_MASK_LOCK, kIgnoredModifiers };
constexpr uint16_t binding_modifiers(uint16_t state) { return state & ~kIgnoredModifiers; }

struct KeyBinding
{
    uint16_t modifier;
    xcb_keysym_t keysym;

    auto operator<=>(KeyBinding const&) const = default;
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
    std::vector<std::string> names; ///< Exactly `count` display labels
};

struct LayoutConfig
{
    LayoutStrategy strategy = LayoutStrategy::MasterStack;
    double default_ratio = 0.5;
    double min_ratio = 0.1;
    // Split ratios stay within [min_ratio, 1 - min_ratio].
    bool accepts_ratio(double ratio) const { return ratio >= min_ratio && ratio <= 1.0 - min_ratio; }
    double clamp_ratio(double ratio) const { return std::clamp(ratio, min_ratio, 1.0 - min_ratio); }
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
