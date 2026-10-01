#include "lwm/core/focus.hpp"
#include "wm.hpp"
#include <utility>

namespace lwm {

bool WindowManager::grab_pointer_for_drag(xcb_cursor_t cursor)
{
    if (drag_active())
        return false;
    auto* reply = xcb_grab_pointer_reply(
        conn_.get(),
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
        ),
        nullptr
    );
    bool acquired = reply && reply->status == XCB_GRAB_STATUS_SUCCESS;
    free(reply);
    return acquired;
}

std::optional<WindowManager::SplitBorderHit> WindowManager::hit_split_border(int16_t x, int16_t y) const
{
    auto index = focus::monitor_index_at_point(state_.monitors(), x, y);
    if (!index)
        return std::nullopt;
    auto const& monitor = state_.monitors()[*index];
    auto const& workspace = monitor.current();
    auto hit = layout_.hit_test(
        tiled_participants(monitor).size(),
        monitor.working_area(),
        workspace.layout_strategy,
        workspace.split_ratios,
        x,
        y
    );
    if (!hit)
        return std::nullopt;
    return SplitBorderHit{ *hit, *index };
}

// Domain changes (floating conversion, leaving maximize) happen only after the
// pointer grab succeeds.
void WindowManager::begin_window_drag(xcb_window_t window, int16_t x, int16_t y, uint8_t button, floating::ResizeEdge edges)
{
    auto const* client = state_.find(window);
    if (!client || !state_.visible(*client) || client->fullscreen || state_.showing_desktop() || !grab_pointer_for_drag())
        return;
    focus_window(window);
    // A tiled resize away from a split becomes a floating resize.
    if (client->kind() == Client::Kind::Tiled && edges != floating::ResizeEdge::None)
        state_.floating(window, true);
    if (presents_maximized(*client))
    {
        // Exiting maximize starts from the displayed rectangle.
        state_.geometry(window, presentation_geometry(*client));
        state_.maximize(window, false, false);
    }
    auto const* floating = floating_mode(*client);
    Geometry start = floating ? floating->geometry : tiled_mode(*client)->layout;
    drag_ = Drag{ WindowDrag{ window, client->kind(), client->monitor, client->workspace, start, edges }, x, y, x, y, button };
}

void WindowManager::begin_tiled_resize(SplitHitResult const& hit, size_t monitor, int16_t x, int16_t y, uint8_t button)
{
    auto const& target = state_.monitors()[monitor];
    auto participants = tiled_participants(target);
    if (!grab_pointer_for_drag(hit.direction == SplitDirection::Horizontal ? cursor_resize_h_ : cursor_resize_v_))
        return;
    drag_ = Drag{
        TiledResize{
            monitor, target.current_workspace, hit, target.working_area(), target.current().layout_strategy, std::move(participants)
        },
        x,
        y,
        x,
        y,
        button
    };
}

// A drag ends when its client or split context stops existing as it was.
void WindowManager::validate_drag()
{
    if (!drag_)
        return;
    bool valid = false;
    if (auto const* window = std::get_if<WindowDrag>(&drag_->operation))
    {
        auto const* client = state_.find(window->window);
        valid = client && client->kind() == window->kind && state_.visible(*client) && !client->fullscreen
            && !presents_maximized(*client) && client->monitor == window->monitor
            && client->workspace == window->workspace && !state_.showing_desktop();
    }
    else
    {
        auto const& split = std::get<TiledResize>(drag_->operation);
        if (split.monitor < state_.monitors().size())
        {
            auto const& monitor = state_.monitors()[split.monitor];
            valid = monitor.current_workspace == split.workspace && monitor.working_area() == split.area
                && monitor.current().layout_strategy == split.strategy && tiled_participants(monitor) == split.participants;
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
        if (window->kind == Client::Kind::Tiled)
        {
            // The tile preview is presentation only; membership changes on release.
            presentation_dirty_ = true;
            return;
        }
        auto const* client = state_.find(window->window);
        if (!client || client->kind() != window->kind)
        {
            end_drag(false);
            return;
        }
        update_floating_geometry(*client, floating::drag_geometry(window->start_geometry, dx, dy, window->edges));
        window->monitor = client->monitor;
        window->workspace = client->workspace;
        return;
    }
    auto const& resize = std::get<TiledResize>(drag_->operation);
    auto const& split = resize.split;
    if (split.available_extent <= 0)
        return;
    int32_t delta = split.direction == SplitDirection::Horizontal ? dx : dy;
    double ratio = config_.layout.clamp_ratio(split.ratio + static_cast<double>(delta) / split.available_extent);
    // Validation guarantees the resized workspace is still current.
    state_.ratio(resize.monitor, split.address, ratio);
}

void WindowManager::end_drag(bool commit)
{
    if (!drag_)
        return;
    auto drag = *std::exchange(drag_, std::nullopt);
    xcb_ungrab_pointer(conn_.get(), XCB_CURRENT_TIME);
    set_root_cursor(cursor_default_);
    drain_requested_ = true;
    presentation_dirty_ = true;

    if (auto const* resize = std::get_if<TiledResize>(&drag.operation))
    {
        auto const& ratios = state_.monitors()[resize->monitor].workspaces[resize->workspace].split_ratios;
        auto it = ratios.find(resize->split.address);
        if (commit && it != ratios.end() && it->second != resize->split.ratio)
            queue_event(event::LayoutChange{ "resize_split", std::to_string(it->second), std::nullopt });
        return;
    }
    auto const& move = std::get<WindowDrag>(drag.operation);
    auto const* client = state_.find(move.window);
    if (!commit || move.kind != Client::Kind::Tiled || !client || client->kind() != Client::Kind::Tiled)
        return;
    // Layout slots are not membership indices: hidden members have no slot and
    // sticky guests belong to another workspace. A drop on the guest suffix
    // appends to the current workspace.
    auto target = focus::monitor_index_at_point(state_.monitors(), drag.last_x, drag.last_y).value_or(client->monitor);
    auto const& monitor = state_.monitors()[target];
    auto const& workspace = monitor.current();
    auto participants = tiled_participants(monitor);
    std::erase(participants, client->id);
    size_t slot = layout_.drop_target_index(
        participants.size() + 1,
        monitor.working_area(),
        workspace.layout_strategy,
        workspace.split_ratios,
        drag.last_x,
        drag.last_y
    );
    xcb_window_t anchor = slot < participants.size() && state_.require(participants[slot]).workspace == monitor.current_workspace
        ? participants[slot]
        : XCB_NONE;
    size_t index = 0;
    for (auto window : workspace.windows)
    {
        if (window == anchor)
            break;
        if (window != client->id)
            ++index;
    }
    if (state_.relocate(client->id, target, monitor.current_workspace, State::RelocationGeometry::Preserve, index))
        focus_window(client->id);
}


} // namespace lwm
