#pragma once

#include "lwm/config/config.hpp"
#include "lwm/core/types.hpp"
#include <span>

namespace lwm {

// First matching rule wins.
RuleActions const* match_window_rules(std::span<WindowRuleConfig const> rules, Client const& client);

// Resolve a rule's monitor against the current outputs; nullopt leaves the monitor unchanged.
std::optional<size_t> resolve_rule_monitor(RuleActions const& actions, std::span<Monitor const> monitors);

} // namespace lwm
