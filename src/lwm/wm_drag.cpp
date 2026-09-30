#include "lwm/core/focus.hpp"
#include "wm.hpp"
#include <utility>

namespace lwm {

void WindowManager::set_root_cursor(xcb_cursor_t cursor)
{
    if (cursor == current_root_cursor_)
        return;
    uint32_t value = cursor;
    xcb_change_window_attributes(conn_.get(), conn_.screen()->root, XCB_CW_CURSOR, &value);
    current_root_cursor_ = cursor;
}

bool WindowManager::grab_pointer_for_drag(xcb_cursor_t cursor)
{
    if (drag_active())
        return false;
    auto cookie = xcb_grab_pointer(
        conn_.get(),
        0,
        conn_.screen()->root,
        XCB_EVENT_MASK_POINTER_MOTION | XCB_EVENT_MASK_BUTTON_RELEASE,
        XCB_GRAB_MODE_ASYNC,
        XCB_GRAB_MODE_ASYNC,
        XCB_NONE,
        cursor,
        XCB_CURRENT_TIME
    );
    auto* reply = xcb_grab_pointer_reply(conn_.get(), cookie, nullptr);
    bool acquired = reply && reply->status == XCB_GRAB_STATUS_SUCCESS;
    free(reply);
    return acquired;
}

std::vector<xcb_window_t> WindowManager::tiled_participants(Monitor const& monitor) const
{
    std::vector<xcb_window_t> windows;
    windows.reserve(monitor.current().windows.size());
    auto collect = [&](Workspace const& workspace, bool current)
    {
        for (auto window : workspace.windows)
        {
            auto const& client = require_client(window);
            if (is_visible(client) && !client.fullscreen && (current || client.sticky))
                windows.push_back(window);
        }
    };
    // Unique membership is an invariant; current workspace precedes sticky guests.
    collect(monitor.current(), true);
    for (size_t i = 0; i < monitor.workspaces.size(); ++i)
        if (i != monitor.current_workspace)
            collect(monitor.workspaces[i], false);
    return windows;
}

std::optional<WindowManager::SplitBorderHit> WindowManager::try_hit_split_border(int16_t x, int16_t y)
{
    auto* monitor = monitor_at_point(x, y);
    if (!monitor)
        return std::nullopt;
    auto& ws = monitor->current();
    auto hit = layout_.hit_test(
        tiled_participants(*monitor).size(),
        monitor->working_area(),
        ws.layout_strategy,
        ws.split_ratios,
        x,
        y
    );
    if (!hit)
        return std::nullopt;
    return SplitBorderHit{ *hit, monitor_index(*monitor) };
}

void WindowManager::begin_window_drag(
    xcb_window_t window,
    int16_t x,
    int16_t y,
    uint8_t button,
    floating::ResizeEdge edges
)
{
    auto* client = get_client(window);
    if (!client || !is_visible(*client) || client->iconic || client->fullscreen || showing_desktop_
        || (client->kind() != Client::Kind::Floating && client->kind() != Client::Kind::Tiled))
        return;
    if (!grab_pointer_for_drag())
        return;
    focus_any_window(window);
    // A tiled resize away from a split becomes a floating resize, after acquisition.
    if (client->kind() == Client::Kind::Tiled && edges != floating::ResizeEdge::None)
    {
        state_.floating(window, true);
        invalidate_monitor(client->monitor);
    }
    if (client->kind() == Client::Kind::Floating && (client->maximized_horz || client->maximized_vert))
    {
        state_.geometry(window, presentation_geometry(*client));
        state_.maximize(client->id, false, false);
    }
    Geometry start = client->kind() == Client::Kind::Floating ? floating_geometry(*client) : client->tiled_geometry;
    drag_ = Drag{
        WindowDrag{ window, client->kind(), client->monitor, client->workspace, start, edges },
        x, y, x, y, button
    };
}

void WindowManager::begin_tiled_resize(SplitHitResult const& hit, size_t monitor, int16_t x, int16_t y, uint8_t button)
{
    auto& mon = monitors_[monitor];
    auto participants = tiled_participants(mon);
    auto cursor = hit.direction == SplitDirection::Horizontal ? cursor_resize_h_ : cursor_resize_v_;
    if (!grab_pointer_for_drag(cursor))
        return;
    drag_ = Drag{
        TiledResize{ monitor,
                    mon.current_workspace,
                    hit, mon.working_area(),
                    mon.current().layout_strategy,
                    std::move(participants) },
        x,
        y,
        x,
        y,
        button
    };
}

void WindowManager::validate_drag(bool layout_changed)
{
    if (!drag_)
        return;
    bool valid = false;
    if (auto const* window = std::get_if<WindowDrag>(&drag_->operation))
    {
        auto const* client = get_client(window->window);
        valid = client && client->kind() == window->kind && is_visible(*client) && !client->iconic
            && !client->fullscreen && !client->maximized_horz && !client->maximized_vert
            && client->monitor == window->monitor && client->workspace == window->workspace && !showing_desktop_;
    }
    else
    {
        if (!layout_changed)
            return;
        auto const& split = std::get<TiledResize>(drag_->operation);
        if (split.monitor < monitors_.size())
        {
            auto const& mon = monitors_[split.monitor];
            valid = mon.current_workspace == split.workspace && mon.working_area() == split.area
                && mon.current().layout_strategy == split.strategy && tiled_participants(mon) == split.participants;
        }
    }
    if (!valid)
        end_drag(false);
}

void WindowManager::update_drag(int16_t x, int16_t y)
{
    if (!drag_)
        return;
    drag_->last_x = x;
    drag_->last_y = y;
    int32_t dx = static_cast<int32_t>(x) - drag_->start_x;
    int32_t dy = static_cast<int32_t>(y) - drag_->start_y;
    if (auto* window = std::get_if<WindowDrag>(&drag_->operation))
    {
        auto* client = get_client(window->window);
        if (!client || client->kind() != window->kind)
        {
            end_drag(false);
            return;
        }
        if (window->kind == Client::Kind::Floating)
        {
            auto updated = floating::drag_geometry(window->start_geometry, dx, dy, window->edges);
            if (updated == floating_geometry(*client))
                return;
            state_.geometry(client->id, updated);
            update_floating_monitor_for_geometry(*client);
            window->monitor = client->monitor;
            window->workspace = client->workspace;
            if (active_window_ == client->id && focused_monitor_ != client->monitor)
            {
                state_.focus_monitor(client->monitor);
                request_current_desktop_update();
            }
        }
        request_geometry(*client);
    }
    else
    {
        auto const& resize = std::get<TiledResize>(drag_->operation);
        auto const& split = resize.split;
        if (split.available_extent <= 0)
            return;
        int32_t delta = split.direction == SplitDirection::Horizontal ? dx : dy;
        double ratio = std::clamp(
            split.ratio + static_cast<double>(delta) / split.available_extent,
            config_.layout.min_ratio,
            1.0 - config_.layout.min_ratio
        );
        auto& ratios = monitors_[resize.monitor].workspaces[resize.workspace].split_ratios;
        auto it = ratios.find(split.address);
        if (it != ratios.end() && it->second == ratio)
            return;
        state_.ratio(resize.monitor, split.address, ratio);
    }
}

void WindowManager::end_drag(bool commit)
{
    if (!drag_)
        return;
    auto drag = std::exchange(drag_, std::nullopt);
    xcb_ungrab_pointer(conn_.get(), XCB_CURRENT_TIME);
    if (cursor_default_ != XCB_NONE)
        set_root_cursor(cursor_default_);
    state_.effects().drain_crossing = true;

    auto const* move = std::get_if<WindowDrag>(&drag->operation);
    if (!move || move->kind != Client::Kind::Tiled)
        return;
    auto* client = get_client(move->window);
    if (!client || client->kind() != Client::Kind::Tiled)
        return;
    // Ending the preview always restores layout geometry, even if no drop is committed.
    request_geometry(*client);
    if (!commit)
        return;
    auto target = focus::monitor_index_at_point(monitors_, drag->last_x, drag->last_y).value_or(client->monitor);
    auto& mon = monitors_[target];
    auto& ws = mon.current();
    auto participants = tiled_participants(mon);
    std::erase(participants, client->id);
    size_t slot = layout_.drop_target_index(
        participants.size() + 1,
        mon.working_area(),
        ws.layout_strategy,
        ws.split_ratios,
        drag->last_x,
        drag->last_y
    );
    // Layout slots are not membership indices: hidden members have no slot, and
    // sticky guests belong to another workspace. Drops on the guest suffix append
    // to the current workspace without moving the guests or hidden members.
    xcb_window_t anchor =
        slot < participants.size() && require_client(participants[slot]).workspace == mon.current_workspace
        ? participants[slot]
        : XCB_NONE;
    size_t index = 0;
    for (auto window : ws.windows)
    {
        if (window == anchor)
            break;
        if (window != client->id)
            ++index;
    }
    if (state_.relocate(client->id, target, mon.current_workspace, RelocationGeometry::Preserve, index))
    {
        state_.remember_focus(target, mon.current_workspace, client->id);
        focus_any_window(client->id);
    }
}

} // namespace lwm
