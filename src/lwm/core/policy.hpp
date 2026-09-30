#pragma once

// Pure desktop numbering, workspace focus memory, and output rebinding rules.

#include "lwm/core/types.hpp"
#include <algorithm>
#include <cassert>
#include <functional>
#include <optional>
#include <span>
#include <tuple>
#include <unordered_map>
#include <utility>

namespace lwm::ewmh_policy {

/// X timestamps are 32-bit millisecond counters and comparisons must treat
/// subtraction as a signed delta so ordering remains correct across wraparound.
inline bool timestamp_is_before(uint32_t timestamp, uint32_t reference)
{
    return timestamp != reference && timestamp - reference >= 0x80000000U;
}

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

// A concrete desktop on an existing monitor; the sticky value and other monitors are not placements.
inline std::optional<std::pair<size_t, size_t>>
desktop_placement(uint32_t desktop, size_t workspaces_per_monitor, size_t monitor_count)
{
    auto indices = desktop == 0xFFFFFFFF ? std::nullopt : desktop_to_indices(desktop, workspaces_per_monitor);
    if (!indices || indices->first >= monitor_count)
        return std::nullopt;
    return indices;
}

} // namespace lwm::ewmh_policy

namespace lwm::workspace_policy {

struct WorkspaceSwitchResult
{
    size_t old_workspace = 0;
    size_t new_workspace = 0;
};

inline std::optional<WorkspaceSwitchResult> validate_workspace_switch(Monitor const& monitor, size_t target_ws)
{
    size_t workspace_count = monitor.workspaces.size();
    if (workspace_count == 0)
        return std::nullopt;
    if (target_ws >= workspace_count)
        return std::nullopt;
    if (target_ws == monitor.current_workspace)
        return std::nullopt;

    return WorkspaceSwitchResult{ monitor.current_workspace, target_ws };
}

constexpr size_t kFocusHistoryMax = 16;

/// Push a window to the top (back) of focus_history, removing duplicates.
inline void push_focus_history(Workspace& ws, xcb_window_t window)
{
    if (window == XCB_NONE)
        return;
    std::erase(ws.focus_history, window);
    ws.focus_history.push_back(window);
    if (ws.focus_history.size() > kFocusHistoryMax)
        ws.focus_history.erase(ws.focus_history.begin());
}

/// Remove a window from focus_history.
inline void remove_from_focus_history(Workspace& ws, xcb_window_t window) { std::erase(ws.focus_history, window); }

/// Set focused_window and update focus_history in one step.
inline void set_workspace_focus(Workspace& ws, xcb_window_t window)
{
    ws.focused_window = window;
    push_focus_history(ws, window);
}

/// Fix up workspace.focused_window after a window has been removed from the window list.
/// Consults focus_history first (MRU), then falls back to last non-iconic window.
inline void
fixup_workspace_focus(Workspace& ws, xcb_window_t removed_window, std::function<bool(xcb_window_t)> const& is_iconic)
{
    if (ws.focused_window != removed_window)
        return;
    ws.focused_window = XCB_NONE;

    // Try focus_history (MRU order, back = most recent)
    for (auto rit = ws.focus_history.rbegin(); rit != ws.focus_history.rend(); ++rit)
    {
        if (*rit != removed_window && ws.find_window(*rit) != ws.windows.end() && !is_iconic(*rit))
        {
            xcb_window_t target = *rit; // Extract before set_workspace_focus invalidates iterators
            set_workspace_focus(ws, target);
            return;
        }
    }

    // Fallback: reverse-iterate window list
    for (auto rit = ws.windows.rbegin(); rit != ws.windows.rend(); ++rit)
    {
        if (!is_iconic(*rit))
        {
            xcb_window_t target = *rit; // Extract before set_workspace_focus invalidates iterators
            set_workspace_focus(ws, target);
            break;
        }
    }
}

} // namespace lwm::workspace_policy

namespace lwm::hotplug_policy {

// Transfer complete workspace state for surviving outputs. Discovery supplies
// fresh geometry; old geometry remains available to relocate floating clients.
// Every monitor has the same nonzero configured workspace count.
inline std::vector<size_t> preserve_workspaces(std::span<Monitor> previous, std::span<Monitor> discovered)
{
    assert(!discovered.empty());
    std::vector<size_t> destinations(previous.size(), 0);
    for (size_t old = 0; old < previous.size(); ++old)
    {
        for (size_t next = 0; next < discovered.size(); ++next)
        {
            if (previous[old].name != discovered[next].name)
                continue;
            destinations[old] = next;
            auto& source = previous[old];
            auto& target = discovered[next];
            assert(source.workspaces.size() == target.workspaces.size());
            target.workspaces = std::move(source.workspaces);
            source.workspaces.clear();
            target.current_workspace = source.current_workspace;
            target.previous_workspace = source.previous_workspace;
            break;
        }
    }
    // Removed outputs fall back to output 0. Surviving workspace order and
    // focus take precedence; incoming tiled clients retain their relative order.
    for (auto& source : previous)
    {
        for (size_t w = 0; w < source.workspaces.size(); ++w)
        {
            auto& from = source.workspaces[w];
            auto& to = discovered[0].workspaces[w];
            to.windows.insert(to.windows.end(), from.windows.begin(), from.windows.end());
            if (to.focused_window == XCB_NONE && !from.windows.empty())
            {
                to.focused_window = from.focused_window;
                to.focus_history = std::move(from.focus_history);
            }
        }
    }
    return destinations;
}

} // namespace lwm::hotplug_policy
