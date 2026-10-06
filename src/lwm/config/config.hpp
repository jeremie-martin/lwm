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
    // All specified criteria must match.
    WindowMatcher match;
    std::optional<WindowType> type;
    std::optional<bool> transient;
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

// Mouse bindings grip the window under the pointer, or run an action after
// focusing that window.
enum class MouseGrip
{
    Move,
    Resize
};
struct MousebindConfig
{
    uint16_t modifier = 0;
    uint8_t button = 0;
    std::variant<MouseGrip, Action> action = MouseGrip::Move;
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

struct ScratchpadConfig
{
    std::string name;
    std::vector<std::string> spawn;
    WindowMatcher match;
    double width = 0.8;   ///< Fraction of monitor working area
    double height = 0.7;
};

struct Config
{
    AppearanceConfig appearance;
    LayoutConfig layout;
    FocusConfig focus;
    std::vector<std::string> workspaces = { "1", "2", "3", "4", "5", "6", "7", "8", "9", "10" }; ///< Names; the count
    std::map<KeyBinding, Action> keybinds;
    std::vector<MousebindConfig> mousebinds;
    std::vector<WindowRuleConfig> rules;
    std::vector<ScratchpadConfig> scratchpads;
};

using ConfigLoadResult = std::expected<Config, std::string>;

// Startup accepts an absent implicit file as defaults; a required path must name a file.
ConfigLoadResult load_config(std::string const& path, bool required);
Config default_config();

} // namespace lwm
