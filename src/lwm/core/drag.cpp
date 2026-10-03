// Pointer interactions. A floating drag changes normal geometry as it moves;
// a tiled move is a presentation preview until release; a split drag changes
// the captured split's ratio.

#include "state.hpp"
#include "focus.hpp"

namespace lwm {

bool State::can_drag(xcb_window_t id) const
{
    auto const* client = find(id);
    return !drag_ && client && visible(*client) && !client->fullscreen && !showing_desktop_;
}

void State::begin_window_drag(xcb_window_t id, int16_t x, int16_t y, uint8_t button, floating::ResizeEdge edges)
{
    if (!can_drag(id))
        return;
    focus(id);
    auto const& client = require(id);
    // A tiled resize away from a split becomes a floating resize.
    if (client.kind() == Client::Kind::Tiled && edges != floating::ResizeEdge::None)
        floating(id, true);
    if (presents_maximized(client))
    {
        // Exiting maximize starts from the displayed rectangle.
        geometry(id, presentation_geometry(client));
        maximize(id, false, false);
    }
    drag_ = Drag{ WindowDrag{ id, client.kind(), client.monitor, client.workspace, normal_geometry(client), edges }, x, y, x, y, button };
}

std::optional<State::SplitHit> State::split_at(int16_t x, int16_t y) const
{
    auto index = focus::monitor_index_at_point(monitors_, x, y);
    if (!index)
        return std::nullopt;
    auto const& monitor = monitors_[*index];
    auto const& workspace = monitor.current();
    auto hit = layout_.hit_test(
        tiled_participants(*index, fullscreen_visibility()).size(),
        monitor.working_area(),
        workspace.layout_strategy,
        workspace.split_ratios,
        x,
        y
    );
    if (!hit)
        return std::nullopt;
    return SplitHit{ *hit, *index };
}

void State::begin_split_drag(SplitHit const& hit, int16_t x, int16_t y, uint8_t button)
{
    if (drag_)
        return;
    auto const& monitor = monitors_[hit.monitor];
    drag_ = Drag{ SplitDrag{ hit.monitor,
                             monitor.current_workspace,
                             hit.hit,
                             monitor.working_area(),
                             monitor.current().layout_strategy,
                             tiled_participants(hit.monitor, fullscreen_visibility()) },
                  x,
                  y,
                  x,
                  y,
                  button };
}

void State::drag_to(int16_t x, int16_t y)
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
            mutated();
            return;
        }
        auto const* client = find(window->window);
        if (!client || client->kind() != window->kind)
        {
            end_drag(false);
            return;
        }
        request_geometry(window->window, floating::drag_geometry(window->start_geometry, dx, dy, window->edges));
        window->monitor = client->monitor;
        window->workspace = client->workspace;
        return;
    }
    auto const& resize = std::get<SplitDrag>(drag_->operation);
    auto const& split = resize.split;
    if (split.available_extent <= 0)
        return;
    int32_t delta = split.direction == SplitDirection::Horizontal ? dx : dy;
    // Validation guarantees the resized workspace is still current.
    ratio(resize.monitor, split.address, config_.layout.clamp_ratio(split.ratio + static_cast<double>(delta) / split.available_extent));
}

bool State::drag_valid() const
{
    if (auto const* window = std::get_if<WindowDrag>(&drag_->operation))
    {
        auto const* client = find(window->window);
        return client && client->kind() == window->kind && visible(*client) && !client->fullscreen
            && !presents_maximized(*client) && client->monitor == window->monitor && client->workspace == window->workspace
            && !showing_desktop_;
    }
    auto const& split = std::get<SplitDrag>(drag_->operation);
    if (split.monitor >= monitors_.size())
        return false;
    auto const& monitor = monitors_[split.monitor];
    return monitor.current_workspace == split.workspace && monitor.working_area() == split.area
        && monitor.current().layout_strategy == split.strategy
        && tiled_participants(split.monitor, fullscreen_visibility()) == split.participants;
}

std::optional<double> State::end_drag(bool commit)
{
    if (!drag_)
        return std::nullopt;
    mutated();
    auto drag = *std::exchange(drag_, std::nullopt);
    if (auto const* resize = std::get_if<SplitDrag>(&drag.operation))
    {
        if (!commit || resize->monitor >= monitors_.size())
            return std::nullopt;
        auto const& ratios = monitors_[resize->monitor].workspaces[resize->workspace].split_ratios;
        auto it = ratios.find(resize->split.address);
        if (it != ratios.end() && it->second != resize->split.ratio)
            return it->second;
        return std::nullopt;
    }
    auto const& move = std::get<WindowDrag>(drag.operation);
    auto const* client = find(move.window);
    if (!commit || move.kind != Client::Kind::Tiled || !client || client->kind() != Client::Kind::Tiled)
        return std::nullopt;
    // Layout slots are not membership indices: hidden members have no slot and
    // sticky guests belong to another workspace. A drop on the guest suffix
    // appends to the current workspace.
    auto target = focus::monitor_index_at_point(monitors_, drag.last_x, drag.last_y).value_or(client->monitor);
    auto const& monitor = monitors_[target];
    auto const& workspace = monitor.current();
    auto participants = tiled_participants(target, fullscreen_visibility());
    std::erase(participants, client->id);
    size_t slot = layout_.drop_target_index(
        participants.size() + 1, monitor.working_area(), workspace.layout_strategy, workspace.split_ratios, drag.last_x, drag.last_y
    );
    xcb_window_t anchor = slot < participants.size() && require(participants[slot]).workspace == monitor.current_workspace
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
    if (relocate(client->id, target, monitor.current_workspace, RelocationGeometry::Preserve, index))
        focus(client->id);
    return std::nullopt;
}

// A tiled move previews its rectangle without changing membership.
std::optional<Geometry> State::drag_preview(Client const& client) const
{
    if (drag_ && client.kind() == Client::Kind::Tiled && !client.fullscreen)
        if (auto const* move = std::get_if<WindowDrag>(&drag_->operation); move && move->window == client.id)
            return floating::drag_geometry(
                move->start_geometry,
                static_cast<int32_t>(drag_->last_x) - drag_->start_x,
                static_cast<int32_t>(drag_->last_y) - drag_->start_y,
                move->edges
            );
    return std::nullopt;
}

} // namespace lwm
