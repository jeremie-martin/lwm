#pragma once

/**
 * @file policy.hpp
 * @brief Pure policy functions for the window manager's state model.
 *
 * These functions are the executable form of the policies summarized in
 * `ARCHITECTURE.md`; keep behavior changes covered by the corresponding tests.
 */

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
    if (is_iconic)
        return false;
    if (client_monitor >= monitors.size())
        return false;
    if (is_sticky)
        return true;
    if (showing_desktop)
        return false;
    return client_workspace == monitors[client_monitor].current_workspace;
}

// The caller supplies ownership resolved for the client's visible monitor scope.
inline bool is_fullscreen_suppressed(Client const& client, xcb_window_t owner)
{
    return owner != XCB_NONE && owner != client.id && owner != client.transient_for;
}

} // namespace lwm::visibility_policy

namespace lwm::fullscreen_policy {

// Selection reads authoritative placement and state; no candidate snapshot is needed.
inline xcb_window_t select_owner(
    std::unordered_map<xcb_window_t, Client> const& clients,
    std::span<Monitor const> monitors,
    size_t monitor,
    bool showing_desktop,
    xcb_window_t preferred = XCB_NONE
)
{
    if (monitor >= monitors.size() || showing_desktop)
        return XCB_NONE;
    auto eligible = [&](Client const& client)
    {
        return client.monitor == monitor && client.fullscreen && !client.iconic
            && (client.kind() == Client::Kind::Tiled || client.kind() == Client::Kind::Floating)
            && (client.sticky || client.workspace == monitors[monitor].current_workspace);
    };
    for (auto id : { preferred, monitors[monitor].fullscreen_owner })
        if (auto it = clients.find(id); it != clients.end() && eligible(it->second))
            return id;
    Client const* newest = nullptr;
    for (auto const& [id, client] : clients)
        if (eligible(client) && (!newest || std::tie(client.order, client.id) > std::tie(newest->order, newest->id)))
            newest = &client;
    return newest ? newest->id : XCB_NONE;
}

} // namespace lwm::fullscreen_policy

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
inline void remove_from_focus_history(Workspace& ws, xcb_window_t window)
{
    std::erase(ws.focus_history, window);
}

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

inline bool
remove_tiled_window(Workspace& workspace, xcb_window_t window, std::function<bool(xcb_window_t)> const& is_iconic)
{
    auto it = workspace.find_window(window);
    if (it == workspace.windows.end())
        return false;
    workspace.windows.erase(it);
    remove_from_focus_history(workspace, window);
    fixup_workspace_focus(workspace, window, is_iconic);
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
    set_workspace_focus(target, window);
    return true;
}

} // namespace lwm::workspace_policy

namespace lwm::classification_policy {

struct DesiredWindowState
{
    bool skip_taskbar = false;
    bool skip_pager = false;
    bool sticky = false;
    bool modal = false;
    LayerHint layer_hint = LayerHint::Normal;
    bool borderless = false;
};

struct DesiredStateInputs
{
    bool classification_skip_taskbar = false;
    bool classification_skip_pager = false;
    bool classification_above = false;

    bool app_skip_taskbar = false;
    bool app_skip_pager = false;
    bool ewmh_sticky = false;
    bool ewmh_modal = false;
    bool app_above = false;
    bool app_below = false;

    std::optional<bool> rule_skip_taskbar;
    std::optional<bool> rule_skip_pager;
    std::optional<bool> rule_sticky;
    std::optional<LayerHint> rule_layer_hint;
    std::optional<bool> rule_borderless;

    bool has_transient = false;
    bool is_sticky_desktop = false;
};

inline DesiredWindowState compute_desired_state(DesiredStateInputs const& in)
{
    DesiredWindowState out;

    out.skip_taskbar = in.has_transient || in.classification_skip_taskbar || in.app_skip_taskbar;
    if (in.rule_skip_taskbar.has_value())
        out.skip_taskbar = *in.rule_skip_taskbar;

    out.skip_pager = in.has_transient || in.classification_skip_pager || in.app_skip_pager;
    if (in.rule_skip_pager.has_value())
        out.skip_pager = *in.rule_skip_pager;

    out.sticky = in.is_sticky_desktop || in.ewmh_sticky;
    if (in.rule_sticky.has_value())
        out.sticky = *in.rule_sticky;

    out.modal = in.ewmh_modal;

    LayerHint hint = LayerHint::Normal;
    if (!out.modal)
    {
        if (in.classification_above || in.app_above)
            hint = LayerHint::Above;
        else if (in.app_below)
            hint = LayerHint::Below;
    }
    if (in.rule_layer_hint.has_value())
        hint = *in.rule_layer_hint;
    if (out.modal && hint == LayerHint::Below)
        hint = LayerHint::Normal;
    out.layer_hint = hint;

    out.borderless = in.rule_borderless.value_or(false);

    return out;
}

} // namespace lwm::classification_policy

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
            // Reconciliation revalidates this owner against the rebound clients.
            target.fullscreen_owner = source.fullscreen_owner;
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
