#include "window_rules.hpp"
#include <algorithm>
#include <cstring>
#include <iostream>

namespace lwm {

namespace {

// Escape special regex characters for literal matching
std::string escape_regex(std::string const& str)
{
    static char const* const metacharacters = R"(\.^$|()[]{}*+?)";
    std::string result;
    result.reserve(str.size() * 2);
    for (char c : str)
    {
        if (std::strchr(metacharacters, c) != nullptr)
        {
            result += '\\';
        }
        result += c;
    }
    return result;
}

}
std::optional<std::regex> WindowRules::compile_pattern(std::optional<std::string> const& pattern)
{
    if (!pattern.has_value() || pattern->empty())
    {
        return std::nullopt;
    }

    try
    {
        return std::regex(*pattern, std::regex::ECMAScript | std::regex::optimize);
    }
    catch (std::regex_error const& e)
    {
        // Fall back to literal string matching by escaping the pattern
        std::cerr << "Warning: Invalid regex pattern '" << *pattern << "', using literal match: " << e.what()
                  << std::endl;
        try
        {
            return std::regex(escape_regex(*pattern), std::regex::ECMAScript | std::regex::optimize);
        }
        catch (...)
        {
            return std::nullopt;
        }
    }
}

std::optional<WindowType> WindowRules::parse_window_type(std::optional<std::string> const& type_str)
{
    if (!type_str.has_value())
    {
        return std::nullopt;
    }

    std::string type = *type_str;
    // Convert to lowercase for case-insensitive matching
    std::ranges::transform(type, type.begin(), [](unsigned char c) { return std::tolower(c); });

    if (type == "desktop")
        return WindowType::Desktop;
    if (type == "dock")
        return WindowType::Dock;
    if (type == "toolbar")
        return WindowType::Toolbar;
    if (type == "menu")
        return WindowType::Menu;
    if (type == "utility")
        return WindowType::Utility;
    if (type == "splash")
        return WindowType::Splash;
    if (type == "dialog")
        return WindowType::Dialog;
    if (type == "dropdown_menu" || type == "dropdownmenu")
        return WindowType::DropdownMenu;
    if (type == "popup_menu" || type == "popupmenu")
        return WindowType::PopupMenu;
    if (type == "tooltip")
        return WindowType::Tooltip;
    if (type == "notification")
        return WindowType::Notification;
    if (type == "combo")
        return WindowType::Combo;
    if (type == "dnd")
        return WindowType::Dnd;
    if (type == "normal")
        return WindowType::Normal;

    return std::nullopt;
}

void WindowRules::load_rules(std::vector<WindowRuleConfig> const& configs)
{
    rules_.clear();
    rules_.reserve(configs.size());

    for (auto const& cfg : configs)
    {
        CompiledWindowRule rule;

        rule.class_regex = compile_pattern(cfg.class_pattern);
        rule.instance_regex = compile_pattern(cfg.instance_pattern);
        rule.title_regex = compile_pattern(cfg.title_pattern);

        rule.type = parse_window_type(cfg.type);
        rule.transient = cfg.transient;

        rule.floating = cfg.floating;
        rule.workspace = cfg.workspace;
        rule.workspace_name = cfg.workspace_name;
        rule.monitor = cfg.monitor;
        rule.monitor_name = cfg.monitor_name;
        rule.fullscreen = cfg.fullscreen;
        rule.above = cfg.above;
        rule.below = cfg.below;
        rule.sticky = cfg.sticky;
        rule.skip_taskbar = cfg.skip_taskbar;
        rule.skip_pager = cfg.skip_pager;
        rule.geometry = cfg.geometry;
        rule.center = cfg.center;

        rules_.push_back(std::move(rule));
    }
}

bool WindowRules::matches_rule(CompiledWindowRule const& rule, WindowMatchInfo const& info) const
{
    if (rule.class_regex.has_value())
    {
        if (!std::regex_search(info.wm_class, *rule.class_regex))
        {
            return false;
        }
    }

    if (rule.instance_regex.has_value())
    {
        if (!std::regex_search(info.wm_class_name, *rule.instance_regex))
        {
            return false;
        }
    }

    if (rule.title_regex.has_value())
    {
        if (!std::regex_search(info.title, *rule.title_regex))
        {
            return false;
        }
    }

    if (rule.type.has_value())
    {
        if (info.ewmh_type != *rule.type)
        {
            return false;
        }
    }

    if (rule.transient.has_value())
    {
        if (info.is_transient != *rule.transient)
        {
            return false;
        }
    }

    return true;
}

std::optional<size_t> WindowRules::resolve_monitor(
    std::optional<int> index,
    std::optional<std::string> const& name,
    std::span<Monitor const> monitors
)
{
    if (index.has_value())
    {
        if (*index >= 0 && static_cast<size_t>(*index) < monitors.size())
        {
            return static_cast<size_t>(*index);
        }
        return std::nullopt;
    }

    // Try name resolution
    if (name.has_value())
    {
        for (size_t i = 0; i < monitors.size(); ++i)
        {
            if (monitors[i].name == *name)
            {
                return i;
            }
        }
    }

    return std::nullopt;
}

std::optional<size_t> WindowRules::resolve_workspace(
    std::optional<int> index,
    std::optional<std::string> const& name,
    std::span<std::string const> workspace_names
)
{
    if (index.has_value())
    {
        if (*index >= 0 && static_cast<size_t>(*index) < workspace_names.size())
        {
            return static_cast<size_t>(*index);
        }
        return std::nullopt;
    }

    // Try name resolution
    if (name.has_value())
    {
        for (size_t i = 0; i < workspace_names.size(); ++i)
        {
            if (workspace_names[i] == *name)
            {
                return i;
            }
        }
    }

    return std::nullopt;
}

WindowRuleResult WindowRules::match(
    WindowMatchInfo const& info,
    std::span<Monitor const> monitors,
    std::span<std::string const> workspace_names
) const
{
    WindowRuleResult result;

    // First match wins
    for (auto const& rule : rules_)
    {
        if (!matches_rule(rule, info))
        {
            continue;
        }

        result.matched = true;

        result.floating = rule.floating;

        result.target_monitor = resolve_monitor(rule.monitor, rule.monitor_name, monitors);
        result.target_workspace = resolve_workspace(rule.workspace, rule.workspace_name, workspace_names);

        result.fullscreen = rule.fullscreen;
        result.above = rule.above;
        result.below = rule.below;
        result.sticky = rule.sticky;
        result.skip_taskbar = rule.skip_taskbar;
        result.skip_pager = rule.skip_pager;

        if (rule.geometry.has_value())
        {
            Geometry geo;
            geo.x = static_cast<int16_t>(rule.geometry->x.value_or(0));
            geo.y = static_cast<int16_t>(rule.geometry->y.value_or(0));
            geo.width = static_cast<uint16_t>(rule.geometry->width.value_or(800));
            geo.height = static_cast<uint16_t>(rule.geometry->height.value_or(600));
            result.geometry = geo;
        }

        result.center = rule.center.value_or(false);

        break;
    }

    return result;
}

}
