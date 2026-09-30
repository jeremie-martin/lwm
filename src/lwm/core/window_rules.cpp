#include "window_rules.hpp"
#include <algorithm>

namespace lwm {

RuleActions const* match_window_rules(std::span<WindowRuleConfig const> rules, WindowMatchInfo const& info)
{
    for (auto const& rule : rules)
        if (rule.match.matches(info.wm_class, info.wm_class_name, info.title)
            && (!rule.type || *rule.type == info.ewmh_type) && (!rule.transient || *rule.transient == info.is_transient))
            return &rule.actions;
    return nullptr;
}

std::optional<size_t> resolve_rule_monitor(RuleActions const& actions, std::span<Monitor const> monitors)
{
    if (!actions.monitor)
        return std::nullopt;
    if (auto const* index = std::get_if<size_t>(&*actions.monitor))
        return *index < monitors.size() ? std::optional{ *index } : std::nullopt;
    auto const& name = std::get<std::string>(*actions.monitor);
    auto it = std::ranges::find(monitors, name, &Monitor::name);
    return it == monitors.end() ? std::nullopt : std::optional{ static_cast<size_t>(it - monitors.begin()) };
}

} // namespace lwm
