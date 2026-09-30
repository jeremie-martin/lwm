#pragma once

#include "lwm/config/config.hpp"
#include "lwm/core/types.hpp"
#include <span>
#include <string>

namespace lwm {

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

// First matching rule wins.
RuleActions const* match_window_rules(std::span<WindowRuleConfig const> rules, WindowMatchInfo const& info);

// Resolve a rule's monitor against the current outputs; nullopt leaves the monitor unchanged.
std::optional<size_t> resolve_rule_monitor(RuleActions const& actions, std::span<Monitor const> monitors);

} // namespace lwm
