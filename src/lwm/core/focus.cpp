#include "lwm/core/focus.hpp"
#include "lwm/core/log.hpp"
#include <algorithm>
#include <tuple>

namespace lwm::focus {

namespace {
bool newer(Client const& a, Client const& b)
{
    return std::tie(a.mru_order, a.order, a.id) > std::tie(b.mru_order, b.order, b.id);
}
}

bool accepts_focus(Client const& client)
{
    return (client.kind() == Client::Kind::Tiled || client.kind() == Client::Kind::Floating)
        && (client.accepts_input || client.supports_take_focus);
}

bool eligible(Client const& client, Context const& context)
{
    return accepts_focus(client) && !context.showing_desktop && !client.iconic && client.monitor == context.monitor
        && (client.sticky || client.workspace == context.workspace)
        && (context.fullscreen_owner == XCB_NONE || context.fullscreen_owner == client.id
            || context.fullscreen_owner == client.transient_for);
}

xcb_window_t fallback(Clients const& clients, Monitor const& monitor, Context const& context)
{
    auto current_tile = [&](xcb_window_t window)
    {
        auto it = clients.find(window);
        return it != clients.end() && it->second.kind() == Client::Kind::Tiled
            && it->second.workspace == context.workspace && eligible(it->second, context);
    };
    auto const& workspace = monitor.workspaces[context.workspace];
    if (current_tile(workspace.focused_window))
        return workspace.focused_window;
    for (auto it = workspace.focus_history.rbegin(); it != workspace.focus_history.rend(); ++it)
        if (current_tile(*it))
            return *it;
    for (auto it = workspace.windows.rbegin(); it != workspace.windows.rend(); ++it)
        if (current_tile(*it))
            return *it;
    // Preserve the existing reverse workspace/membership priority for sticky tiles.
    for (size_t i = monitor.workspaces.size(); i-- > 0;)
    {
        if (i == context.workspace)
            continue;
        auto const& windows = monitor.workspaces[i].windows;
        for (auto it = windows.rbegin(); it != windows.rend(); ++it)
            if (auto client = clients.find(*it); client != clients.end() && client->second.kind() == Client::Kind::Tiled
                && client->second.sticky && eligible(client->second, context))
                return *it;
    }
    Client const* best = nullptr;
    for (auto const& [id, client] : clients)
        if (client.kind() == Client::Kind::Floating && eligible(client, context) && (!best || newer(client, *best)))
            best = &client;
    return best ? best->id : XCB_NONE;
}

std::vector<xcb_window_t> recent_order(Clients const& clients)
{
    std::vector<xcb_window_t> order;
    order.reserve(clients.size());
    // Retain only ordering, not eligibility. Existing windows can become eligible
    // during a traversal; new registrations start a fresh traversal in the WM.
    for (auto const& [id, client] : clients) order.push_back(id);
    std::sort(order.begin(), order.end(), [&](auto a, auto b) { return newer(clients.at(a), clients.at(b)); });
    return order;
}

xcb_window_t cycle_target(
    std::span<xcb_window_t const> order,
    Clients const& clients,
    Context const& context,
    xcb_window_t current,
    bool forward
)
{
    if (order.empty())
        return XCB_NONE;
    auto it = std::ranges::find(order, current);
    size_t index = it == order.end() ? (forward ? order.size() - 1 : 0) : static_cast<size_t>(it - order.begin());
    for (size_t visited = 0; visited < order.size(); ++visited)
    {
        index = forward ? (index + 1) % order.size() : (index + order.size() - 1) % order.size();
        auto found = clients.find(order[index]);
        if (found != clients.end() && eligible(found->second, context))
            return found->first;
    }
    return XCB_NONE;
}

std::optional<size_t> monitor_index_at_point(std::span<Monitor const> monitors, int32_t x, int32_t y)
{
    for (size_t i = 0; i < monitors.size(); ++i)
    {
        auto const& monitor = monitors[i];
        if (x >= monitor.x && x < monitor.x + monitor.width && y >= monitor.y && y < monitor.y + monitor.height)
        {
            return i;
        }
    }
    return std::nullopt;
}

PointerFocusResult pointer_move(std::span<Monitor const> monitors, size_t active_monitor, int16_t x, int16_t y)
{
    PointerFocusResult result;
    result.new_monitor = active_monitor;

    auto new_monitor = monitor_index_at_point(monitors, x, y);
    if (!new_monitor)
        return result;

    if (*new_monitor == active_monitor)
        return result;

    result.transition = PointerTransition::MonitorChangedClearFocus;
    result.new_monitor = *new_monitor;
    LWM_LOG_TRACE("pointer_move: monitor changed from {} to {}", active_monitor, *new_monitor);
    return result;
}

} // namespace lwm::focus
