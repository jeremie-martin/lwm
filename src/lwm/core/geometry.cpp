#include "state.hpp"
#include "floating.hpp"
#include <algorithm>
#include <cassert>
#include <limits>

namespace lwm {

// Current workspace first, then sticky tiles in workspace order. A prospective
// view reveals one tile in its normal workspace, without fullscreen occlusion.
std::vector<xcb_window_t> State::workspace_tiles(
    size_t monitor, size_t workspace, FullscreenVisibility const* fullscreen, xcb_window_t include
) const
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
            if (fullscreen ? !client.fullscreen && visible(client, *fullscreen) : !client.iconic || window == include)
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
    return workspace_tiles(monitor, monitors_[monitor].current_workspace, &fullscreen);
}

std::vector<State::Projected> State::project(FullscreenVisibility const& fullscreen) const
{
    std::vector<Projected> result;
    result.reserve(clients_.size());
    for (size_t m = 0; m < monitors_.size(); ++m)
    {
        auto const& monitor = monitors_[m];
        auto const& workspace = monitor.current();
        auto windows = tiled_participants(m, fullscreen);
        auto slots = layout_.arrange(windows.size(), monitor.working_area(), workspace.layout_strategy, workspace.split_ratios);
        for (size_t i = 0; i < windows.size(); ++i) result.push_back({ &require(windows[i]), slots[i] });
    }
    for (auto const& [id, client] : clients_)
    {
        if (!visible(client, fullscreen))
            result.push_back({ &client, std::nullopt });
        else if (client.fullscreen || floating_mode(client))
            result.push_back({ &client, presentation_geometry(client) });
    }
    std::ranges::sort(result, { }, [](auto const& projected) { return projected.client->order; });
    return result;
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
        // Hidden, minimized and fullscreen tiles still have a well-defined
        // normal rectangle: the slot they would occupy when revealed normally.
        workspace = client.sticky ? workspace : client.workspace;
        windows = workspace_tiles(client.monitor, workspace, nullptr, client.id);
    }
    auto position = std::ranges::find(windows, client.id);
    assert(position != windows.end());
    auto const& policy = monitor.workspaces[workspace];
    auto slots = layout_.arrange(windows.size(), monitor.working_area(), policy.layout_strategy, policy.split_ratios);
    return slots[static_cast<size_t>(position - windows.begin())];
}

Geometry State::fullscreen_geometry(Client const& client) const
{
    auto const& monitors = monitors_;
    Geometry area = monitors[client.monitor].geometry;
    if (!client.fullscreen_monitors)
        return area;
    auto const& spec = *client.fullscreen_monitors;
    int32_t min_x = std::numeric_limits<int32_t>::max(), min_y = min_x;
    int32_t max_x = std::numeric_limits<int32_t>::min(), max_y = max_x;
    for (auto index : { spec.top, spec.bottom, spec.left, spec.right })
    {
        if (index >= monitors.size())
            continue;
        auto const& m = monitors[index];
        min_x = std::min<int32_t>(min_x, m.geometry.x);
        min_y = std::min<int32_t>(min_y, m.geometry.y);
        max_x = std::max<int32_t>(max_x, m.geometry.x + m.geometry.width);
        max_y = std::max<int32_t>(max_y, m.geometry.y + m.geometry.height);
    }
    if (min_x > max_x)
        return area;
    return { geometry_coordinate(min_x), geometry_coordinate(min_y), geometry_extent(max_x - min_x), geometry_extent(max_y - min_y) };
}

Geometry State::presentation_geometry(Client const& client) const
{
    if (client.fullscreen)
        return fullscreen_geometry(client);
    if (auto const* floating = floating_mode(client))
        return floating::presentation_geometry(
            floating->geometry, monitors_[client.monitor].working_area(), client.maximized_horz, client.maximized_vert
        );
    return normal_geometry(client);
}

} // namespace lwm
