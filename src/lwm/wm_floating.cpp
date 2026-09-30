#include "lwm/core/floating.hpp"
#include "lwm/core/focus.hpp"
#include "wm.hpp"
#include <algorithm>
#include <xcb/xcb_icccm.h>

namespace lwm {

WindowManager::FloatingPlacement WindowManager::initial_floating_placement(
    xcb_window_t window,
    ClassificationResult const& initial,
    DesktopResolutionResult const& desktop_target
)
{
    auto transient = initial.transient_for != XCB_NONE ? std::optional{ initial.transient_for } : std::nullopt;
    std::optional<size_t> monitor_idx;
    std::optional<size_t> workspace_idx;
    std::optional<Geometry> parent_geom;

    if (transient)
    {
        if (auto const* parent = get_client(*transient);
            parent && (parent->kind() == Client::Kind::Tiled || parent->kind() == Client::Kind::Floating))
        {
            monitor_idx = parent->monitor;
            workspace_idx = parent->workspace;
        }

        auto geom_cookie = xcb_get_geometry(conn_.get(), *transient);
        auto* geom_reply = xcb_get_geometry_reply(conn_.get(), geom_cookie, nullptr);
        if (geom_reply)
        {
            parent_geom = Geometry{ geom_reply->x, geom_reply->y, geom_reply->width, geom_reply->height };
            free(geom_reply);
        }
    }

    bool desktop_pinned = desktop_target.kind == WindowManager::DesktopResolution::Resolved;
    if (!monitor_idx || !workspace_idx)
    {
        if (desktop_target.kind == WindowManager::DesktopResolution::OutOfRange)
            LWM_LOG_WARN("manage_floating_window({:#x}): _NET_WM_DESKTOP out of range, ignoring hint", window);
        else if (desktop_pinned)
        {
            monitor_idx = desktop_target.monitor;
            workspace_idx = desktop_target.workspace;
        }
    }

    if (!monitor_idx)
        monitor_idx = focused_monitor_;
    if (!workspace_idx)
        workspace_idx = monitors_[*monitor_idx].current_workspace;

    xcb_size_hints_t size_hints;
    bool has_hints = xcb_icccm_get_wm_normal_hints_reply(
        conn_.get(),
        xcb_icccm_get_wm_normal_hints(conn_.get(), window),
        &size_hints,
        nullptr
    );
    bool has_position_hint = false;
    bool has_size_hint = false;
    int16_t hinted_x = 0;
    int16_t hinted_y = 0;
    uint32_t hinted_width = 0;
    uint32_t hinted_height = 0;
    if (has_hints)
    {
        // Respect user-specified position (US_POSITION) always.
        // For program-specified position (P_POSITION), only use it for non-transient
        // windows — transient windows (dialogs, file pickers) should center on their
        // parent rather than land at whatever fixed coordinate the app requested.
        bool dominated_by_parent = transient.has_value();
        bool user_placed = (size_hints.flags & XCB_ICCCM_SIZE_HINT_US_POSITION) != 0;
        bool program_placed = (size_hints.flags & XCB_ICCCM_SIZE_HINT_P_POSITION) != 0;
        if (user_placed || (program_placed && !dominated_by_parent))
        {
            has_position_hint = true;
            hinted_x = geometry_coordinate(size_hints.x);
            hinted_y = geometry_coordinate(size_hints.y);
        }
        if (size_hints.flags & (XCB_ICCCM_SIZE_HINT_US_SIZE | XCB_ICCCM_SIZE_HINT_P_SIZE))
        {
            has_size_hint = true;
            hinted_width = size_hints.width > 0 ? geometry_extent(size_hints.width) : 0;
            hinted_height = size_hints.height > 0 ? geometry_extent(size_hints.height) : 0;
        }
    }

    uint32_t width = 300;
    uint32_t height = 200;
    auto geom_cookie = xcb_get_geometry(conn_.get(), window);
    auto* geom_reply = xcb_get_geometry_reply(conn_.get(), geom_cookie, nullptr);
    if (geom_reply)
    {
        width = geom_reply->width;
        height = geom_reply->height;
        free(geom_reply);
    }
    if (has_size_hint)
    {
        if (hinted_width > 0)
            width = hinted_width;
        if (hinted_height > 0)
            height = hinted_height;
    }
    if (width == 0)
        width = 300;
    if (height == 0)
        height = 200;
    width = std::max<uint32_t>(1, width);
    height = std::max<uint32_t>(1, height);

    // A position hint may legitimately target a different monitor than the one
    // chosen above (e.g. `xterm -geometry +2400+100` while another monitor is
    // focused). For windows not anchored elsewhere — no transient parent, no
    // _NET_WM_DESKTOP — the hint decides the monitor, so the guard below only
    // rejects hints that land on no monitor at all.
    if (has_position_hint && !transient && !desktop_pinned)
    {
        int32_t center_x = static_cast<int32_t>(hinted_x) + static_cast<int32_t>(width) / 2;
        int32_t center_y = static_cast<int32_t>(hinted_y) + static_cast<int32_t>(height) / 2;
        if (auto hinted_monitor = focus::monitor_index_at_point(monitors_, center_x, center_y))
        {
            monitor_idx = *hinted_monitor;
            workspace_idx = monitors_[*hinted_monitor].current_workspace;
        }
    }

    Geometry placement;
    if (has_position_hint
        && floating::hint_targets_monitor(
            monitors_[*monitor_idx].geometry(),
            hinted_x,
            hinted_y,
            static_cast<uint16_t>(width),
            static_cast<uint16_t>(height)
        ))
    {
        placement.x = hinted_x;
        placement.y = hinted_y;
        placement.width = static_cast<uint16_t>(width);
        placement.height = static_cast<uint16_t>(height);
    }
    else
    {
        placement = floating::place_floating(
            monitors_[*monitor_idx].working_area(),
            static_cast<uint16_t>(width),
            static_cast<uint16_t>(height),
            parent_geom
        );
    }

    return { placement, *monitor_idx, *workspace_idx, desktop_pinned };
}

bool WindowManager::is_floating_window(xcb_window_t window) const
{
    auto const* client = get_client(window);
    return client && client->kind() == Client::Kind::Floating;
}

void WindowManager::update_floating_monitor_for_geometry(Client& client)
{
    update_floating_monitor_for_geometry(client, floating_geometry(client));
}

void WindowManager::update_floating_monitor_for_geometry(Client& client, Geometry const& geom)
{
    int32_t center_x = static_cast<int32_t>(geom.x) + static_cast<int32_t>(geom.width) / 2;
    int32_t center_y = static_cast<int32_t>(geom.y) + static_cast<int32_t>(geom.height) / 2;
    auto new_monitor = focus::monitor_index_at_point(monitors_, center_x, center_y);
    if (!new_monitor || *new_monitor == client.monitor)
        return;

    if (!move_floating_client_to_workspace(client, *new_monitor, monitors_[*new_monitor].current_workspace, false))
        return;

    if (active_window_ == client.id && is_suppressed_by_fullscreen(client))
        focus_or_fallback(monitors_[client.monitor], false);
}

} // namespace lwm
