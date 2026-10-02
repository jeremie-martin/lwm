// Explicit scratchpad hiding uses iconic state; ordinary visibility remains derived.

#include "lwm/core/log.hpp"
#include "wm.hpp"
#include <algorithm>

namespace lwm {

ScratchpadConfig const* WindowManager::scratchpad_config(std::string_view name) const
{
    auto it = std::ranges::find(config_.scratchpads, name, &ScratchpadConfig::name);
    return it == config_.scratchpads.end() ? nullptr : &*it;
}

// Launch-pending state begins only after successful process creation and
// suppresses duplicate launches until a matching window arrives or IPC cancels it.
std::expected<void, std::string> WindowManager::toggle_scratchpad(std::string_view name)
{
    auto const* config = scratchpad_config(name);
    if (!config)
        return std::unexpected("unknown scratchpad: " + std::string(name));
    if (state_.toggle_scratchpad(*config) && launch_program(config->spawn, "scratchpad"))
        state_.scratchpad_pending(name, true);
    return { };
}

void WindowManager::stash_window(xcb_window_t window)
{
    auto const& client = state_.require(window);
    if (state_.scratchpad_claim(window) || state_.pooled(window) || client.fullscreen || client.iconic || drag_active())
        return;
    LWM_LOG_DEBUG("Stashing window {:#x} to scratchpad pool", window);
    state_.pool_scratchpad(window);
    state_.iconic(window, true);
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
        state_.iconic(window, true);
        state_.advance_scratchpad_pool();
        if (pool.size() > 1)
            show_pooled_scratchpad(pool.back());
    }
}

void WindowManager::show_pooled_scratchpad(xcb_window_t window)
{
    LWM_LOG_DEBUG("Showing pool scratchpad window {:#x}", window);
    size_t monitor = state_.focused_monitor();
    state_.relocate(window, monitor, state_.monitors()[monitor].current_workspace, State::RelocationGeometry::Translate);
    state_.restore(window, true);
}

} // namespace lwm
