// Pointer interactions. A floating drag changes normal geometry as it moves;
// a tiled move is a presentation preview until release; a split drag changes
// the captured split's ratio.

#include "state.hpp"
#include "focus.hpp"
#include "lwm/config/config.hpp"

namespace lwm {

bool State::can_drag(xcb_window_t id) const
{
    auto const* client = find(id);
    return !drag_ && client && visible(*client) && !client->fullscreen && !showing_desktop_;
}

State::Press State::press(xcb_window_t window, int16_t x, int16_t y, uint8_t button, uint16_t modifiers, uint32_t time)
{
    using Edge = floating::ResizeEdge;
    auto const* client = find(window);
    if (client && !visible(*client))
        return { true };
    modifiers = binding_modifiers(modifiers);
    auto binding = std::ranges::find_if(
        config_.mousebinds, [&](auto const& bind) { return bind.button == button && bind.modifier == modifiers; }
    );
    if (binding != config_.mousebinds.end())
    {
        auto grip = [&](Edge edges)
        { return Press{ true, can_drag(window) ? std::optional<Interaction>{ Grip{ window, edges } } : std::nullopt }; };
        // Tiled and root clicks prefer a split; otherwise resize a floating
        // window or convert a tile once the pointer is held.
        if (binding->action == MouseAction::ResizeFloating && (!client || client->kind() == Client::Kind::Tiled))
            if (auto hit = split_at(x, y))
                return { true, *hit };
        if (client)
            switch (binding->action)
            {
                case MouseAction::DragWindow:
                    return grip(Edge::None);
                case MouseAction::ResizeFloating:
                    return grip(Edge::Right | Edge::Bottom);
                case MouseAction::ToggleFloat:
                    toggle_floating(window);
                    return { true };
            }
    }
    // Ordinary clicks focus the client and still reach it.
    if (client)
    {
        if (window != active_window_)
            focus(window);
        return { false };
    }
    auto hit = window == XCB_NONE && button == 1 && (modifiers & ~XCB_MOD_MASK_CONTROL) == 0 ? split_at(x, y) : std::nullopt;
    if (!hit)
    {
        hover(window, x, y);
        return { true };
    }
    auto previous = std::exchange(gap_click_, GapClick{ time, hit->hit.address, hit->monitor });
    auto elapsed = previous ? static_cast<int32_t>(time - previous->time) : 0;
    bool double_click =
        elapsed > 0 && elapsed < 400 && previous->address == hit->hit.address && previous->monitor == hit->monitor;
    if (!double_click && !(modifiers & XCB_MOD_MASK_CONTROL))
        return { true, *hit };
    erase_ratio(hit->monitor, hit->hit.address);
    gap_click_.reset();
    return { true };
}

std::optional<State::Interaction> State::moveresize(xcb_window_t id, floating::ResizeEdge edges) const
{
    auto const* client = find(id);
    if (!client || client->kind() != Client::Kind::Floating || !can_drag(id))
        return std::nullopt;
    return Grip{ id, edges };
}

void State::cancel_moveresize(xcb_window_t id)
{
    if (auto const* move = drag_ ? std::get_if<WindowDrag>(&drag_->operation) : nullptr; move && move->window == id)
        end_drag(false);
}

void State::begin_drag(Interaction const& interaction, int16_t x, int16_t y, uint8_t button)
{
    if (auto const* grip = std::get_if<Grip>(&interaction))
    {
        auto [id, edges] = *grip;
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
            geometry(id, frame(client));
            maximize(id, false, false);
        }
        WindowDrag drag{ id, client.kind(), client.monitor, client.workspace, normal_geometry(client), edges };
        drag_ = Drag{ drag, x, y, x, y, button };
        return;
    }
    auto const& hit = std::get<SplitHit>(interaction);
    if (drag_)
        return;
    auto const& monitor = monitors_[hit.monitor];
    SplitDrag drag{ hit.monitor,
                    monitor.current_workspace,
                    hit.hit,
                    monitor.working_area(),
                    monitor.current().layout_strategy,
                    tiled_participants(hit.monitor, fullscreen_visibility()) };
    drag_ = Drag{ drag, x, y, x, y, button };
}

std::optional<State::SplitHit> State::split_at(int16_t x, int16_t y) const
{
    auto index = monitor_at(monitors_, x, y);
    if (!index)
        return std::nullopt;
    auto const& monitor = monitors_[*index];
    auto count = tiled_participants(*index, fullscreen_visibility()).size();
    auto hit = layout().hit_test(count, monitor.working_area(), monitor.current(), x, y);
    return hit ? std::optional{ SplitHit{ *hit, *index } } : std::nullopt;
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
    auto target = monitor_at(monitors_, drag.last_x, drag.last_y).value_or(client->monitor);
    auto const& monitor = monitors_[target];
    auto const& workspace = monitor.current();
    auto participants = tiled_participants(target, fullscreen_visibility());
    std::erase(participants, client->id);
    auto area = monitor.working_area();
    size_t slot = layout().drop_target_index(participants.size() + 1, area, workspace, drag.last_x, drag.last_y);
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
