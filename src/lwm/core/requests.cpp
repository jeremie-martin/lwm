// Policy for decoded application and pager requests. The shell translates X
// messages into these values and owns no decision about them.

#include "state.hpp"
#include "classification.hpp"
#include "log.hpp"

namespace lwm {

void State::request_states(xcb_window_t id, StateChange change, WindowStates requested)
{
    auto const* client = find(id);
    if (!client)
        return;
    auto enable = [change](bool current) { return change == StateChange::Toggle ? !current : change == StateChange::Add; };
    auto value = [&](WindowState state, bool current) { return requested.has(state) ? enable(current) : current; };
    bool fullscreen = value(WindowState::Fullscreen, client->fullscreen);
    bool horizontal = value(WindowState::MaximizedHorz, client->maximized_horz);
    bool vertical = value(WindowState::MaximizedVert, client->maximized_vert);
    if (!fullscreen && client->fullscreen)
        this->fullscreen(id, false);
    bool above_requested = requested.has(WindowState::Above);
    bool below_requested = requested.has(WindowState::Below);
    if (above_requested || below_requested)
    {
        auto current = client->preferences.layer.value_or(effective_layer(*client));
        bool above = value(WindowState::Above, current == LayerHint::Above);
        bool below = value(WindowState::Below, current == LayerHint::Below);
        if (above_requested && above)
            below = false;
        else if (below_requested && below)
            above = false;
        layer(id, above ? LayerHint::Above : below ? LayerHint::Below : LayerHint::Normal);
    }
    if (requested.has(WindowState::SkipTaskbar))
        skip_taskbar(id, enable(skips_taskbar(*client)));
    if (requested.has(WindowState::SkipPager))
        skip_pager(id, enable(skips_pager(*client)));
    if (requested.has(WindowState::Sticky))
        sticky(id, enable(client->sticky));
    if (requested.has(WindowState::Modal))
        modal(id, enable(client->modal));
    if (requested.has(WindowState::DemandsAttention))
        urgency(id, UrgencySource::App, enable(client->urgency.has(UrgencySource::App)));
    if (requested.has(WindowState::Hidden))
    {
        if (enable(client->iconic))
            iconic(id, true);
        else
            restore(id, false);
    }
    if (requested.has(WindowState::MaximizedHorz) || requested.has(WindowState::MaximizedVert))
        maximize(id, horizontal, vertical);
    if (fullscreen && requested.has(WindowState::Fullscreen))
        request_fullscreen(id);
}

void State::request_activation(xcb_window_t id, bool application, uint32_t timestamp)
{
    auto const* client = find(id);
    if (!client)
        return;
    auto deny = [&](char const* reason)
    {
        LWM_LOG_DEBUG("Activation rejected: window={:#x} reason={}", id, reason);
        if (application)
            urgency(id, UrgencySource::WmInitiated, true);
    };
    if (application && active_window_ != XCB_NONE && active_window_ != id)
    {
        if (timestamp == 0)
            return deny("missing-timestamp");
        auto const* current = find(active_window_);
        if (current && current->user_time != 0 && timestamp_is_before(timestamp, current->user_time))
            return deny("stale-timestamp");
    }
    bool shown = client->sticky || shows(client->monitor, client->workspace);
    if (shown && suppressed(*client))
        return deny("fullscreen-suppressed");
    focus(id, application ? timestamp : 0);
}

void State::request_desktop(xcb_window_t id, uint32_t desktop)
{
    if (!find(id))
        return;
    if (desktop == STICKY_DESKTOP)
        return sticky(id, true);
    if (auto placement = desktop_placement(desktop))
    {
        sticky(id, false);
        relocate(id, placement->first, placement->second, RelocationGeometry::Center);
        pin_desktop(id, true);
    }
}

void State::switch_desktop(uint32_t desktop)
{
    auto placement = desktop_placement(desktop);
    if (!placement)
        return;
    auto [monitor, workspace] = *placement;
    if (monitor == focused_monitor_ && workspace == monitors_[monitor].current_workspace)
        return;
    focus_monitor(monitor);
    // A switch on the now focused monitor chooses its own focus.
    if (!switch_workspace(monitor, workspace))
        focus_fallback(monitor);
}

// WM_HINTS also mirrors the urgency LWM publishes, so only a hint that differs
// from it is the application's request, and the echo of LWM's own write is not.
void State::hint_urgency(xcb_window_t id, bool urgent)
{
    if (id != active_window_ && urgent != require(id).urgency.active())
        urgency(id, UrgencySource::App, urgent);
}

// Requests name the X window rectangle inside the client's normal frame.
void State::configure_request(xcb_window_t id, GeometryRequest request)
{
    auto const* client = find(id);
    if (client && !client->fullscreen && (request.x || request.y || request.width || request.height))
        moveresize_request(id, request);
}

void State::moveresize_request(xcb_window_t id, GeometryRequest request)
{
    auto const* client = find(id);
    auto const* floating = client ? floating_mode(*client) : nullptr;
    if (!floating)
        return;
    auto frame = resize_frame(*client, floating->geometry, request.width, request.height);
    frame.x = request.x.value_or(frame.x);
    frame.y = request.y.value_or(frame.y);
    request_geometry(id, frame);
}

} // namespace lwm
