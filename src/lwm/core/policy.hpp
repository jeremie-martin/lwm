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

} // namespace lwm::visibility_policy

namespace lwm::fullscreen_policy {

struct FullscreenCandidate
{
    xcb_window_t window = XCB_NONE;
    bool eligible = false;
    uint64_t order = 0;
};

inline xcb_window_t
select_owner(xcb_window_t current_owner, std::span<FullscreenCandidate const> candidates)
{
    for (auto const& candidate : candidates)
    {
        if (candidate.window == current_owner && candidate.eligible)
            return current_owner;
    }

    xcb_window_t best = XCB_NONE;
    uint64_t best_order = 0;
    for (auto const& candidate : candidates)
    {
        if (!candidate.eligible)
            continue;
        if (best == XCB_NONE || candidate.order > best_order)
        {
            best = candidate.window;
            best_order = candidate.order;
        }
    }
    return best;
}

} // namespace lwm::fullscreen_policy

namespace lwm::focus_policy {

inline bool is_focus_eligible(bool accepts_input_focus, bool supports_take_focus)
{
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

    // Focus history (MRU order, back = most recent)
    for (auto it = workspace.focus_history.rbegin(); it != workspace.focus_history.rend(); ++it)
    {
        if (workspace.find_window(*it) != workspace.windows.end() && eligible(*it))
            return FocusSelection{ *it, false };
    }

    // Fallback: last window in tiled list
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

struct FocusCycleCandidate
{
    xcb_window_t id = XCB_NONE;
    bool is_floating = false;
};

/// Build cycle candidates sorted by MRU order (most recently used first).
/// get_mru_order returns the MRU timestamp for a window (higher = more recent).
inline std::vector<FocusCycleCandidate> build_cycle_candidates(
    std::span<xcb_window_t const> tiled_windows,
    std::span<FloatingCandidate const> floating_windows,
    size_t monitor_idx,
    size_t workspace_idx,
    std::function<bool(xcb_window_t)> const& is_eligible,
    std::function<uint64_t(xcb_window_t)> const& get_mru_order
)
{
    struct Ranked
    {
        FocusCycleCandidate candidate;
        uint64_t mru;
    };
    std::vector<Ranked> ranked;

    for (xcb_window_t w : tiled_windows)
    {
        if (is_eligible(w))
            ranked.push_back({ { w, false }, get_mru_order(w) });
    }

    for (auto const& fw : floating_windows)
    {
        if (fw.monitor != monitor_idx)
            continue;
        if (!fw.sticky && fw.workspace != workspace_idx)
            continue;
        if (is_eligible(fw.id))
            ranked.push_back({ { fw.id, true }, get_mru_order(fw.id) });
    }

    std::stable_sort(ranked.begin(), ranked.end(), [](auto const& a, auto const& b) { return a.mru > b.mru; });

    std::vector<FocusCycleCandidate> result;
    result.reserve(ranked.size());
    for (auto& r : ranked)
        result.push_back(std::move(r.candidate));
    return result;
}

/// Find the next window in focus cycle order.
/// Returns nullopt if no candidates or cycling not possible.
inline std::optional<FocusCycleCandidate>
cycle_focus_next(std::span<FocusCycleCandidate const> candidates, xcb_window_t current_window)
{
    if (candidates.empty())
        return std::nullopt;

    // Find current position
    bool found = false;
    size_t current = 0;
    for (size_t i = 0; i < candidates.size(); ++i)
    {
        if (candidates[i].id == current_window)
        {
            current = i;
            found = true;
            break;
        }
    }

    if (!found)
        return candidates[0];

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
    bool found = false;
    size_t current = 0;
    for (size_t i = 0; i < candidates.size(); ++i)
    {
        if (candidates[i].id == current_window)
        {
            current = i;
            found = true;
            break;
        }
    }

    if (!found)
        return candidates.back();

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

namespace lwm::stacking_policy {

/// Stacking tier: defines coarse layering across monitors.
/// Higher tier = visually on top.  Within a tier, ordering falls back to
/// (kind_floating, active, order).
enum class Tier : int
{
    Below = 0,      ///< _NET_WM_STATE_BELOW or suppressed by another window's fullscreen
    Normal = 1,     ///< Default tier for tiled and floating windows
    Above = 2,      ///< _NET_WM_STATE_ABOVE or modal
    Fullscreen = 3, ///< Fullscreen owner of its monitor
};

/// Inputs the policy needs to rank a single client.  Caller-side concerns
/// (kind, transient_for, etc.) are resolved before building these.
struct ClientStackInputs
{
    xcb_window_t id = XCB_NONE;
    bool visible = true;     ///< Policy-visible (not iconic, on current workspace)
    Tier tier = Tier::Normal;
    bool is_floating = false;
    bool is_active = false;
    uint64_t order = 0;
};

/// Decide which tier a client belongs to.  `is_suppressed_by_fullscreen`
/// overrides every other state — a window occluded by another fullscreen
/// owner sinks to Below regardless of its own layer hint.
inline Tier compute_tier(
    bool is_suppressed_by_fullscreen,
    bool is_fullscreen,
    bool is_above_hint,
    bool is_below_hint,
    bool is_modal)
{
    if (is_suppressed_by_fullscreen)
        return Tier::Below;
    if (is_fullscreen)
        return Tier::Fullscreen;
    if (is_above_hint || is_modal)
        return Tier::Above;
    if (is_below_hint)
        return Tier::Below;
    return Tier::Normal;
}

/// Compute the global stacking order: bottom-up, hidden first, visible last.
/// Stable on `order` so equal keys never reshuffle.
inline std::vector<xcb_window_t> compute_order(std::span<ClientStackInputs const> inputs)
{
    auto sort_key = [](ClientStackInputs const& in) {
        return std::tuple{
            in.visible ? 1 : 0,
            static_cast<int>(in.tier),
            in.is_floating ? 1 : 0,
            in.is_active ? 1 : 0,
            static_cast<long long>(in.order),
        };
    };

    std::vector<ClientStackInputs> ranked(inputs.begin(), inputs.end());
    std::stable_sort(ranked.begin(), ranked.end(),
        [&](auto const& a, auto const& b) { return sort_key(a) < sort_key(b); });

    std::vector<xcb_window_t> result;
    result.reserve(ranked.size());
    for (auto const& in : ranked)
        result.push_back(in.id);
    return result;
}

struct StackMove
{
    xcb_window_t window;
    xcb_window_t sibling;
    uint32_t mode;
};

// Keep a longest subsequence already in server order. Each other visible
// managed window needs one move; unrelated root children are not reordered.
inline std::vector<StackMove>
plan_moves(std::span<xcb_window_t const> server_order, std::span<xcb_window_t const> desired_order)
{
    std::unordered_map<xcb_window_t, size_t> positions;
    positions.reserve(server_order.size());
    for (size_t i = 0; i < server_order.size(); ++i) positions.emplace(server_order[i], i);
    std::vector<xcb_window_t> windows;
    std::vector<size_t> ranks;
    for (auto window : desired_order)
    {
        auto found = positions.find(window);
        // A client may have been destroyed since the policy was computed.
        if (found == positions.end())
            continue;
        windows.push_back(window);
        ranks.push_back(found->second);
    }
    if (std::is_sorted(ranks.begin(), ranks.end()))
        return {};
    size_t none = windows.size();
    std::vector<size_t> tails, previous(windows.size(), none);
    for (size_t i = 0; i < windows.size(); ++i)
    {
        auto tail = std::lower_bound(
            tails.begin(),
            tails.end(),
            ranks[i],
            [&](size_t index, size_t rank) { return ranks[index] < rank; }
        );
        if (tail != tails.begin())
            previous[i] = *(tail - 1);
        if (tail == tails.end())
            tails.push_back(i);
        else
            *tail = i;
    }
    std::vector<bool> keep(windows.size());
    size_t anchor = tails.back();
    for (size_t i = anchor; i != none; i = previous[i])
    {
        keep[i] = true;
        anchor = i;
    }
    std::vector<StackMove> moves;
    moves.reserve(windows.size() - tails.size());
    for (size_t i = 0; i < windows.size(); ++i)
        if (!keep[i])
            moves.push_back(
                { windows[i], i ? windows[i - 1] : windows[anchor], i ? XCB_STACK_MODE_ABOVE : XCB_STACK_MODE_BELOW }
            );
    return moves;
}

} // namespace lwm::stacking_policy
