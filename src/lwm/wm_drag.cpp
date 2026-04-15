/**
 * @file wm_drag.cpp
 * @brief Mouse drag operation implementation for WindowManager
 *
 * This file contains the drag-and-drop state machine for window movement and resizing.
 * It handles both floating window dragging (move/resize) and tiled window reordering.
 *
 * The drag lifecycle is:
 *   begin_drag / begin_tiled_drag -> update_drag (called on motion) -> end_drag
 *
 * Extracted from wm.cpp to improve navigability and local reasoning about
 * drag-related behavior. All functions are methods of WindowManager.
 */

#include "lwm/core/focus.hpp"
#include "wm.hpp"
#include <unordered_set>

namespace lwm {

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
    auto const& ws = monitor.current();
    for (auto w : ws.windows)
    {
        if (auto const* c = get_client(w); c && !c->fullscreen && !c->hidden)
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
            if (auto const* c = get_client(w); c && c->sticky && !c->hidden && !c->fullscreen)
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

void WindowManager::begin_drag(xcb_window_t window, bool resize, int16_t root_x, int16_t root_y)
{
    if (is_client_fullscreen(window))
        return;

    if (!is_floating_window(window))
        return;

    auto* client = get_client(window);
    if (!client)
        return;

    drag_state_.active = true;
    drag_state_.tiled = false;
    drag_state_.resizing = resize;
    drag_state_.window = window;
    drag_state_.start_root_x = root_x;
    drag_state_.start_root_y = root_y;
    drag_state_.last_root_x = root_x;
    drag_state_.last_root_y = root_y;
    drag_state_.start_geometry = client->floating_geometry;

    grab_pointer_for_drag();
}

void WindowManager::begin_tiled_drag(xcb_window_t window, int16_t root_x, int16_t root_y)
{
    if (showing_desktop_)
        return;
    if (is_client_fullscreen(window))
        return;
    if (is_floating_window(window))
        return;
    if (!monitor_index_for_window(window))
        return;

    auto const* client = get_client(window);
    if (!client)
        return;

    drag_state_.active = true;
    drag_state_.tiled = true;
    drag_state_.resizing = false;
    drag_state_.window = window;
    drag_state_.start_root_x = root_x;
    drag_state_.start_root_y = root_y;
    drag_state_.last_root_x = root_x;
    drag_state_.last_root_y = root_y;
    drag_state_.start_geometry = client->tiled_geometry;

    grab_pointer_for_drag();
}

void WindowManager::begin_tiled_resize(
    SplitHitResult const& hit,
    size_t monitor_idx,
    int16_t root_x,
    int16_t root_y)
{
    drag_state_.active = true;
    drag_state_.tiled = true;
    drag_state_.resizing = true;
    drag_state_.window = XCB_NONE;
    drag_state_.start_root_x = root_x;
    drag_state_.start_root_y = root_y;
    drag_state_.last_root_x = root_x;
    drag_state_.last_root_y = root_y;

    drag_state_.tiled_resize = TiledResizeState {
        monitor_idx,
        monitors_[monitor_idx].current_workspace,
        hit.address,
        hit.direction,
        hit.ratio,
        hit.available_extent
    };

    xcb_cursor_t resize_cursor = (hit.direction == SplitDirection::Horizontal)
        ? cursor_resize_h_ : cursor_resize_v_;
    grab_pointer_for_drag(resize_cursor);
}

void WindowManager::update_drag(int16_t root_x, int16_t root_y)
{
    if (!drag_state_.active)
        return;

    drag_state_.last_root_x = root_x;
    drag_state_.last_root_y = root_y;

    // Tiled resize: adjust split ratio from mouse delta
    if (drag_state_.tiled_resize.has_value())
    {
        auto& tr = *drag_state_.tiled_resize;
        if (tr.monitor_idx >= monitors_.size()
            || monitors_[tr.monitor_idx].current_workspace != tr.workspace_idx)
        {
            end_drag();
            return;
        }

        int32_t pixel_delta;
        if (tr.direction == SplitDirection::Horizontal)
            pixel_delta = static_cast<int32_t>(root_x) - static_cast<int32_t>(drag_state_.start_root_x);
        else
            pixel_delta = static_cast<int32_t>(root_y) - static_cast<int32_t>(drag_state_.start_root_y);

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
                    drag_state_.last_root_x = root_x;
                    drag_state_.last_root_y = root_y;
                    free(ev);
                    continue;
                }
                // Non-motion event: push back is not possible, so just stop compressing.
                // The event loop will pick up remaining events after we return.
                // We must handle this event now to avoid losing it.
                handle_event(*ev);
                free(ev);
                if (!drag_state_.active || !drag_state_.tiled_resize.has_value())
                    return; // drag was cancelled by the handled event
                break;
            }

            // Recompute delta with compressed coordinates
            if (tr.direction == SplitDirection::Horizontal)
                pixel_delta = static_cast<int32_t>(root_x) - static_cast<int32_t>(drag_state_.start_root_x);
            else
                pixel_delta = static_cast<int32_t>(root_y) - static_cast<int32_t>(drag_state_.start_root_y);
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
    }

    // Tiled reorder: move window visually to follow pointer
    if (drag_state_.tiled)
    {
        int32_t dx = static_cast<int32_t>(root_x) - static_cast<int32_t>(drag_state_.start_root_x);
        int32_t dy = static_cast<int32_t>(root_y) - static_cast<int32_t>(drag_state_.start_root_y);
        int32_t new_x = static_cast<int32_t>(drag_state_.start_geometry.x) + dx;
        int32_t new_y = static_cast<int32_t>(drag_state_.start_geometry.y) + dy;
        uint32_t values[] = { static_cast<uint32_t>(new_x), static_cast<uint32_t>(new_y) };
        xcb_configure_window(conn_.get(), drag_state_.window, XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y, values);
        conn_.flush();
        return;
    }

