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

#include "wm.hpp"
#include "lwm/core/focus.hpp"

namespace lwm {

void WindowManager::begin_drag(xcb_window_t window, bool resize, int16_t root_x, int16_t root_y)
{
    if (is_client_fullscreen(window))
        return;

    auto* floating_window = find_floating_window(window);
    if (!floating_window)
        return;

    drag_state_.active = true;
    drag_state_.tiled = false;
    drag_state_.resizing = resize;
    drag_state_.window = window;
    drag_state_.start_root_x = root_x;
    drag_state_.start_root_y = root_y;
    drag_state_.last_root_x = root_x;
    drag_state_.last_root_y = root_y;
    drag_state_.start_geometry = floating_window->geometry;

    xcb_grab_pointer(
        conn_.get(),
        0,
        conn_.screen()->root,
        XCB_EVENT_MASK_POINTER_MOTION | XCB_EVENT_MASK_BUTTON_RELEASE,
        XCB_GRAB_MODE_ASYNC,
        XCB_GRAB_MODE_ASYNC,
        XCB_NONE,
        XCB_NONE,
        XCB_CURRENT_TIME
    );
    conn_.flush();
}

void WindowManager::begin_tiled_drag(xcb_window_t window, int16_t root_x, int16_t root_y)
{
    if (showing_desktop_)
        return;
    if (is_client_fullscreen(window))
        return;
    if (find_floating_window(window))
        return;
    if (!monitor_containing_window(window))
        return;

    auto geom_cookie = xcb_get_geometry(conn_.get(), window);
    auto* geom_reply = xcb_get_geometry_reply(conn_.get(), geom_cookie, nullptr);
    if (!geom_reply)
        return;

    drag_state_.active = true;
    drag_state_.tiled = true;
    drag_state_.resizing = false;
    drag_state_.window = window;
    drag_state_.start_root_x = root_x;
    drag_state_.start_root_y = root_y;
    drag_state_.last_root_x = root_x;
    drag_state_.last_root_y = root_y;
    drag_state_.start_geometry = { geom_reply->x, geom_reply->y, geom_reply->width, geom_reply->height };
    free(geom_reply);

    xcb_grab_pointer(
        conn_.get(),
        0,
        conn_.screen()->root,
        XCB_EVENT_MASK_POINTER_MOTION | XCB_EVENT_MASK_BUTTON_RELEASE,
        XCB_GRAB_MODE_ASYNC,
        XCB_GRAB_MODE_ASYNC,
        XCB_NONE,
        XCB_NONE,
        XCB_CURRENT_TIME
    );
    conn_.flush();
}

void WindowManager::update_drag(int16_t root_x, int16_t root_y)
{
    if (!drag_state_.active)
        return;

    drag_state_.last_root_x = root_x;
    drag_state_.last_root_y = root_y;

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

    auto* floating_window = find_floating_window(drag_state_.window);
    if (!floating_window)
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
        layout_.apply_size_hints(floating_window->id, hinted_width, hinted_height);
        updated.width = static_cast<uint16_t>(std::max<uint32_t>(1, hinted_width));
        updated.height = static_cast<uint16_t>(std::max<uint32_t>(1, hinted_height));
    }
    else
    {
        updated.x = static_cast<int16_t>(static_cast<int32_t>(drag_state_.start_geometry.x) + dx);
        updated.y = static_cast<int16_t>(static_cast<int32_t>(drag_state_.start_geometry.y) + dy);
    }

    floating_window->geometry = updated;
    apply_floating_geometry(*floating_window);
    update_floating_monitor_for_geometry(*floating_window);

    if (active_window_ == floating_window->id)
    {
        focused_monitor_ = floating_window->monitor;
        update_ewmh_current_desktop();
        update_all_bars();
    }

    conn_.flush();
}

void WindowManager::end_drag()
{
    if (!drag_state_.active)
        return;

    if (drag_state_.tiled)
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
                bool same_workspace = *source_monitor_idx == target_monitor_idx
                    && *source_workspace_idx == target_workspace_idx;

                auto& source_ws = monitors_[*source_monitor_idx].workspaces[*source_workspace_idx];
                auto source_it = source_ws.find_window(window);
                if (source_it != source_ws.windows.end())
                {
                    Window moved_window = *source_it;

                    auto& target_ws = monitors_[target_monitor_idx].workspaces[target_workspace_idx];
                    size_t layout_count = target_ws.windows.size();
                    if (!same_workspace)
                        layout_count += 1;

                    source_ws.windows.erase(source_it);

                    size_t target_index = 0;
                    if (layout_count > 0)
                    {
                        target_index = layout_.drop_target_index(
                            layout_count,
                            monitors_[target_monitor_idx].working_area(),
                            bar_.has_value(),
                            drag_state_.last_root_x,
                            drag_state_.last_root_y
                        );
                    }

                    size_t insert_index = std::min(target_index, target_ws.windows.size());
                    target_ws.windows.insert(target_ws.windows.begin() + insert_index, moved_window);
                    target_ws.focused_window = window;

                    if (!same_workspace && source_ws.focused_window == window)
                    {
                        source_ws.focused_window =
                            source_ws.windows.empty() ? XCB_NONE : source_ws.windows.back().id;
                    }

                    if (!same_workspace)
                    {
                        uint32_t desktop = get_ewmh_desktop_index(target_monitor_idx, target_workspace_idx);
                        ewmh_.set_window_desktop(window, desktop);
                    }

                    if (*source_monitor_idx == target_monitor_idx)
                    {
                        rearrange_monitor(monitors_[target_monitor_idx]);
                    }
                    else
                    {
                        rearrange_monitor(monitors_[*source_monitor_idx]);
                        rearrange_monitor(monitors_[target_monitor_idx]);
                    }

                    update_ewmh_client_list();
                    update_all_bars();
                    focus_window(window);
                }
            }
        }
    }

    drag_state_.active = false;
    drag_state_.tiled = false;
    drag_state_.window = XCB_NONE;
    xcb_ungrab_pointer(conn_.get(), XCB_CURRENT_TIME);
    conn_.flush();
}

} // namespace lwm
