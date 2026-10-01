// Explicit scratchpad hiding uses iconic state; ordinary visibility remains derived.

#include "lwm/core/log.hpp"
#include "wm.hpp"
#include <algorithm>

namespace lwm {

void WindowManager::configure_scratchpads()
{
    std::vector<std::string> names;
    for (auto const& scratchpad : config_.scratchpads) names.push_back(scratchpad.name);
    state_.configure_scratchpads(names);
}

ScratchpadConfig const* WindowManager::scratchpad_config(std::string_view name) const
{
    auto it = std::ranges::find(config_.scratchpads, name, &ScratchpadConfig::name);
    return it == config_.scratchpads.end() ? nullptr : &*it;
}

// A rule naming a scratchpad takes precedence over scratchpad matchers. Only
// unclaimed names can claim a window.
std::optional<std::string> WindowManager::match_scratchpad(WindowMatchInfo const& properties, RuleActions const* rule) const
{
    auto unclaimed = [&](std::string const& name)
    {
        auto const* slot = state_.named_scratchpad(name);
        return slot && slot->window() == XCB_NONE;
    };
    if (rule && rule->scratchpad && unclaimed(*rule->scratchpad))
        return rule->scratchpad;
    for (auto const& scratchpad : config_.scratchpads)
        if (unclaimed(scratchpad.name)
            && scratchpad.match.matches(properties.wm_class, properties.wm_class_name, properties.title))
            return scratchpad.name;
    return std::nullopt;
}

// A pending launch shows the claimed window; an unrequested match starts hidden.
void WindowManager::claim_scratchpad(xcb_window_t window, std::string const& name)
{
    auto const* slot = state_.named_scratchpad(name);
    if (!slot || slot->window() != XCB_NONE)
        return;
    bool requested = slot->pending_launch();
    state_.claim_scratchpad(name, window);
    if (requested)
        show_named_scratchpad(window, *scratchpad_config(name));
    else
        hide_scratchpad(window);
}

// Metadata can arrive after mapping; a pending launch still claims the window.
bool WindowManager::claim_pending_scratchpad(xcb_window_t window, WindowMatchInfo const& properties, RuleActions const* rule)
{
    if (state_.scratchpad_claim(window) || state_.pooled(window))
        return false;
    auto name = match_scratchpad(properties, rule);
    if (!name || !state_.named_scratchpad(*name)->pending_launch())
        return false;
    claim_scratchpad(window, *name);
    return true;
}

// Launch-pending state begins only after successful process creation and
// suppresses duplicate launches until a matching window arrives or IPC cancels it.
std::expected<void, std::string> WindowManager::toggle_scratchpad(std::string_view name)
{
    auto const* slot = state_.named_scratchpad(name);
    if (!slot)
        return std::unexpected("unknown scratchpad: " + std::string(name));
    auto const& config = *scratchpad_config(name);
    xcb_window_t window = slot->window();
    if (window == XCB_NONE)
    {
        if (!slot->pending_launch() && launch_program(config.spawn, "scratchpad"))
            state_.scratchpad_pending(name, true);
        return { };
    }
    auto const& client = state_.require(window);
    if (client.monitor != state_.focused_monitor() || !state_.visible(client))
        show_named_scratchpad(window, config);
    else if (window == state_.active_window())
        hide_scratchpad(window);
    else
        state_.focus(window);
    return { };
}

void WindowManager::stash_window(xcb_window_t window)
{
    auto const& client = state_.require(window);
    if (state_.scratchpad_claim(window) || state_.pooled(window) || client.fullscreen || client.iconic || drag_active())
        return;
    LWM_LOG_DEBUG("Stashing window {:#x} to scratchpad pool", window);
    state_.pool_scratchpad(window);
    hide_scratchpad(window);
}

// Pool order owns selection, independently of workspace visibility or minimization.
// Recall/focus the target first; cycling an active target advances the rotation.
void WindowManager::cycle_scratchpad_pool()
{
    auto const& pool = state_.scratchpad_pool();
    if (pool.empty())
        return;
    auto window = pool.back();
    auto const& client = state_.require(window);
    if (client.monitor != state_.focused_monitor() || !state_.visible(client))
        show_pooled_scratchpad(window);
    else if (window != state_.active_window())
        state_.focus(window);
    else
    {
        hide_scratchpad(window);
        state_.advance_scratchpad_pool();
        if (pool.size() > 1)
            show_pooled_scratchpad(pool.back());
    }
}

void WindowManager::hide_scratchpad(xcb_window_t window)
{
    LWM_LOG_DEBUG("Hiding scratchpad window {:#x}", window);
    state_.iconic(window, true);
}

void WindowManager::show_named_scratchpad(xcb_window_t window, ScratchpadConfig const& config)
{
    LWM_LOG_DEBUG("Showing named scratchpad '{}' window {:#x}", config.name, window);
    size_t monitor = state_.focused_monitor();
    size_t workspace = state_.monitors()[monitor].current_workspace;
    state_.floating(window, true);
    Geometry area = state_.monitors()[monitor].working_area();
    auto width = geometry_extent(static_cast<int64_t>(area.width * config.width));
    auto height = geometry_extent(static_cast<int64_t>(area.height * config.height));
    state_.geometry(window, floating::place_floating(area, width, height, std::nullopt));
    state_.relocate(window, monitor, workspace);
    state_.restore(window, true);
}

void WindowManager::show_pooled_scratchpad(xcb_window_t window)
{
    LWM_LOG_DEBUG("Showing pool scratchpad window {:#x}", window);
    size_t monitor = state_.focused_monitor();
    state_.relocate(window, monitor, state_.monitors()[monitor].current_workspace, State::RelocationGeometry::Translate);
    state_.restore(window, true);
}

} // namespace lwm
