#pragma once

#include <algorithm>
#include "lwm/core/types.hpp"
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

inline bool is_workspace_visible(
    bool showing_desktop,
    size_t monitor_idx,
    size_t workspace_idx,
    std::span<Monitor const> monitors
)
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

namespace lwm::focus_policy {

inline bool is_focus_eligible(Client::Kind kind, bool accepts_input_focus, bool supports_take_focus)
{
    if (kind == Client::Kind::Dock || kind == Client::Kind::Desktop)
        return false;
    return accepts_input_focus || supports_take_focus;
}

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
    std::span<FloatingCandidate const> floating_mru,
    std::function<bool(xcb_window_t)> const& is_eligible
)
{
    auto eligible = [&](xcb_window_t window) {
        return window != XCB_NONE && is_eligible(window);
    };

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

} // namespace lwm::focus_policy

namespace lwm::workspace_policy {

struct WorkspaceSwitchResult
{
    size_t old_workspace = 0;
    size_t new_workspace = 0;
};

inline std::optional<WorkspaceSwitchResult> apply_workspace_switch(Monitor& monitor, int target_ws)
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

    size_t old = monitor.current_workspace;
    monitor.previous_workspace = old;
    monitor.current_workspace = target;
    return WorkspaceSwitchResult{ old, target };
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

    auto& source_ws = monitor.current();
    auto it = source_ws.find_window(window);
    if (it == source_ws.windows.end())
        return false;

    source_ws.windows.erase(it);
    if (source_ws.focused_window == window)
    {
        source_ws.focused_window = XCB_NONE;
        for (auto rit = source_ws.windows.rbegin(); rit != source_ws.windows.rend(); ++rit)
        {
            if (!is_iconic(*rit))
            {
                source_ws.focused_window = *rit;
                break;
            }
        }
    }

    auto& target = monitor.workspaces[target_ws];
    target.windows.push_back(window);
    target.focused_window = window;
    return true;
}

} // namespace lwm::workspace_policy
