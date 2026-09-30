#include "lwm/core/focus.hpp"
#include <algorithm>
#include <tuple>

namespace lwm::focus {

namespace {

bool newer(Client const& a, Client const& b)
{
    return std::tie(a.mru_order, a.order, a.id) > std::tie(b.mru_order, b.order, b.id);
}

// One fullscreen owner per selection keeps candidate checks linear.
struct Eligibility
{
    State const& state;
    size_t monitor;
    xcb_window_t owner;

    bool operator()(Client const& client) const
    {
        return client.monitor == monitor && State::accepts_focus(client) && !state.showing_desktop()
            && state.visible(client, owner);
    }
};

}

xcb_window_t fallback(State const& state, size_t monitor)
{
    auto const& monitors = state.monitors();
    if (monitor >= monitors.size())
        return XCB_NONE;
    Eligibility eligible{ state, monitor, state.fullscreen_owner(monitor) };
    auto const& current = monitors[monitor].current();
    auto current_tile = [&](xcb_window_t window)
    {
        auto const* client = state.find(window);
        return client && client->kind() == Client::Kind::Tiled && eligible(*client);
    };
    if (current_tile(current.focused_window))
        return current.focused_window;
    for (auto it = current.focus_history.rbegin(); it != current.focus_history.rend(); ++it)
        if (current_tile(*it))
            return *it;
    for (auto it = current.windows.rbegin(); it != current.windows.rend(); ++it)
        if (current_tile(*it))
            return *it;
    // Sticky tiles use reverse workspace order, then reverse membership order.
    auto const& workspaces = monitors[monitor].workspaces;
    for (size_t i = workspaces.size(); i-- > 0;)
    {
        if (i == monitors[monitor].current_workspace)
            continue;
        for (auto it = workspaces[i].windows.rbegin(); it != workspaces[i].windows.rend(); ++it)
            if (current_tile(*it))
                return *it;
    }
    Client const* best = nullptr;
    for (auto const& [id, client] : state.clients())
        if (client.kind() == Client::Kind::Floating && eligible(client) && (!best || newer(client, *best)))
            best = &client;
    return best ? best->id : XCB_NONE;
}

std::vector<xcb_window_t> recent_order(State const& state)
{
    std::vector<Client const*> clients;
    clients.reserve(state.clients().size());
    for (auto const& [id, client] : state.clients()) clients.push_back(&client);
    std::ranges::sort(clients, [](auto const* a, auto const* b) { return newer(*a, *b); });
    std::vector<xcb_window_t> order;
    order.reserve(clients.size());
    for (auto const* client : clients) order.push_back(client->id);
    return order;
}

xcb_window_t
cycle_target(std::span<xcb_window_t const> order, State const& state, size_t monitor, xcb_window_t current, bool forward)
{
    if (order.empty() || monitor >= state.monitors().size())
        return XCB_NONE;
    Eligibility eligible{ state, monitor, state.fullscreen_owner(monitor) };
    auto it = std::ranges::find(order, current);
    size_t index = it == order.end() ? (forward ? order.size() - 1 : 0) : static_cast<size_t>(it - order.begin());
    for (size_t visited = 0; visited < order.size(); ++visited)
    {
        index = forward ? (index + 1) % order.size() : (index + order.size() - 1) % order.size();
        if (auto const* client = state.find(order[index]); client && eligible(*client))
            return client->id;
    }
    return XCB_NONE;
}

std::optional<size_t> monitor_index_at_point(std::span<Monitor const> monitors, int32_t x, int32_t y)
{
    for (size_t i = 0; i < monitors.size(); ++i)
    {
        auto const& monitor = monitors[i];
        if (x >= monitor.x && x < monitor.x + monitor.width && y >= monitor.y && y < monitor.y + monitor.height)
            return i;
    }
    return std::nullopt;
}

} // namespace lwm::focus
