#pragma once

#include "log.hpp"
#include "types.hpp"
#include <cstdlib>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace lwm::invariants {

struct Violation
{
    char const* message;
    xcb_window_t window = XCB_NONE;
};

// Check relationships between authoritative records. Client kind/state consistency
// is guaranteed by ClientState; fullscreen and iconic are deliberately independent.
inline std::optional<Violation> validate(
    std::unordered_map<xcb_window_t, Client> const& clients,
    std::vector<Monitor> const& monitors,
    xcb_window_t active_window = XCB_NONE
)
{
    std::unordered_set<xcb_window_t> tiled_windows;
    for (size_t m = 0; m < monitors.size(); ++m)
    {
        auto const& monitor = monitors[m];
        if (monitor.current_workspace >= monitor.workspaces.size()
            || monitor.previous_workspace >= monitor.workspaces.size())
            return Violation{ "Monitor has an invalid current or previous workspace" };

        for (size_t w = 0; w < monitor.workspaces.size(); ++w)
        {
            auto const& workspace = monitor.workspaces[w];
            for (auto window : workspace.windows)
            {
                if (!tiled_windows.insert(window).second)
                    return Violation{ "Window has duplicate tiled membership", window };
                auto it = clients.find(window);
                if (it == clients.end())
                    return Violation{ "Workspace contains an unmanaged window", window };
                auto const& client = it->second;
                if (client.kind() != Client::Kind::Tiled)
                    return Violation{ "Workspace contains a non-tiled client", window };
                if (client.monitor != m || client.workspace != w)
                    return Violation{ "Tiled membership disagrees with client placement", window };
            }

            if (workspace.focused_window != XCB_NONE)
            {
                auto window = workspace.focused_window;
                if (workspace.find_window(window) == workspace.windows.end())
                    return Violation{ "Workspace focus is absent from tiled membership", window };
                if (clients.at(window).iconic)
                    return Violation{ "Workspace focus is iconic", window };
            }
        }
    }

    for (auto const& [id, client] : clients)
    {
        if (id == XCB_NONE || client.id != id)
            return Violation{ "Client id disagrees with registry key", id };
        if (client.kind() != Client::Kind::Tiled && client.kind() != Client::Kind::Floating)
            continue;
        if (client.monitor >= monitors.size() || client.workspace >= monitors[client.monitor].workspaces.size())
            return Violation{ "Client has invalid monitor or workspace placement", id };
        if (client.kind() == Client::Kind::Tiled && !tiled_windows.contains(id))
            return Violation{ "Tiled client is absent from workspace membership", id };
    }
    if (active_window != XCB_NONE)
    {
        auto it = clients.find(active_window);
        if (it == clients.end())
            return Violation{ "Active window is unmanaged", active_window };
        auto const& client = it->second;
        if (client.kind() != Client::Kind::Tiled && client.kind() != Client::Kind::Floating)
            return Violation{ "Active window is a dock or desktop", active_window };
        if (client.iconic || client.hidden)
            return Violation{ "Active window is iconic or hidden", active_window };
    }
    return std::nullopt;
}

} // namespace lwm::invariants

#ifdef NDEBUG
#    define LWM_ASSERT_INVARIANTS(clients, monitors, active_window) ((void)0)
#else
#    define LWM_ASSERT_INVARIANTS(clients, monitors, active_window)                                  \
        do                                                                                           \
        {                                                                                            \
            if (auto violation = lwm::invariants::validate(clients, monitors, active_window))        \
            {                                                                                        \
                LOG_ERROR("INVARIANT VIOLATION: {} ({:#x})", violation->message, violation->window); \
                std::abort();                                                                        \
            }                                                                                        \
        } while (0)
#endif
