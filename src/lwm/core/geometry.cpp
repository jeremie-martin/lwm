#include "state.hpp"
#include "floating.hpp"
#include <algorithm>
#include <cassert>

namespace lwm {

// Current workspace first, then sticky tiles in workspace order.
template <typename Eligible>
std::vector<xcb_window_t> State::workspace_tiles(size_t monitor, size_t workspace, Eligible eligible) const
{
    auto const& output = monitors_[monitor];
    std::vector<xcb_window_t> windows;
    auto collect = [&](size_t index)
    {
        for (auto window : output.workspaces[index].windows)
        {
            auto const& client = require(window);
            if (index != workspace && !client.sticky)
                continue;
            if (eligible(client))
                windows.push_back(window);
        }
    };
    collect(workspace);
    for (size_t index = 0; index < output.workspaces.size(); ++index)
        if (index != workspace)
            collect(index);
    return windows;
}

std::vector<xcb_window_t> State::tiled_participants(size_t monitor, FullscreenVisibility const& fullscreen) const
{
    return workspace_tiles(
        monitor,
        monitors_[monitor].current_workspace,
        [&](Client const& client) { return !client.fullscreen && visible(client, fullscreen); }
    );
}

std::vector<State::Projected> State::project(FullscreenVisibility const& fullscreen) const
{
    std::vector<Projected> result;
    result.reserve(clients_.size());
    for (size_t m = 0; m < monitors_.size(); ++m)
    {
        auto const& monitor = monitors_[m];
        auto windows = tiled_participants(m, fullscreen);
        auto slots = layout().arrange(windows.size(), monitor.working_area(), monitor.current());
        for (size_t i = 0; i < windows.size(); ++i)
        {
            auto const& client = require(windows[i]);
            result.push_back({ &client, presentation(client, drag_preview(client).value_or(slots[i])) });
        }
    }
    for (auto const& [id, client] : clients_)
    {
        if (!visible(client, fullscreen))
            result.push_back({ &client, std::nullopt });
        else if (client.fullscreen || floating_mode(client))
            result.push_back({ &client, presentation(client) });
    }
    std::ranges::sort(result, { }, [](auto const& projected) { return projected.client->order; });
    return result;
}

State::Presentation State::presentation(Client const& client) const { return presentation(client, frame(client)); }

State::Presentation State::presentation(Client const& client, Geometry frame) const
{
    auto border = client.fullscreen ? 0 : this->border(client, frame);
    return { inset(frame, border), border };
}

uint32_t State::border(Client const& client, Geometry frame) const
{
    return client.borderless ? 0 : std::min(config_.appearance.border_width, (std::min(frame.width, frame.height) - 1U) / 2);
}

// Position-only and unchanged size requests preserve the frame. New window
// sizes use the configured border; omitted sizes retain the current X extent.
Geometry State::resize_frame(Client const& client, Geometry frame, std::optional<uint16_t> width, std::optional<uint16_t> height) const
{
    auto window = inset(frame, border(client, frame));
    if (width.value_or(window.width) == window.width && height.value_or(window.height) == window.height)
        return frame;
    window.width = width.value_or(window.width);
    window.height = height.value_or(window.height);
    return outset(window, border(client));
}

uint32_t State::border_color(Client const& client) const
{
    if (client.id == active_window_)
        return config_.appearance.border_color;
    return client.urgency.active() ? config_.appearance.urgent_border_color : 0;
}

Geometry State::normal_geometry(Client const& client) const
{
    if (auto const* floating = floating_mode(client))
        return floating->geometry;
    auto const& monitor = monitors_[client.monitor];
    size_t workspace = monitor.current_workspace;
    auto windows = tiled_participants(client.monitor, fullscreen_visibility());
    if (std::ranges::find(windows, client.id) == windows.end())
    {
        // Hidden, minimized and fullscreen tiles still have a well-defined normal
        // rectangle: the slot they would occupy when revealed in their workspace
        // without fullscreen occlusion.
        workspace = client.sticky ? workspace : client.workspace;
        windows = workspace_tiles(
            client.monitor, workspace, [&](Client const& tile) { return !tile.iconic || tile.id == client.id; }
        );
    }
    auto position = std::ranges::find(windows, client.id);
    assert(position != windows.end());
    auto slots = layout().arrange(windows.size(), monitor.working_area(), monitor.workspaces[workspace]);
    return slots[static_cast<size_t>(position - windows.begin())];
}

// _NET_WM_FULLSCREEN_MONITORS spans the bounds of its existing edge monitors.
Geometry State::fullscreen_geometry(Client const& client) const
{
    std::vector<Geometry> edges;
    if (auto const& spec = client.fullscreen_monitors)
        for (auto index : { spec->top, spec->bottom, spec->left, spec->right })
            if (index < monitors_.size())
                edges.push_back(monitors_[index].geometry);
    return bounds(edges).value_or(monitors_[client.monitor].geometry);
}

Geometry State::frame(Client const& client) const
{
    if (auto preview = drag_preview(client))
        return *preview;
    if (client.fullscreen)
        return fullscreen_geometry(client);
    if (auto const* floating = floating_mode(client))
        return floating::presentation_geometry(
            floating->geometry, monitors_[client.monitor].working_area(), client.maximized_horz, client.maximized_vert
        );
    return normal_geometry(client);
}

void State::request_geometry(xcb_window_t id, Geometry rectangle)
{
    geometry(id, rectangle);
    auto const& client = require(id);
    if (auto const* floating = floating_mode(client))
        if (auto monitor = monitor_at(monitors_, floating->geometry); monitor && *monitor != client.monitor)
            relocate(id, *monitor, monitors_[*monitor].current_workspace);
}

} // namespace lwm
