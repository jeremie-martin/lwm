#include "lwm/core/floating.hpp"
#include "lwm/core/log.hpp"
#include "lwm/core/policy.hpp"
#include "wm.hpp"
#include <algorithm>
#include <cassert>

namespace lwm {

void WindowManager::assign_window_workspace(Client& client, size_t monitor_idx, size_t workspace_idx)
{
    if (monitor_idx >= monitors_.size() || workspace_idx >= monitors_[monitor_idx].workspaces.size())
    {
        LWM_LOG_WARN("assign_window_workspace: invalid indices monitor={} workspace={}", monitor_idx, workspace_idx);
        return;
    }

    client.monitor = monitor_idx;
    client.workspace = workspace_idx;
    effects_.desktops.insert(client.id);
}

void WindowManager::attach_tile(Client const& client, std::optional<size_t> index)
{
    auto& windows = monitors_[client.monitor].workspaces[client.workspace].windows;
    auto position = std::min(index.value_or(windows.size()), windows.size());
    windows.insert(windows.begin() + static_cast<std::ptrdiff_t>(position), client.id);
}

std::optional<SavedTilePos> WindowManager::detach_tile(Client const& client)
{
    auto& ws = monitors_[client.monitor].workspaces[client.workspace];
    auto it = ws.find_window(client.id);
    if (it == ws.windows.end())
        return std::nullopt;
    SavedTilePos position{ static_cast<size_t>(it - ws.windows.begin()), client.monitor, client.workspace };
    ws.windows.erase(it);
    workspace_policy::remove_from_focus_history(ws, client.id);
    workspace_policy::fixup_workspace_focus(ws, client.id, [this](xcb_window_t id) { return window_is_iconic(id); });
    return position;
}

bool WindowManager::relocate_client(
    Client& client,
    size_t monitor,
    size_t workspace,
    RelocationGeometry geometry,
    std::optional<size_t> tile_index
)
{
    if (client.kind() != Client::Kind::Tiled && client.kind() != Client::Kind::Floating)
        return false;
    if (monitor >= monitors_.size() || workspace >= monitors_[monitor].workspaces.size())
        return false;
    bool tiled = client.kind() == Client::Kind::Tiled;
    size_t source = client.monitor;
    if (source == monitor && client.workspace == workspace)
    {
        if (!tiled || !tile_index)
            return true;
        auto& windows = monitors_[monitor].workspaces[workspace].windows;
        auto from = std::ranges::find(windows, client.id);
        if (from == windows.end())
            return false;
        auto target = windows.begin() + static_cast<std::ptrdiff_t>(std::min(*tile_index, windows.size() - 1));
        // Reordering does not remove membership or discard remembered focus.
        if (from < target)
            std::rotate(from, from + 1, target + 1);
        else if (target < from)
            std::rotate(target, from, from + 1);
        else
            return true;
        invalidate_monitor(monitor);
        return true;
    }
    if (tiled && !detach_tile(client))
        return false;
    if (!tiled && source != monitor && geometry == RelocationGeometry::CenterOnMonitorChange)
    {
        auto& rectangle = floating_geometry(client);
        rectangle = floating::place_floating(
            monitors_[monitor].working_area(),
            rectangle.width,
            rectangle.height,
            std::nullopt
        );
    }
    assign_window_workspace(client, monitor, workspace);
    if (tiled)
        attach_tile(client, tile_index);
    else
        request_geometry(client);
    invalidate_monitor(source);
    invalidate_monitor(monitor);
    return true;
}

void WindowManager::change_client_state(Client& client, ClientState state, std::optional<size_t> tile_index)
{
    assert(client.kind() == Client::Kind::Tiled || client.kind() == Client::Kind::Floating);
    assert(std::holds_alternative<TiledState>(state) || std::holds_alternative<FloatingState>(state));
    bool was_tiled = client.kind() == Client::Kind::Tiled;
    bool tiled = std::holds_alternative<TiledState>(state);
    if (was_tiled && !tiled)
        std::get<FloatingState>(state).saved_tiled_pos = detach_tile(client);
    client.state = std::move(state);
    if (!was_tiled && tiled)
        attach_tile(client, tile_index);
    client.suppress_next_configure_request = false;
    request_allowed_actions(client);
    request_geometry(client);
    invalidate_monitor(client.monitor);
}

} // namespace lwm
