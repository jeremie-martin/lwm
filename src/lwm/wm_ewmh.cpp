#include "lwm/core/log.hpp"
#include "lwm/core/policy.hpp"
#include "wm.hpp"
#include <algorithm>
#include <limits>

namespace lwm {

void WindowManager::setup_ewmh()
{
    ewmh_.init_atoms();
    ewmh_.set_wm_name("lwm");

    update_ewmh_desktops();
    update_ewmh_current_desktop();
}

void WindowManager::update_ewmh_desktops()
{
    int32_t min_x = 0;
    int32_t min_y = 0;
    int32_t max_x = 0;
    int32_t max_y = 0;
    if (!monitors_.empty())
    {
        min_x = monitors_[0].x;
        min_y = monitors_[0].y;
        max_x = static_cast<int32_t>(monitors_[0].x) + monitors_[0].width;
        max_y = static_cast<int32_t>(monitors_[0].y) + monitors_[0].height;
        for (auto const& monitor : monitors_)
        {
            min_x = std::min<int32_t>(min_x, monitor.x);
            min_y = std::min<int32_t>(min_y, monitor.y);
            max_x = std::max<int32_t>(max_x, static_cast<int32_t>(monitor.x) + monitor.width);
            max_y = std::max<int32_t>(max_y, static_cast<int32_t>(monitor.y) + monitor.height);
        }
    }

    desktop_origin_x_ = min_x;
    desktop_origin_y_ = min_y;

    uint32_t desktop_width = static_cast<uint32_t>(std::max<int32_t>(1, max_x - min_x));
    uint32_t desktop_height = static_cast<uint32_t>(std::max<int32_t>(1, max_y - min_y));
    ewmh_.set_desktop_geometry(desktop_width, desktop_height);

    size_t workspaces_per_monitor = config_.workspaces.count;
    uint32_t total_desktops = static_cast<uint32_t>(monitors_.size() * workspaces_per_monitor);
    ewmh_.set_number_of_desktops(total_desktops);

    std::vector<std::string> names;
    names.reserve(total_desktops);
    for (size_t m = 0; m < monitors_.size(); ++m)
    {
        for (size_t w = 0; w < workspaces_per_monitor; ++w)
        {
            if (w < config_.workspaces.names.size())
            {
                names.push_back(config_.workspaces.names[w]);
            }
            else
            {
                names.push_back(std::to_string(w + 1));
            }
        }
    }
    ewmh_.set_desktop_names(names);
    ewmh_.set_desktop_viewport(monitors_, desktop_origin_x_, desktop_origin_y_);
    update_ewmh_workarea();
}

void WindowManager::update_ewmh_client_list()
{
    // Build client list sorted by mapping order using Client registry
    std::vector<std::pair<uint64_t, xcb_window_t>> ordered;
    ordered.reserve(clients_.size());
    for (auto const& [window, client] : clients_)
    {
        ordered.push_back({ client.order, window });
    }
    std::sort(ordered.begin(), ordered.end(), [](auto const& a, auto const& b) { return a.first < b.first; });

    std::vector<xcb_window_t> windows;
    windows.reserve(ordered.size());
    for (auto const& entry : ordered)
    {
        windows.push_back(entry.second);
    }
    ewmh_.update_client_list(windows);

    // Build stacking order from X server
    std::vector<xcb_window_t> stacking;
    stacking.reserve(windows.size());
    std::unordered_set<xcb_window_t> managed(windows.begin(), windows.end());

    auto cookie = xcb_query_tree(conn_.get(), conn_.screen()->root);
    auto* reply = xcb_query_tree_reply(conn_.get(), cookie, nullptr);
    if (reply)
    {
        int length = xcb_query_tree_children_length(reply);
        xcb_window_t* children = xcb_query_tree_children(reply);
        for (int i = 0; i < length; ++i)
        {
            if (managed.contains(children[i]))
            {
                stacking.push_back(children[i]);
                managed.erase(children[i]);
            }
        }
        free(reply);
    }

    // Append any windows not found in X tree (shouldn't happen, but be safe)
    if (!managed.empty())
    {
        for (auto const& entry : ordered)
        {
            if (managed.contains(entry.second))
                stacking.push_back(entry.second);
        }
    }

    ewmh_.update_client_list_stacking(stacking);
}

void WindowManager::update_ewmh_current_desktop()
{
    // Per-monitor workspaces: report the active monitor's current workspace only.
    uint32_t desktop = get_ewmh_desktop_index(focused_monitor_, focused_monitor().current_workspace);
    LOG_TRACE(
        "update_ewmh_current_desktop: focused_monitor_={} current_ws={} desktop={}",
        focused_monitor_,
        focused_monitor().current_workspace,
        desktop
    );
    ewmh_.set_current_desktop(desktop);
}

void WindowManager::update_ewmh_workarea()
{
    std::vector<Geometry> workareas;
    workareas.reserve(monitors_.size() * config_.workspaces.count);
    for (auto const& monitor : monitors_)
    {
        Geometry area = monitor.working_area();
        if (bar_)
        {
            uint32_t bar_height = config_.appearance.status_bar_height;
            if (area.height > bar_height)
            {
                area.y = static_cast<int16_t>(area.y + bar_height);
                area.height = static_cast<uint16_t>(area.height - bar_height);
            }
            else
            {
                area.height = 1;
            }
        }
        int32_t offset_x = static_cast<int32_t>(area.x) - desktop_origin_x_;
        int32_t offset_y = static_cast<int32_t>(area.y) - desktop_origin_y_;
        offset_x = std::clamp<int32_t>(offset_x, 0, std::numeric_limits<int16_t>::max());
        offset_y = std::clamp<int32_t>(offset_y, 0, std::numeric_limits<int16_t>::max());
        area.x = static_cast<int16_t>(offset_x);
        area.y = static_cast<int16_t>(offset_y);
        for (size_t i = 0; i < config_.workspaces.count; ++i)
        {
            workareas.push_back(area);
        }
    }
    ewmh_.set_workarea(workareas);
}

uint32_t WindowManager::get_ewmh_desktop_index(size_t monitor_idx, size_t workspace_idx) const
{
    // Desktop index = monitor_idx * workspaces_per_monitor + workspace_idx
    return ewmh_policy::desktop_index(monitor_idx, workspace_idx, config_.workspaces.count);
}

void WindowManager::switch_to_ewmh_desktop(uint32_t desktop)
{
    LOG_DEBUG("switch_to_ewmh_desktop({}) called, focused_monitor_={}", desktop, focused_monitor_);

    // Convert EWMH desktop index to monitor + workspace
    size_t workspaces_per_monitor = config_.workspaces.count;
    auto indices = ewmh_policy::desktop_to_indices(desktop, workspaces_per_monitor);
    if (!indices)
    {
        LOG_TRACE("switch_to_ewmh_desktop: invalid desktop index");
        return;
    }
    size_t monitor_idx = indices->first;
    size_t workspace_idx = indices->second;

    LOG_TRACE("switch_to_ewmh_desktop: target monitor_idx={} workspace_idx={}", monitor_idx, workspace_idx);

    if (monitor_idx >= monitors_.size())
    {
        LOG_TRACE("switch_to_ewmh_desktop: invalid monitor index");
        return;
    }

    auto& monitor = monitors_[monitor_idx];
    if (workspace_idx >= monitor.workspaces.size())
    {
        LOG_TRACE("switch_to_ewmh_desktop: invalid workspace index");
        return;
    }

    // Early return if already on target monitor and workspace (matches switch_workspace behavior)
    if (monitor_idx == focused_monitor_ && workspace_idx == monitor.current_workspace)
    {
        LOG_TRACE("switch_to_ewmh_desktop: already on target, returning");
        return;
    }

    size_t old_workspace = monitor.current_workspace;
    LOG_DEBUG(
        "switch_to_ewmh_desktop: switching from ws {} to ws {} on monitor {}",
        old_workspace,
        workspace_idx,
        monitor_idx
    );

    // Unmap floating windows from old workspace FIRST
    // This prevents visual glitches where old floating windows appear over new workspace content
    for (auto& fw : floating_windows_)
    {
        auto const* client = get_client(fw.id);
        if (!client || client->monitor != monitor_idx)
            continue;
        if (is_client_sticky(fw.id))
            continue;
        if (client->workspace == old_workspace)
        {
            LOG_TRACE("switch_to_ewmh_desktop: pre-unmapping floating {:#x}", fw.id);
            hide_window(fw.id);
        }
    }

    // Unmap tiled windows from OLD workspace before switching
    for (xcb_window_t window : monitor.current().windows)
    {
        if (is_client_sticky(window))
            continue;
        LOG_TRACE("switch_to_ewmh_desktop: unmapping window {:#x}", window);
        hide_window(window);
    }
    // Flush unmaps before rearranging to ensure old windows are hidden
    conn_.flush();

    // Switch to target monitor and workspace
    focused_monitor_ = monitor_idx;
    if (workspace_idx != old_workspace)
    {
        monitor.previous_workspace = old_workspace;
    }
    monitor.current_workspace = workspace_idx;
    LOG_TRACE("switch_to_ewmh_desktop: updating EWMH current desktop");
    update_ewmh_current_desktop();
    LOG_TRACE("switch_to_ewmh_desktop: rearranging monitor");
    rearrange_monitor(monitor);
    LOG_TRACE("switch_to_ewmh_desktop: updating floating visibility");
    update_floating_visibility(monitor_idx);
    LOG_TRACE("switch_to_ewmh_desktop: focus_or_fallback");
    focus_or_fallback(monitor);
    LOG_TRACE("switch_to_ewmh_desktop: update_all_bars");
    update_all_bars();
    conn_.flush();
    LOG_DEBUG("switch_to_ewmh_desktop: DONE, now on monitor {} ws {}", focused_monitor_, monitor.current_workspace);
}

} // namespace lwm
