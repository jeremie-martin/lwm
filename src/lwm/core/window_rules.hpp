#pragma once

#include "lwm/config/config.hpp"
#include "lwm/core/ewmh.hpp"
#include "lwm/core/types.hpp"
#include <optional>
#include <regex>
#include <span>
#include <string>
#include <vector>

namespace lwm {

// Compile once at configuration load; matching does not recompile regexes.
struct CompiledWindowRule
{
    // Pre-compiled regex patterns (nullopt if not specified)
    std::optional<std::regex> class_regex;
    std::optional<std::regex> instance_regex;
    std::optional<std::regex> title_regex;

    // Non-regex matching criteria
    std::optional<WindowType> type;
    std::optional<bool> transient;

    // Actions (copied from WindowRuleConfig)
    std::optional<bool> floating;
    std::optional<int> workspace;
    std::optional<std::string> workspace_name;
    std::optional<int> monitor;
    std::optional<std::string> monitor_name;
    std::optional<bool> fullscreen;
    std::optional<LayerHint> layer_hint;
    std::optional<bool> sticky;
    std::optional<bool> skip_taskbar;
    std::optional<bool> skip_pager;
    std::optional<bool> borderless;
    std::optional<RuleGeometry> geometry;
    std::optional<bool> center;
    std::optional<std::string> scratchpad; ///< Assign to named scratchpad

    // Set when any matching criterion had an invalid value (empty pattern, unknown type).
    // Rules with this flag never match any window.
    bool never_matches = false;
};

// X properties collected by the caller for pure rule matching.
struct WindowMatchInfo
{
    std::string wm_class;      // WM_CLASS class name
    std::string wm_class_name; // WM_CLASS instance name
    std::string title;         // _NET_WM_NAME or WM_NAME
    WindowType ewmh_type = WindowType::Normal;
    bool is_transient = false;
};

inline WindowMatchInfo window_match_info(Client const& client)
{
    return { client.wm_class, client.wm_class_name, client.name, client.ewmh_type, client.transient_for != XCB_NONE };
}

// Optional actions preserve the distinction between unspecified and false.
struct WindowRuleResult
{
    bool matched = false;

    // Classification override
    std::optional<bool> floating;

    // Target location (resolved to indices)
    std::optional<size_t> target_monitor;
    std::optional<size_t> target_workspace;

    // State flags
    std::optional<bool> fullscreen;
    std::optional<LayerHint> layer_hint;
    std::optional<bool> sticky;
    std::optional<bool> skip_taskbar;
    std::optional<bool> skip_pager;
    std::optional<bool> borderless;

    // Floating geometry
    std::optional<Geometry> geometry;
    bool center = false;

    // Scratchpad assignment
    std::optional<std::string> scratchpad;

    bool operator==(WindowRuleResult const&) const = default;
};

// First matching rule wins; every specified criterion must match.
class WindowRules
{
public:
    // TOML parsing rejects invalid regexes. Programmatically supplied patterns
    // fall back to literal matching here if compilation fails.
    void load_rules(std::vector<WindowRuleConfig> const& configs);

    // Resolve named placement against the supplied monitor/workspace lists.
    WindowRuleResult match(
        WindowMatchInfo const& info,
        std::span<Monitor const> monitors,
        std::span<std::string const> workspace_names
    ) const;

    size_t rule_count() const { return rules_.size(); }

    // Invalid regex syntax falls back to literal matching.
    static std::optional<std::regex> compile_pattern(std::optional<std::string> const& pattern);

private:
    std::vector<CompiledWindowRule> rules_;

    static std::optional<WindowType> parse_window_type(std::optional<std::string> const& type_str);

    bool matches_rule(CompiledWindowRule const& rule, WindowMatchInfo const& info) const;

    static std::optional<size_t> resolve_monitor(
        std::optional<int> index,
        std::optional<std::string> const& name,
        std::span<Monitor const> monitors
    );

    static std::optional<size_t> resolve_workspace(
        std::optional<int> index,
        std::optional<std::string> const& name,
        std::span<std::string const> workspace_names
    );
};

} // namespace lwm
