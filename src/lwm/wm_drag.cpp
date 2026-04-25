/**
 * @file wm_drag.cpp
 * @brief Mouse drag operation implementation for WindowManager
 *
 * This file contains the drag-and-drop state machine for window movement and resizing.
 * It handles both floating window dragging (move/resize) and tiled window reordering.
 *
 * The drag lifecycle is:
 *   begin_floating_move / begin_floating_resize / begin_tiled_drag -> update_drag -> end_drag
 *
 * Extracted from wm.cpp to improve navigability and local reasoning about
 * drag-related behavior. All functions are methods of WindowManager.
 */

#include "lwm/core/focus.hpp"
#include "wm.hpp"
#include <unordered_set>

namespace lwm {

namespace {

template<class... Ts>
struct Overloaded : Ts...
{
    using Ts::operator()...;
};

template<class... Ts>
Overloaded(Ts...) -> Overloaded<Ts...>;

} // namespace

void WindowManager::set_root_cursor(xcb_cursor_t cursor)
{
    if (cursor == current_root_cursor_)
        return;
    uint32_t cursor_val = cursor;
    xcb_change_window_attributes(conn_.get(), conn_.screen()->root, XCB_CW_CURSOR, &cursor_val);
    current_root_cursor_ = cursor;
}

void WindowManager::grab_pointer_for_drag(xcb_cursor_t cursor)
{
    xcb_grab_pointer(
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
    conn_.flush();
}

size_t WindowManager::visible_tiled_count(Monitor const& monitor) const
{
    size_t count = 0;
    std::unordered_set<xcb_window_t> seen;
    // workspace.windows entries are guaranteed managed; assert_workspace_consistency enforces it.
    auto const& ws = monitor.current();
    for (auto w : ws.windows)
    {
        auto const& c = require_client(w);
        if (!c.fullscreen && !c.hidden)
        {
            ++count;
            seen.insert(w);
        }
    }
    for (auto const& other_ws : monitor.workspaces)
    {
        if (&other_ws == &ws)
            continue;
        for (auto w : other_ws.windows)
        {
            if (seen.contains(w))
                continue;
            auto const& c = require_client(w);
            if (c.sticky && !c.hidden && !c.fullscreen)
            {
                ++count;
                seen.insert(w);
            }
        }
    }
    return count;
}

std::optional<WindowManager::SplitBorderHit> WindowManager::try_hit_split_border(int16_t x, int16_t y)
{
    Monitor* mon = monitor_at_point(x, y);
    if (!mon)
        return std::nullopt;

    size_t vis_count = visible_tiled_count(*mon);
    if (vis_count < 2)
        return std::nullopt;

    auto& ws = mon->current();
    auto hit = layout_.hit_test(vis_count, mon->working_area(), ws.layout_strategy, ws.split_ratios, x, y);
    if (!hit)
        return std::nullopt;

    return SplitBorderHit { *hit, monitor_index(*mon) };
}

bool WindowManager::drag_active() const
{
    return !std::holds_alternative<NoDrag>(drag_state_);
}

void WindowManager::begin_floating_move(xcb_window_t window, int16_t root_x, int16_t root_y)
{
    auto* client = get_client(window);
    if (!client)
        return;
    if (client->fullscreen)
        return;
    if (client->kind != Client::Kind::Floating)
        return;

    drag_state_ = FloatingMove { window, root_x, root_y, root_x, root_y, floating_geometry(*client) };

    grab_pointer_for_drag();
}

void WindowManager::begin_floating_resize(xcb_window_t window, int16_t root_x, int16_t root_y)
{
    auto* client = get_client(window);
    if (!client)
        return;
    if (client->fullscreen)
        return;
    if (client->kind != Client::Kind::Floating)
        return;

    drag_state_ = FloatingResize { window, root_x, root_y, root_x, root_y, floating_geometry(*client) };

    grab_pointer_for_drag();
}

void WindowManager::begin_tiled_drag(xcb_window_t window, int16_t root_x, int16_t root_y)
{
    if (showing_desktop_)
        return;
    auto const* client = get_client(window);
    if (!client)
        return;
    if (client->fullscreen)
        return;
    if (client->kind != Client::Kind::Tiled)
        return;

    drag_state_ = TiledMove { window, root_x, root_y, root_x, root_y, client->tiled_geometry };

    grab_pointer_for_drag();
}

void WindowManager::begin_tiled_resize(
    SplitHitResult const& hit,
    size_t monitor_idx,
    int16_t root_x,
    int16_t root_y)
{
    drag_state_ = TiledResize {
        monitor_idx,
        monitors_[monitor_idx].current_workspace,
        hit.address,
        hit.direction,
        hit.ratio,
        hit.available_extent,
        root_x,
        root_y,
        root_x,
        root_y
    };

    xcb_cursor_t resize_cursor = (hit.direction == SplitDirection::Horizontal)
        ? cursor_resize_h_ : cursor_resize_v_;
    grab_pointer_for_drag(resize_cursor);
}

void WindowManager::record_drag_position(int16_t root_x, int16_t root_y)
{
    std::visit(Overloaded {
        [](NoDrag&) {},
        [=](auto& drag)
        {
            drag.last_root_x = root_x;
            drag.last_root_y = root_y;
        }
    }, drag_state_);
}

void WindowManager::update_drag(int16_t root_x, int16_t root_y)
{
    if (!drag_active())
        return;

    bool abort_drag = false;
    auto update_tiled_resize = [&](TiledResize& tr)
    {
        tr.last_root_x = root_x;
        tr.last_root_y = root_y;
        if (tr.monitor_idx >= monitors_.size()
            || monitors_[tr.monitor_idx].current_workspace != tr.workspace_idx)
        {
            abort_drag = true;
            return;
        }

        int32_t pixel_delta;
        if (tr.direction == SplitDirection::Horizontal)
            pixel_delta = static_cast<int32_t>(root_x) - static_cast<int32_t>(tr.start_root_x);
        else
            pixel_delta = static_cast<int32_t>(root_y) - static_cast<int32_t>(tr.start_root_y);

        if (tr.available_extent <= 0)
            return;

        // Motion compression: drain all queued motion events, use the latest position.
        // This prevents redundant rearranges when events queue up faster than we process them.
        {
            xcb_generic_event_t* ev;
            while ((ev = xcb_poll_for_queued_event(conn_.get())) != nullptr)
            {
                uint8_t type = ev->response_type & ~0x80;
                if (type == XCB_MOTION_NOTIFY)
                {
                    auto* m = reinterpret_cast<xcb_motion_notify_event_t*>(ev);
                    root_x = m->root_x;
                    root_y = m->root_y;
                    tr.last_root_x = root_x;
                    tr.last_root_y = root_y;
                    free(ev);
                    continue;
                }
                // Non-motion event: push back is not possible, so just stop compressing.
                // The event loop will pick up remaining events after we return.
                // We must handle this event now to avoid losing it.
                handle_event(*ev);
                free(ev);
                if (!std::holds_alternative<TiledResize>(drag_state_))
                    return; // drag was cancelled by the handled event
                break;
            }

            auto* live_resize = std::get_if<TiledResize>(&drag_state_);
            if (!live_resize)
                return;
            auto& live = *live_resize;
            // Recompute delta with compressed coordinates
            if (live.direction == SplitDirection::Horizontal)
                pixel_delta = static_cast<int32_t>(root_x) - static_cast<int32_t>(live.start_root_x);
            else
                pixel_delta = static_cast<int32_t>(root_y) - static_cast<int32_t>(live.start_root_y);
        }

        double ratio_delta = static_cast<double>(pixel_delta) / static_cast<double>(tr.available_extent);
        double min_r = config_.layout.min_ratio;
        double new_ratio = std::clamp(tr.start_ratio + ratio_delta, min_r, 1.0 - min_r);

        auto& ws = monitors_[tr.monitor_idx].current();
        auto it = ws.split_ratios.find(tr.address);
        if (it != ws.split_ratios.end() && it->second == new_ratio)
            return;

        // Server grab prevents clients from repainting between window configures,
        // eliminating flicker when multiple tiled windows resize simultaneously.
        xcb_grab_server(conn_.get());
        ws.split_ratios[tr.address] = new_ratio;
        rearrange_monitor(monitors_[tr.monitor_idx], true);
        xcb_ungrab_server(conn_.get());
        conn_.flush();
        return;
    };

    auto update_tiled_move = [&](TiledMove& drag)
    {
        drag.last_root_x = root_x;
        drag.last_root_y = root_y;
        int32_t dx = static_cast<int32_t>(root_x) - static_cast<int32_t>(drag.start_root_x);
        int32_t dy = static_cast<int32_t>(root_y) - static_cast<int32_t>(drag.start_root_y);
        int32_t new_x = static_cast<int32_t>(drag.start_geometry.x) + dx;
        int32_t new_y = static_cast<int32_t>(drag.start_geometry.y) + dy;
        uint32_t values[] = { static_cast<uint32_t>(new_x), static_cast<uint32_t>(new_y) };
        xcb_configure_window(conn_.get(), drag.window, XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y, values);
        conn_.flush();
        return;
    };

    auto update_floating = [&](auto& drag, bool resizing)
    {
        drag.last_root_x = root_x;
        drag.last_root_y = root_y;

        auto* client = get_client(drag.window);
        if (!client || client->kind != Client::Kind::Floating)
            return;

        int32_t dx = static_cast<int32_t>(root_x) - static_cast<int32_t>(drag.start_root_x);
        int32_t dy = static_cast<int32_t>(root_y) - static_cast<int32_t>(drag.start_root_y);

        Geometry updated = drag.start_geometry;
        if (resizing)
        {
            int32_t new_w = static_cast<int32_t>(drag.start_geometry.width) + dx;
            int32_t new_h = static_cast<int32_t>(drag.start_geometry.height) + dy;
            updated.width = static_cast<uint16_t>(std::max<int32_t>(1, new_w));
            updated.height = static_cast<uint16_t>(std::max<int32_t>(1, new_h));

            uint32_t hinted_width = updated.width;
            uint32_t hinted_height = updated.height;
            layout_.apply_size_hints(drag.window, hinted_width, hinted_height);
            updated.width = static_cast<uint16_t>(std::max<uint32_t>(1, hinted_width));
            updated.height = static_cast<uint16_t>(std::max<uint32_t>(1, hinted_height));
        }
        else
        {
            updated.x = static_cast<int16_t>(static_cast<int32_t>(drag.start_geometry.x) + dx);
            updated.y = static_cast<int16_t>(static_cast<int32_t>(drag.start_geometry.y) + dy);
        }

        floating_geometry(*client) = updated;

        apply_floating_geometry(*client);
        update_floating_monitor_for_geometry(*client);

        if (active_window_ == drag.window)
        {
            focused_monitor_ = client->monitor;
            update_ewmh_current_desktop();
        }

        conn_.flush();
    };

    std::visit(Overloaded {
        [](NoDrag&) {},
        update_tiled_resize,
        update_tiled_move,
        [&](FloatingMove& drag) { update_floating(drag, false); },
        [&](FloatingResize& drag) { update_floating(drag, true); },
    }, drag_state_);

    if (abort_drag)
        end_drag();
}

void WindowManager::end_drag()
{
    if (!drag_active())
        return;

    bool was_tiled_resize = std::holds_alternative<TiledResize>(drag_state_);

    auto finish_tiled_move = [&](TiledMove const& drag)
    {
        xcb_window_t window = drag.window;
        auto* client = get_client(window);
        if (!client || client->kind != Client::Kind::Tiled)
            return;

        size_t source_monitor_idx = client->monitor;
        size_t source_workspace_idx = client->workspace;
        auto target_monitor =
            focus::monitor_index_at_point(monitors_, drag.last_root_x, drag.last_root_y);
        size_t target_monitor_idx = target_monitor.value_or(source_monitor_idx);
        size_t target_workspace_idx = monitors_[target_monitor_idx].current_workspace;
        bool same_workspace =
            source_monitor_idx == target_monitor_idx && source_workspace_idx == target_workspace_idx;

        auto& source_ws = monitors_[source_monitor_idx].workspaces[source_workspace_idx];
        auto source_it = source_ws.find_window(window);
        if (source_it == source_ws.windows.end())
            return;

        auto& target_ws = monitors_[target_monitor_idx].workspaces[target_workspace_idx];
        size_t layout_count = target_ws.windows.size();
        if (!same_workspace)
            layout_count += 1;

        size_t target_index = 0;
        if (layout_count > 0)
        {
            target_index = layout_.drop_target_index(
                layout_count,
                monitors_[target_monitor_idx].working_area(),
                target_ws.layout_strategy,
                target_ws.split_ratios,
                drag.last_root_x,
                drag.last_root_y
            );
        }

        if (move_tiled_client_to_workspace(*client, target_monitor_idx, target_workspace_idx, target_index))
        {
            workspace_policy::set_workspace_focus(target_ws, window);
            flush_and_drain_crossing();
            focus_any_window(window);
        }
    };

    std::visit(Overloaded {
        [](NoDrag const&) {},
        [&](TiledMove const& drag) { finish_tiled_move(drag); },
        [](TiledResize const&) {},
        [](FloatingMove const&) {},
        [](FloatingResize const&) {},
    }, drag_state_);

    drag_state_ = NoDrag {};
    xcb_ungrab_pointer(conn_.get(), XCB_CURRENT_TIME);

    if (was_tiled_resize && cursor_default_ != XCB_NONE)
        set_root_cursor(cursor_default_);

    flush_and_drain_crossing();
}

}
