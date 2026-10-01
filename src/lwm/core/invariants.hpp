#pragma once

#include "log.hpp"
#include "state.hpp"
#include <cstdlib>
#include <optional>
#include <unordered_set>

namespace lwm::invariants {

struct Violation
{
    char const* message;
    xcb_window_t window = XCB_NONE;
};

// Relationships the types cannot express. Client mode and fixture roles are
// consistent by construction; visibility and fullscreen ownership are derived.
inline std::optional<Violation> validate(State const& state)
{
    auto const& clients = state.clients();
    auto const& monitors = state.monitors();
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
                auto const* client = state.find(window);
                if (!client)
                    return Violation{ "Workspace contains an unmanaged window", window };
                if (client->kind() != Client::Kind::Tiled)
                    return Violation{ "Workspace contains a non-tiled client", window };
                if (client->monitor != m || client->workspace != w)
                    return Violation{ "Tiled membership disagrees with client placement", window };
            }
            if (auto window = workspace.preferred_tile; window != XCB_NONE)
            {
                if (workspace.find_window(window) == workspace.windows.end())
                    return Violation{ "Workspace tile preference is absent from tiled membership", window };
                if (clients.at(window).iconic)
                    return Violation{ "Workspace tile preference is iconic", window };
            }
        }
    }

    std::unordered_set<uint64_t> registrations;
    std::unordered_set<uint64_t> recencies;
    for (auto const& [id, client] : clients)
    {
        if (client.mru_order >= state.next_recency()
            || (client.mru_order && !recencies.insert(client.mru_order).second))
            return Violation{ "Client focus recency is invalid or duplicated", id };
        if (client.order >= state.next_order() || !registrations.insert(client.order).second)
            return Violation{ "Client registration order is invalid or duplicated", id };
        if (id == XCB_NONE || client.id != id)
            return Violation{ "Client id disagrees with registry key", id };
        if (state.find_fixture(id))
            return Violation{ "Window is both a client and a fixture", id };
        if (client.monitor >= monitors.size() || client.workspace >= monitors[client.monitor].workspaces.size())
            return Violation{ "Client has invalid monitor or workspace placement", id };
        if (client.kind() == Client::Kind::Tiled && !tiled_windows.contains(id))
            return Violation{ "Tiled client is absent from workspace membership", id };
        if (client.fullscreen != (client.fullscreen_claim != 0))
            return Violation{ "Fullscreen state disagrees with its claim", id };
        if (client.fullscreen && (client.maximized_horz || client.maximized_vert))
            return Violation{ "Fullscreen client retains maximize state", id };
    }
    for (auto const& [id, fixture] : state.fixtures())
    {
        if (id == XCB_NONE || fixture.id != id)
            return Violation{ "Fixture id disagrees with registry key", id };
        if (fixture.order >= state.next_order() || !registrations.insert(fixture.order).second)
            return Violation{ "Fixture registration order is invalid or duplicated", id };
    }

    std::unordered_set<xcb_window_t> scratchpads;
    for (auto const& slot : state.named_scratchpads())
        if (auto window = slot.window(); window != XCB_NONE)
        {
            if (!state.find(window))
                return Violation{ "Named scratchpad claims an unmanaged window", window };
            if (!scratchpads.insert(window).second)
                return Violation{ "Window has more than one scratchpad claim", window };
        }
    for (auto window : state.scratchpad_pool())
    {
        if (!state.find(window))
            return Violation{ "Scratchpad pool contains an unmanaged window", window };
        if (!scratchpads.insert(window).second)
            return Violation{ "Window has more than one scratchpad claim", window };
    }

    if (auto active = state.active_window(); active != XCB_NONE)
    {
        auto const* client = state.find(active);
        if (!client)
            return Violation{ "Active window is unmanaged", active };
        if (!state.focusable(*client))
            return Violation{ "Active window cannot hold focus", active };
    }
    return std::nullopt;
}

} // namespace lwm::invariants

#ifdef NDEBUG
#    define LWM_ASSERT_INVARIANTS(state) ((void)0)
#else
#    define LWM_ASSERT_INVARIANTS(state)                                                                 \
        do                                                                                               \
        {                                                                                                \
            if (auto violation = lwm::invariants::validate(state))                                      \
            {                                                                                            \
                LWM_LOG_ERROR("INVARIANT VIOLATION: {} ({:#x})", violation->message, violation->window); \
                std::abort();                                                                            \
            }                                                                                            \
        } while (0)
#endif
