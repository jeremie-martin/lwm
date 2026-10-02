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

// Persistent ownership is checked before external observations can filter or
// normalize it. Runtime checks use this same graph, then add live-only facts.
inline std::optional<Violation> validate(restart::Snapshot const& graph)
{
    std::unordered_set<std::string> outputs;
    for (auto const& monitor : graph.monitors)
    {
        if (monitor.name.empty() || !outputs.insert(monitor.name).second || monitor.workspaces.empty()
            || monitor.current_workspace >= monitor.workspaces.size() || monitor.previous_workspace >= monitor.workspaces.size())
            return Violation{ "Monitor identity or workspace selection is invalid" };
        for (auto const& workspace : monitor.workspaces)
            for (auto const& [address, ratio] : workspace.split_ratios)
                if (!(ratio > 0 && ratio < 1))
                    return Violation{ "Workspace ratio is invalid" };
    }
    if (graph.focused_monitor >= std::max<size_t>(1, graph.monitors.size()))
        return Violation{ "Focused monitor is invalid" };
    std::unordered_set<xcb_window_t> registered;
    std::unordered_set<uint64_t> ranks;
    // Leave rank headroom for a whole X11 window-ID space during adoption.
    auto register_window = [&](auto const& window)
    {
        return window.id != XCB_NONE && registered.insert(window.id).second
            && window.order < UINT64_MAX - UINT32_MAX && ranks.insert(window.order).second;
    };
    for (auto const& fixture : graph.fixtures)
        if (!register_window(fixture))
            return Violation{ "Fixture identity or registration rank is invalid or duplicated", fixture.id };
    std::unordered_map<xcb_window_t, ClientIntent const*> clients;
    std::unordered_set<uint64_t> recencies;
    for (auto const& client : graph.clients)
    {
        if (!register_window(client))
            return Violation{ "Client identity or registration rank is invalid or duplicated", client.id };
        if (client.monitor >= graph.monitors.size()
            || client.workspace >= graph.monitors[client.monitor].workspaces.size())
            return Violation{ "Client has invalid monitor or workspace placement", client.id };
        if (client.mru_order == UINT64_MAX || (client.mru_order && !recencies.insert(client.mru_order).second))
            return Violation{ "Client focus recency is invalid or duplicated", client.id };
        if (client.urgency.sources > (static_cast<uint8_t>(UrgencySource::WmInitiated) | static_cast<uint8_t>(UrgencySource::App)))
            return Violation{ "Client urgency sources are invalid", client.id };
        if (auto const* floating = std::get_if<FloatingMode>(&client.mode);
            floating && floating->tile_slot && floating->tile_slot->output.empty())
            return Violation{ "Tile return slot has no output identity", client.id };
        clients.emplace(client.id, &client);
    }
    std::unordered_set<xcb_window_t> tiled;
    for (size_t m = 0; m < graph.monitors.size(); ++m)
        for (size_t w = 0; w < graph.monitors[m].workspaces.size(); ++w)
        {
            auto const& workspace = graph.monitors[m].workspaces[w];
            for (auto id : workspace.windows)
            {
                auto it = clients.find(id);
                if (it == clients.end() || it->second->monitor != m || it->second->workspace != w
                    || !std::holds_alternative<TiledMode>(it->second->mode) || !tiled.insert(id).second)
                    return Violation{ "Tiled membership is missing, duplicated or misplaced", id };
            }
            if (workspace.preferred_tile != XCB_NONE && workspace.find_window(workspace.preferred_tile) == workspace.windows.end())
                return Violation{ "Workspace tile preference is absent from tiled membership", workspace.preferred_tile };
        }
    for (auto const& client : graph.clients)
        if (std::holds_alternative<TiledMode>(client.mode) && !tiled.contains(client.id))
            return Violation{ "Tiled client is absent from workspace membership", client.id };
    std::unordered_set<xcb_window_t> claims;
    for (auto id : graph.fullscreen_claims)
        if (!clients.contains(id) || !claims.insert(id).second)
            return Violation{ "Fullscreen claim is missing or duplicated", id };
    std::unordered_set<std::string> names;
    claims.clear();
    for (auto const& slot : graph.named_scratchpads)
    {
        if (slot.name.empty() || !names.insert(slot.name).second)
            return Violation{ "Named scratchpad identity is missing or duplicated" };
        if (slot.window && (!clients.contains(*slot.window) || !claims.insert(*slot.window).second))
            return Violation{ "Named scratchpad ownership is missing or duplicated", *slot.window };
    }
    for (auto id : graph.pool)
        if (!clients.contains(id) || !claims.insert(id).second)
            return Violation{ "Scratchpad pool ownership is missing or duplicated", id };
    if (graph.active != XCB_NONE && !clients.contains(graph.active))
        return Violation{ "Active window is unmanaged", graph.active };
    return std::nullopt;
}

inline std::optional<Violation> validate(State const& state)
{
    if (auto violation = validate(state.snapshot()))
        return violation;
    std::unordered_set<xcb_window_t> claims(state.fullscreen_claims().begin(), state.fullscreen_claims().end());
    for (auto const& [id, client] : state.clients())
    {
        if (id != client.id)
            return Violation{ "Client registry identity is inconsistent", id };
        if (client.order >= state.next_order())
            return Violation{ "Client registration order is invalid or duplicated", id };
        if (client.mru_order >= state.next_recency())
            return Violation{ "Client focus recency exceeds its bound", id };
        if (client.fullscreen != claims.contains(id)
            || (client.fullscreen && (client.maximized_horz || client.maximized_vert)))
            return Violation{ "Fullscreen state disagrees with its claim or maximize state", id };
        if (client.iconic && state.monitors()[client.monitor].workspaces[client.workspace].preferred_tile == id)
            return Violation{ "Workspace tile preference is iconic", id };
    }
    for (auto const& [id, fixture] : state.fixtures())
        if (id != fixture.id || fixture.order >= state.next_order())
            return Violation{ "Fixture registry identity or order is inconsistent", id };
    if (auto const* active = state.find(state.active_window()); active && !state.focusable(*active))
        return Violation{ "Active window cannot hold focus", active->id };
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
