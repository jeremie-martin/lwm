#include "window_rules.hpp"

namespace lwm {
namespace {
std::optional<size_t>
resolve_monitor(std::optional<int> index, std::optional<std::string> const& name, std::span<Monitor const> monitors)
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

std::optional<size_t> resolve_workspace(
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

}

WindowRuleResult match_window_rules(
    std::span<WindowRuleConfig const> rules,
    WindowMatchInfo const& info,
    std::span<Monitor const> monitors,
    std::span<std::string const> workspace_names
)
{
    WindowRuleResult result;

    // First match wins
    for (auto const& rule : rules)
    {
        if (!rule.match.matches(info.wm_class, info.wm_class_name, info.title)
            || (rule.type && *rule.type != info.ewmh_type) || (rule.transient && *rule.transient != info.is_transient))
        {
            continue;
        }

        result.matched = true;

        result.floating = rule.floating;

        result.target_monitor = resolve_monitor(rule.monitor, rule.monitor_name, monitors);
        result.target_workspace = resolve_workspace(rule.workspace, rule.workspace_name, workspace_names);

        result.fullscreen = rule.fullscreen;
        if (rule.above.value_or(false))
            result.layer_hint = LayerHint::Above;
        else if (rule.below.value_or(false))
            result.layer_hint = LayerHint::Below;
        else if (rule.above || rule.below)
            result.layer_hint = LayerHint::Normal;
        result.sticky = rule.sticky;
        result.skip_taskbar = rule.skip_taskbar;
        result.skip_pager = rule.skip_pager;
        result.borderless = rule.borderless;

        if (rule.geometry.has_value())
        {
            Geometry geo;
            geo.x = rule.geometry->x.value_or(0);
            geo.y = rule.geometry->y.value_or(0);
            geo.width = rule.geometry->width.value_or(800);
            geo.height = rule.geometry->height.value_or(600);
            result.geometry = geo;
        }

        result.center = rule.center.value_or(false);
        result.scratchpad = rule.scratchpad;

        break;
    }

    return result;
}

}
