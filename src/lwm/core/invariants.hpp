#pragma once

/**
 * @file invariants.hpp
 * @brief Debug assertions for window manager invariants
 *
 * These assertions verify critical invariants that must hold at all times.
 * They are enabled in debug builds and can be disabled in release for performance.
 *
 * Key invariants:
 * 1. If `clients_` contains id then window is managed
 * 2. If client.iconic => WM_STATE Iconic and `_NET_WM_STATE` contains HIDDEN
 * 3. If focused window exists => `_NET_ACTIVE_WINDOW` equals it; else None
 * 4. Desktop indices valid or 0xFFFFFFFF
 * 5. Client state flags match EWMH `_NET_WM_STATE` atoms
 */

#include "log.hpp"
#include "types.hpp"
#include <algorithm>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <xcb/xcb.h>
#include <xcb/xcb_ewmh.h>

namespace lwm::invariants {

#ifdef NDEBUG
// Release build: no-op
#    define LWM_ASSERT_INVARIANTS(clients, monitors, floating, docks, desktops)
#    define LWM_ASSERT_CLIENT_STATE(...)
#    define LWM_ASSERT_FOCUS_CONSISTENCY(...)
#else

/**
 * @brief Assert that a window in clients_ is properly managed
 *
 * Verifies:
 * - Window exists in clients_ registry
 * - Window has valid monitor/workspace indices (or is dock/desktop)
 */
inline void assert_client_managed(
    std::unordered_map<xcb_window_t, Client> const& clients,
    std::vector<Monitor> const& monitors,
    xcb_window_t window
)
{
    auto it = clients.find(window);
    if (it == clients.end())
    {
        LOG_ERROR("INVARIANT VIOLATION: Window {:#x} not in clients registry", window);
        return;
    }

    auto const& client = it->second;
    if (client.kind == Client::Kind::Tiled || client.kind == Client::Kind::Floating)
    {
        if (client.monitor >= monitors.size())
        {
            LOG_ERROR("INVARIANT VIOLATION: Window {:#x} has invalid monitor index {}", window, client.monitor);
        }
        if (client.monitor < monitors.size() && client.workspace >= monitors[client.monitor].workspaces.size())
        {
            LOG_ERROR("INVARIANT VIOLATION: Window {:#x} has invalid workspace index {}", window, client.workspace);
        }
    }
}

/**
 * @brief Assert focus consistency
 *
 * Verifies:
 * - If active_window_ != XCB_NONE, it must be in clients_
 * - The active window must be visible and focus-eligible
 */
inline void
assert_focus_consistency(std::unordered_map<xcb_window_t, Client> const& clients, xcb_window_t active_window)
{
    if (active_window == XCB_NONE)
        return;

    auto it = clients.find(active_window);
    if (it == clients.end())
    {
        LOG_ERROR("INVARIANT VIOLATION: Active window {:#x} not in clients registry", active_window);
        return;
    }

    auto const& client = it->second;
    if (client.iconic)
    {
        LOG_ERROR("INVARIANT VIOLATION: Active window {:#x} is iconic (minimized)", active_window);
    }
}

/**
 * @brief Assert client state consistency
 *
 * Verifies state flags are internally consistent:
 * - If fullscreen, should not also be iconic
 * - If above, should not also be below
 */
inline void assert_client_state_consistency(Client const& client)
{
    if (client.fullscreen && client.iconic)
    {
        LOG_ERROR("INVARIANT VIOLATION: Window {:#x} is both fullscreen and iconic", client.id);
    }
    if (client.above && client.below)
    {
        LOG_ERROR("INVARIANT VIOLATION: Window {:#x} is both above and below", client.id);
    }
}

/**
 * @brief Assert desktop index validity
 *
 * Verifies:
 * - Desktop index is valid OR 0xFFFFFFFF (sticky)
 */
inline void assert_valid_desktop(uint32_t desktop, size_t num_monitors, size_t workspaces_per_monitor)
{
    if (desktop == 0xFFFFFFFF)
        return; // Sticky is valid

    uint32_t max_desktop = static_cast<uint32_t>(num_monitors * workspaces_per_monitor);
    if (desktop >= max_desktop)
    {
        LOG_ERROR("INVARIANT VIOLATION: Desktop index {} exceeds maximum {}", desktop, (max_desktop - 1));
    }
}

/**
 * @brief Assert workspace consistency across monitors
 *
 * Verifies:
 * - Each tiled window in workspace vectors exists in clients_
 * - Each client with Kind::Tiled appears in exactly one workspace
 */
inline void assert_workspace_consistency(
    std::unordered_map<xcb_window_t, Client> const& clients,
    std::vector<Monitor> const& monitors
)
{
    std::unordered_set<xcb_window_t> seen;

    for (size_t m = 0; m < monitors.size(); ++m)
    {
        for (size_t w = 0; w < monitors[m].workspaces.size(); ++w)
        {
            for (xcb_window_t win : monitors[m].workspaces[w].windows)
            {
                if (seen.contains(win))
                {
                    LOG_ERROR("INVARIANT VIOLATION: Window {:#x} appears in multiple workspaces", win);
                }
                seen.insert(win);

                auto it = clients.find(win);
                if (it == clients.end())
                {
                    LOG_ERROR("INVARIANT VIOLATION: Window {:#x} in workspace but not in clients registry", win);
                }
                else if (it->second.kind != Client::Kind::Tiled)
                {
                    LOG_ERROR("INVARIANT VIOLATION: Window {:#x} in workspace but not Kind::Tiled", win);
                }
            }
        }
    }

    // Verify all Tiled clients are in some workspace
    for (auto const& [id, client] : clients)
    {
        if (client.kind == Client::Kind::Tiled && !seen.contains(id))
        {
            LOG_ERROR("INVARIANT VIOLATION: Tiled client {:#x} not found in any workspace", id);
        }
    }
}

/**
 * @brief Assert workspace focused_window validity
 *
 * Verifies for each workspace:
 * - focused_window is either XCB_NONE or present in workspace.windows
 * - focused_window is not iconic
 */
inline void assert_workspace_focus_valid(
    std::unordered_map<xcb_window_t, Client> const& clients,
    std::vector<Monitor> const& monitors
)
{
    for (size_t m = 0; m < monitors.size(); ++m)
    {
        for (size_t w = 0; w < monitors[m].workspaces.size(); ++w)
        {
            auto const& ws = monitors[m].workspaces[w];
            if (ws.focused_window == XCB_NONE)
                continue;

            if (ws.find_window(ws.focused_window) == ws.windows.end())
            {
                LOG_ERROR(
                    "INVARIANT VIOLATION: Workspace [{}][{}] focused_window {:#x} not in windows list",
                    m,
                    w,
                    ws.focused_window
                );
            }

            auto it = clients.find(ws.focused_window);
            if (it != clients.end() && it->second.iconic)
            {
                LOG_ERROR(
                    "INVARIANT VIOLATION: Workspace [{}][{}] focused_window {:#x} is iconic",
                    m,
                    w,
                    ws.focused_window
                );
            }
        }
    }
}

/**
 * @brief Assert floating container consistency
 *
 * Verifies:
 * - Every entry in floating_windows exists in clients with Kind::Floating
 * - Every Kind::Floating client is in floating_windows
 */
inline void assert_floating_consistency(
    std::unordered_map<xcb_window_t, Client> const& clients,
    std::vector<xcb_window_t> const& floating_windows
)
{
    for (xcb_window_t fw : floating_windows)
    {
        auto it = clients.find(fw);
        if (it == clients.end())
        {
            LOG_ERROR("INVARIANT VIOLATION: floating window {:#x} not in clients", fw);
        }
        else if (it->second.kind != Client::Kind::Floating)
        {
            LOG_ERROR("INVARIANT VIOLATION: floating window {:#x} has kind {}", fw, static_cast<int>(it->second.kind));
        }
    }

    for (auto const& [id, client] : clients)
    {
        if (client.kind == Client::Kind::Floating && std::ranges::find(floating_windows, id) == floating_windows.end())
        {
            LOG_ERROR("INVARIANT VIOLATION: Floating client {:#x} not in floating_windows", id);
        }
    }
}

/**
 * @brief Assert dock/desktop container consistency
 *
 * Verifies:
 * - Every entry in dock_windows exists in clients with Kind::Dock
 * - Every Kind::Dock client is in dock_windows
 * - Every entry in desktop_windows exists in clients with Kind::Desktop
 * - Every Kind::Desktop client is in desktop_windows
 */
inline void assert_container_consistency(
    std::unordered_map<xcb_window_t, Client> const& clients,
    std::vector<xcb_window_t> const& dock_windows,
    std::vector<xcb_window_t> const& desktop_windows
)
{
    for (xcb_window_t dw : dock_windows)
    {
        auto it = clients.find(dw);
        if (it == clients.end())
        {
            LOG_ERROR("INVARIANT VIOLATION: dock window {:#x} not in clients", dw);
        }
        else if (it->second.kind != Client::Kind::Dock)
        {
            LOG_ERROR("INVARIANT VIOLATION: dock window {:#x} has kind {}", dw, static_cast<int>(it->second.kind));
        }
    }

    for (auto const& [id, client] : clients)
    {
        if (client.kind == Client::Kind::Dock && std::ranges::find(dock_windows, id) == dock_windows.end())
        {
            LOG_ERROR("INVARIANT VIOLATION: Dock client {:#x} not in dock_windows", id);
        }
    }

    for (xcb_window_t dw : desktop_windows)
    {
        auto it = clients.find(dw);
        if (it == clients.end())
        {
            LOG_ERROR("INVARIANT VIOLATION: desktop window {:#x} not in clients", dw);
        }
        else if (it->second.kind != Client::Kind::Desktop)
        {
            LOG_ERROR("INVARIANT VIOLATION: desktop window {:#x} has kind {}", dw, static_cast<int>(it->second.kind));
        }
    }

    for (auto const& [id, client] : clients)
    {
        if (client.kind == Client::Kind::Desktop && std::ranges::find(desktop_windows, id) == desktop_windows.end())
        {
            LOG_ERROR("INVARIANT VIOLATION: Desktop client {:#x} not in desktop_windows", id);
        }
    }
}

#    define LWM_ASSERT_INVARIANTS(clients, monitors, floating, docks, desktops)      \
        do                                                                           \
        {                                                                            \
            lwm::invariants::assert_workspace_consistency(clients, monitors);        \
            lwm::invariants::assert_workspace_focus_valid(clients, monitors);        \
            lwm::invariants::assert_floating_consistency(clients, floating);         \
            lwm::invariants::assert_container_consistency(clients, docks, desktops); \
        } while (0)

#    define LWM_ASSERT_CLIENT_STATE(client) lwm::invariants::assert_client_state_consistency(client)

#    define LWM_ASSERT_FOCUS_CONSISTENCY(clients, active_window) \
        lwm::invariants::assert_focus_consistency(clients, active_window)

#endif // NDEBUG

} // namespace lwm::invariants
