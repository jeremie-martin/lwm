#pragma once

#include "lwm/core/types.hpp"
#include <algorithm>
#include <functional>
#include <optional>
#include <span>
#include <utility>

namespace lwm::ewmh_policy {

inline uint32_t desktop_index(size_t monitor_idx, size_t workspace_idx, size_t workspaces_per_monitor)
{
    return static_cast<uint32_t>(monitor_idx * workspaces_per_monitor + workspace_idx);
}

inline std::optional<std::pair<size_t, size_t>> desktop_to_indices(uint32_t desktop, size_t workspaces_per_monitor)
{
    if (workspaces_per_monitor == 0)
        return std::nullopt;

    size_t monitor_idx = static_cast<size_t>(desktop / workspaces_per_monitor);
    size_t workspace_idx = static_cast<size_t>(desktop % workspaces_per_monitor);
    return std::pair<size_t, size_t>{ monitor_idx, workspace_idx };
}

} // namespace lwm::ewmh_policy

namespace lwm::visibility_policy {

inline bool
is_workspace_visible(bool showing_desktop, size_t monitor_idx, size_t workspace_idx, std::span<Monitor const> monitors)
{
    if (showing_desktop)
        return false;
    if (monitor_idx >= monitors.size())
        return false;
    return workspace_idx == monitors[monitor_idx].current_workspace;
}

inline bool is_window_visible(
    bool showing_desktop,
    bool is_iconic,
    bool is_sticky,
    size_t client_monitor,
    size_t client_workspace,
    std::span<Monitor const> monitors
)
{
    if (showing_desktop)
        return false;
    if (is_iconic)
        return false;
    if (client_monitor >= monitors.size())
        return false;
    if (is_sticky)
        return true;
    return client_workspace == monitors[client_monitor].current_workspace;
}

} // namespace lwm::visibility_policy

namespace lwm::fullscreen_policy {

enum class ApplyContext
{
    StateTransition,
    VisibilityTransition,
    LayoutTransition,
    ConfigureTransition,
    FocusTransition,
};

inline bool should_reapply(ApplyContext context) { return context != ApplyContext::FocusTransition; }

} // namespace lwm::fullscreen_policy

