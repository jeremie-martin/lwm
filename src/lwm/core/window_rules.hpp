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

// First matching rule wins. Resolve placement against the current outputs.
WindowRuleResult match_window_rules(
    std::span<WindowRuleConfig const> rules,
    WindowMatchInfo const& info,
    std::span<Monitor const> monitors,
    std::span<std::string const> workspace_names
);

} // namespace lwm