    auto* client = get_client(drag_state_.window);
    if (!client || !is_floating_window(drag_state_.window))
        return;

    int32_t dx = static_cast<int32_t>(root_x) - static_cast<int32_t>(drag_state_.start_root_x);
    int32_t dy = static_cast<int32_t>(root_y) - static_cast<int32_t>(drag_state_.start_root_y);

    Geometry updated = drag_state_.start_geometry;
    if (drag_state_.resizing)
    {
        int32_t new_w = static_cast<int32_t>(drag_state_.start_geometry.width) + dx;
        int32_t new_h = static_cast<int32_t>(drag_state_.start_geometry.height) + dy;
        updated.width = static_cast<uint16_t>(std::max<int32_t>(1, new_w));
        updated.height = static_cast<uint16_t>(std::max<int32_t>(1, new_h));

        uint32_t hinted_width = updated.width;
        uint32_t hinted_height = updated.height;
        layout_.apply_size_hints(drag_state_.window, hinted_width, hinted_height);
        updated.width = static_cast<uint16_t>(std::max<uint32_t>(1, hinted_width));
        updated.height = static_cast<uint16_t>(std::max<uint32_t>(1, hinted_height));
    }
    else
    {
        updated.x = static_cast<int16_t>(static_cast<int32_t>(drag_state_.start_geometry.x) + dx);
        updated.y = static_cast<int16_t>(static_cast<int32_t>(drag_state_.start_geometry.y) + dy);
    }

    client->floating_geometry = updated;

    apply_floating_geometry(drag_state_.window);
    update_floating_monitor_for_geometry(drag_state_.window);

    if (active_window_ == drag_state_.window)
    {
        focused_monitor_ = client->monitor;
        update_ewmh_current_desktop();
    }

    conn_.flush();
}

void WindowManager::end_drag()
{
    if (!drag_state_.active)
        return;

    if (drag_state_.tiled && !drag_state_.tiled_resize)
    {
        xcb_window_t window = drag_state_.window;
        auto source_monitor_idx = monitor_index_for_window(window);
        auto source_workspace_idx = workspace_index_for_window(window);
        if (source_monitor_idx && source_workspace_idx && !monitors_.empty())
        {
            auto target_monitor =
                focus::monitor_index_at_point(monitors_, drag_state_.last_root_x, drag_state_.last_root_y);
            size_t target_monitor_idx = target_monitor.value_or(*source_monitor_idx);
            if (target_monitor_idx < monitors_.size())
            {
                size_t target_workspace_idx = monitors_[target_monitor_idx].current_workspace;
                bool same_workspace =
                    *source_monitor_idx == target_monitor_idx && *source_workspace_idx == target_workspace_idx;

                auto& source_ws = monitors_[*source_monitor_idx].workspaces[*source_workspace_idx];
                auto source_it = source_ws.find_window(window);
                if (source_it != source_ws.windows.end())
                {
                    xcb_window_t moved_window = *source_it;

                    auto& target_ws = monitors_[target_monitor_idx].workspaces[target_workspace_idx];
                    size_t layout_count = target_ws.windows.size();
                    if (!same_workspace)
                        layout_count += 1;

                    source_ws.windows.erase(source_it);
                    workspace_policy::remove_from_focus_history(source_ws, moved_window);

                    size_t target_index = 0;
                    if (layout_count > 0)
                    {
                        target_index = layout_.drop_target_index(
                            layout_count,
                            monitors_[target_monitor_idx].working_area(),
                            target_ws.layout_strategy,
                            target_ws.split_ratios,
                            drag_state_.last_root_x,
                            drag_state_.last_root_y
                        );
                    }

                    size_t insert_index = std::min(target_index, target_ws.windows.size());
                    target_ws.windows.insert(target_ws.windows.begin() + insert_index, moved_window);
                    workspace_policy::set_workspace_focus(target_ws, window);

                    if (!same_workspace)
                    {
                        auto is_iconic = [this](xcb_window_t w) { return is_client_iconic(w); };
                        workspace_policy::fixup_workspace_focus(source_ws, window, is_iconic);
                    }

                    if (!same_workspace)
                    {
                        assign_window_workspace(window, target_monitor_idx, target_workspace_idx);
                    }

                    if (*source_monitor_idx == target_monitor_idx)
                    {
                        sync_visibility_for_monitor(target_monitor_idx);
                        rearrange_monitor(monitors_[target_monitor_idx]);
                    }
                    else
                    {
                        sync_visibility_for_monitor(*source_monitor_idx);
                        sync_visibility_for_monitor(target_monitor_idx);
                        rearrange_monitor(monitors_[*source_monitor_idx]);
                        rearrange_monitor(monitors_[target_monitor_idx]);
                    }

                    flush_and_drain_crossing();
                    LWM_ASSERT_INVARIANTS(clients_, monitors_);
                    focus_any_window(window);
                }
            }
        }
    }

    bool was_tiled_resize = drag_state_.tiled_resize.has_value();

    drag_state_.active = false;
    drag_state_.tiled = false;
    drag_state_.resizing = false;
    drag_state_.window = XCB_NONE;
    drag_state_.tiled_resize.reset();
    xcb_ungrab_pointer(conn_.get(), XCB_CURRENT_TIME);

    if (was_tiled_resize && cursor_default_ != XCB_NONE)
        set_root_cursor(cursor_default_);

    flush_and_drain_crossing();
}

}