namespace lwm::focus_policy {

inline bool is_focus_eligible(Client::Kind kind, bool accepts_input_focus, bool supports_take_focus)
{
    if (kind == Client::Kind::Dock || kind == Client::Kind::Desktop)
        return false;
    return accepts_input_focus || supports_take_focus;
}

inline bool should_apply_focus_border(bool is_fullscreen) { return !is_fullscreen; }

struct FloatingCandidate
{
    xcb_window_t id = XCB_NONE;
    size_t monitor = 0;
    size_t workspace = 0;
    bool sticky = false;
};

struct FocusSelection
{
    xcb_window_t window = XCB_NONE;
    bool is_floating = false;
};

inline std::optional<FocusSelection> select_focus_candidate(
    Workspace const& workspace,
    size_t monitor_idx,
    size_t workspace_idx,
    std::span<xcb_window_t const> sticky_tiled,
    std::span<FloatingCandidate const> floating_mru,
    std::function<bool(xcb_window_t)> const& is_eligible
)
{
    auto eligible = [&](xcb_window_t window) { return window != XCB_NONE && is_eligible(window); };

    if (workspace.focused_window != XCB_NONE
        && workspace.find_window(workspace.focused_window) != workspace.windows.end()
        && eligible(workspace.focused_window))
    {
        return FocusSelection{ workspace.focused_window, false };
    }

    for (auto it = workspace.windows.rbegin(); it != workspace.windows.rend(); ++it)
    {
        if (eligible(*it))
            return FocusSelection{ *it, false };
    }

    for (auto it = sticky_tiled.rbegin(); it != sticky_tiled.rend(); ++it)
    {
        if (eligible(*it))
            return FocusSelection{ *it, false };
    }

    for (auto it = floating_mru.rbegin(); it != floating_mru.rend(); ++it)
    {
        if (it->monitor != monitor_idx)
            continue;
        if (!it->sticky && it->workspace != workspace_idx)
            continue;
        if (eligible(it->id))
            return FocusSelection{ it->id, true };
    }

    return std::nullopt;
}

template <typename T, typename IdGetter>
inline bool promote_mru(std::vector<T>& items, xcb_window_t id, IdGetter get_id)
{
    auto it = std::find_if(items.begin(), items.end(), [&](T const& item) { return get_id(item) == id; });
    if (it == items.end())
        return false;
    if ((it + 1) == items.end())
        return false;

    T saved = *it;
    items.erase(it);
    items.push_back(saved);
    return true;
}

struct FocusCycleCandidate
{
    xcb_window_t id = XCB_NONE;
    bool is_floating = false;
};

/// Build a list of focus-cycleable candidates from tiled and floating windows.
/// Returns candidates in order: tiled first, then floating.
inline std::vector<FocusCycleCandidate> build_cycle_candidates(
    std::span<xcb_window_t const> tiled_windows,
    std::span<FloatingCandidate const> floating_windows,
    size_t monitor_idx,
    size_t workspace_idx,
    std::function<bool(xcb_window_t)> const& is_eligible
)
{
    std::vector<FocusCycleCandidate> candidates;

    for (xcb_window_t w : tiled_windows)
    {
        if (is_eligible(w))
            candidates.push_back({ w, false });
    }

    for (auto const& fw : floating_windows)
    {
        if (fw.monitor != monitor_idx)
            continue;
        if (!fw.sticky && fw.workspace != workspace_idx)
            continue;
        if (is_eligible(fw.id))
            candidates.push_back({ fw.id, true });
    }

    return candidates;
}

/// Find the next window in focus cycle order.
/// Returns nullopt if no candidates or cycling not possible.
inline std::optional<FocusCycleCandidate>
cycle_focus_next(std::span<FocusCycleCandidate const> candidates, xcb_window_t current_window)
{
    if (candidates.empty())
        return std::nullopt;

    // Find current position
    size_t current = 0;
    for (size_t i = 0; i < candidates.size(); ++i)
    {
        if (candidates[i].id == current_window)
        {
            current = i;
            break;
        }
    }

    // Move to next with wrap
    size_t next = (current + 1) % candidates.size();
    return candidates[next];
}

/// Find the previous window in focus cycle order.
/// Returns nullopt if no candidates or cycling not possible.
inline std::optional<FocusCycleCandidate>
cycle_focus_prev(std::span<FocusCycleCandidate const> candidates, xcb_window_t current_window)
{
    if (candidates.empty())
        return std::nullopt;

    // Find current position
    size_t current = 0;
    for (size_t i = 0; i < candidates.size(); ++i)
    {
        if (candidates[i].id == current_window)
        {
            current = i;
            break;
        }
    }

    // Move to prev with wrap
    size_t prev = (current + candidates.size() - 1) % candidates.size();
    return candidates[prev];
}

} // namespace lwm::focus_policy

namespace lwm::workspace_policy {

struct WorkspaceSwitchResult
{
    size_t old_workspace = 0;
    size_t new_workspace = 0;
};

inline std::optional<WorkspaceSwitchResult> validate_workspace_switch(Monitor const& monitor, int target_ws)
{
    size_t workspace_count = monitor.workspaces.size();
    if (workspace_count == 0)
        return std::nullopt;
    if (target_ws < 0)
        return std::nullopt;

    size_t target = static_cast<size_t>(target_ws);
    if (target >= workspace_count)
        return std::nullopt;
    if (target == monitor.current_workspace)
        return std::nullopt;

    return WorkspaceSwitchResult{ monitor.current_workspace, target };
}

inline bool remove_tiled_window(
    Workspace& workspace,
    xcb_window_t window,
    std::function<bool(xcb_window_t)> const& is_iconic
)
{
    auto it = workspace.find_window(window);
    if (it == workspace.windows.end())
        return false;
    workspace.windows.erase(it);
    if (workspace.focused_window == window)
    {
        workspace.focused_window = XCB_NONE;
        for (auto rit = workspace.windows.rbegin(); rit != workspace.windows.rend(); ++rit)
        {
            if (!is_iconic(*rit))
            {
                workspace.focused_window = *rit;
                break;
            }
        }
    }
    return true;
}

inline bool move_tiled_window(
    Monitor& monitor,
    xcb_window_t window,
    size_t target_ws,
    std::function<bool(xcb_window_t)> const& is_iconic
)
{
    size_t workspace_count = monitor.workspaces.size();
    if (workspace_count == 0)
        return false;
    if (target_ws >= workspace_count)
        return false;
    if (target_ws == monitor.current_workspace)
        return false;

    if (!remove_tiled_window(monitor.current(), window, is_iconic))
        return false;

    auto& target = monitor.workspaces[target_ws];
    target.windows.push_back(window);
    target.focused_window = window;
    return true;
}

} // namespace lwm::workspace_policy
