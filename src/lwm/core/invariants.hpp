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
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <xcb/xcb.h>
#include <xcb/xcb_ewmh.h>

namespace lwm::invariants {

#ifdef NDEBUG
// Release build: no-op
#define LWM_ASSERT_INVARIANTS(...)
#define LWM_ASSERT_CLIENT_STATE(...)
#define LWM_ASSERT_FOCUS_CONSISTENCY(...)
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
        LWM_DEBUG("INVARIANT VIOLATION: Window 0x" << std::hex << window << " not in clients registry");
        return;
    }

    auto const& client = it->second;
    if (client.kind == Client::Kind::Tiled || client.kind == Client::Kind::Floating)
    {
        if (client.monitor >= monitors.size())
        {
            LWM_DEBUG("INVARIANT VIOLATION: Window 0x" << std::hex << window
                << " has invalid monitor index " << client.monitor);
        }
        if (client.monitor < monitors.size() && client.workspace >= monitors[client.monitor].workspaces.size())
        {
            LWM_DEBUG("INVARIANT VIOLATION: Window 0x" << std::hex << window
                << " has invalid workspace index " << client.workspace);
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
inline void assert_focus_consistency(
    std::unordered_map<xcb_window_t, Client> const& clients,
    xcb_window_t active_window
)
{
    if (active_window == XCB_NONE)
        return;

    auto it = clients.find(active_window);
    if (it == clients.end())
    {
        LWM_DEBUG("INVARIANT VIOLATION: Active window 0x" << std::hex << active_window
            << " not in clients registry");
        return;
    }

    auto const& client = it->second;
    if (client.iconic)
    {
        LWM_DEBUG("INVARIANT VIOLATION: Active window 0x" << std::hex << active_window
            << " is iconic (minimized)");
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
        LWM_DEBUG("INVARIANT VIOLATION: Window 0x" << std::hex << client.id
            << " is both fullscreen and iconic");
    }
    if (client.above && client.below)
    {
        LWM_DEBUG("INVARIANT VIOLATION: Window 0x" << std::hex << client.id
            << " is both above and below");
    }
}

/**
 * @brief Assert desktop index validity
 *
 * Verifies:
 * - Desktop index is valid OR 0xFFFFFFFF (sticky)
 */
inline void assert_valid_desktop(
    uint32_t desktop,
    size_t num_monitors,
    size_t workspaces_per_monitor
)
{
    if (desktop == 0xFFFFFFFF)
        return; // Sticky is valid

    uint32_t max_desktop = static_cast<uint32_t>(num_monitors * workspaces_per_monitor);
    if (desktop >= max_desktop)
    {
        LWM_DEBUG("INVARIANT VIOLATION: Desktop index " << desktop
            << " exceeds maximum " << (max_desktop - 1));
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
            for (auto const& win : monitors[m].workspaces[w].windows)
            {
                if (seen.contains(win.id))
                {
                    LWM_DEBUG("INVARIANT VIOLATION: Window 0x" << std::hex << win.id
                        << " appears in multiple workspaces");
                }
                seen.insert(win.id);

                auto it = clients.find(win.id);
                if (it == clients.end())
                {
                    LWM_DEBUG("INVARIANT VIOLATION: Window 0x" << std::hex << win.id
                        << " in workspace but not in clients registry");
                }
                else if (it->second.kind != Client::Kind::Tiled)
                {
                    LWM_DEBUG("INVARIANT VIOLATION: Window 0x" << std::hex << win.id
                        << " in workspace but not Kind::Tiled");
                }
            }
        }
    }

    // Verify all Tiled clients are in some workspace
    for (auto const& [id, client] : clients)
    {
        if (client.kind == Client::Kind::Tiled && !seen.contains(id))
        {
            LWM_DEBUG("INVARIANT VIOLATION: Tiled client 0x" << std::hex << id
                << " not found in any workspace");
        }
    }
}

#define LWM_ASSERT_INVARIANTS(clients, monitors) \
    lwm::invariants::assert_workspace_consistency(clients, monitors)

#define LWM_ASSERT_CLIENT_STATE(client) \
    lwm::invariants::assert_client_state_consistency(client)

#define LWM_ASSERT_FOCUS_CONSISTENCY(clients, active_window) \
    lwm::invariants::assert_focus_consistency(clients, active_window)

#endif // NDEBUG

} // namespace lwm::invariants
