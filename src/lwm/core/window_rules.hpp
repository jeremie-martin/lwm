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

/**
 * @brief Compiled window rule for efficient matching
 *
 * Regex patterns are pre-compiled at load time to avoid repeated
 * compilation during window mapping.
 */
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
    std::optional<bool> above;
    std::optional<bool> below;
    std::optional<bool> sticky;
    std::optional<bool> skip_taskbar;
    std::optional<bool> skip_pager;
    std::optional<RuleGeometry> geometry;
    std::optional<bool> center;
};

/**
 * @brief Window information used for rule matching
 *
 * Collected from WM_CLASS, _NET_WM_NAME/WM_NAME, and EWMH type.
 */
struct WindowMatchInfo
{
    std::string wm_class;      // WM_CLASS class name
    std::string wm_class_name; // WM_CLASS instance name
    std::string title;         // _NET_WM_NAME or WM_NAME
    WindowType ewmh_type = WindowType::Normal;
    bool is_transient = false;
};

/**
 * @brief Result of rule matching
 *
 * Contains all actions to apply if a rule matched.
 */
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
    std::optional<bool> above;
    std::optional<bool> below;
    std::optional<bool> sticky;
    std::optional<bool> skip_taskbar;
    std::optional<bool> skip_pager;

    // Floating geometry
    std::optional<Geometry> geometry;
    bool center = false;
};

/**
 * @brief Window rules engine for automatic window configuration
 *
 * Rules are evaluated in order - first match wins.
 * All criteria in a rule use AND logic (all specified must match).
 */
class WindowRules
{
public:
    /**
     * @brief Load and compile rules from configuration
     *
     * Invalid regex patterns fall back to literal string matching.
     *
     * @param configs Raw rule configurations from TOML
     */
    void load_rules(std::vector<WindowRuleConfig> const& configs);

    /**
     * @brief Match a window against all rules
     *
     * @param info Window properties to match against
     * @param monitors Current monitor list (for name resolution)
     * @param workspace_names Workspace names (for name resolution)
     * @return Match result with actions to apply
     */
    WindowRuleResult match(
        WindowMatchInfo const& info,
        std::span<Monitor const> monitors,
        std::span<std::string const> workspace_names
    ) const;

    /**
     * @brief Get number of loaded rules
     */
    size_t rule_count() const { return rules_.size(); }

private:
    std::vector<CompiledWindowRule> rules_;

    /**
     * @brief Compile a regex pattern, falling back to literal on failure
     */
    static std::optional<std::regex> compile_pattern(std::optional<std::string> const& pattern);

    /**
     * @brief Convert type string to WindowType enum
     */
    static std::optional<WindowType> parse_window_type(std::optional<std::string> const& type_str);

    /**
     * @brief Check if a single rule matches
     */
    bool matches_rule(CompiledWindowRule const& rule, WindowMatchInfo const& info) const;

    /**
     * @brief Resolve monitor name to index
     */
    static std::optional<size_t> resolve_monitor(
        std::optional<int> index,
        std::optional<std::string> const& name,
        std::span<Monitor const> monitors
    );

    /**
     * @brief Resolve workspace name to index
     */
    static std::optional<size_t> resolve_workspace(
        std::optional<int> index,
        std::optional<std::string> const& name,
        std::span<std::string const> workspace_names
    );
};

} // namespace lwm
