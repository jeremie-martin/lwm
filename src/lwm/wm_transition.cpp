#include "lwm/core/invariants.hpp"
#include "lwm/core/log.hpp"
#include "lwm/core/policy.hpp"
#include "lwm/core/stacking.hpp"
#include "wm.hpp"
#include <algorithm>
#include <xcb/xcb_icccm.h>

namespace lwm {

// Resolve domain consequences before freezing State; publication only reads it.
// Compare complete projections with prior output, so mutations need no change lists.
void WindowManager::complete_transition()
{
    bool obligations = workareas_dirty_ || !configure_replies_.empty() || !events_.empty() || restack_requested_
        || drain_requested_ || presentation_dirty_;
    if (!obligations && state_.revision() == published_revision_)
        return;

    // Inputs and domain resolution; State may still change.
    if (std::exchange(workareas_dirty_, false))
        refresh_workareas();
    validate_drag();
    auto focus_request = state_.complete_focus(last_input_time_);
    auto fullscreen = state_.fullscreen_visibility();
    auto const& owners = fullscreen.owners;
    published_revision_ = state_.revision();

    // Publication reads a frozen model.
    state_.freeze();
    for (size_t m = 0; m < std::max(owners.size(), root_.fullscreen_owners.size()); ++m)
    {
        auto before = m < root_.fullscreen_owners.size() ? root_.fullscreen_owners[m] : XCB_NONE;
        auto after = m < owners.size() ? owners[m] : XCB_NONE;
        if (before != after)
            LWM_LOG_DEBUG("Fullscreen owner changed: monitor={} window={:#x} -> {:#x}", m, before, after);
    }
    root_.fullscreen_owners = owners;
    auto clients = state_.project(fullscreen);
    for (auto& projected : clients)
        if (projected.geometry)
            if (auto preview = drag_preview(*projected.client))
                projected.geometry = preview;
    bool moved = publish_clients(clients);
    // Visible acknowledgements use the projection. Hidden clients still owe a
    // geometry reply even though they have no display rectangle.
    for (auto window : configure_replies_)
        if (auto const* client = state_.find(window))
            send_configure_notify(window, presentation_geometry(*client), border_width(*client));
    if (focus_request)
    {
        commit_focus(*focus_request);
        // Explicit focus reasserts the focused state even if a client rewrote it.
        if (auto it = outputs_.find(state_.active_window()); it != outputs_.end())
            it->second.states.reset();
    }
    bool urgency_changed = false;
    StateUpdates states;
    for (auto const& projected : clients)
        urgency_changed |= publish_properties(*projected.client, outputs_.at(projected.client->id), states);
    ewmh_.update_window_states(states, owned_state_atoms());
    publish_fixtures();
    publish_root(clients, urgency_changed);
    reconcile_stacking(fullscreen, focus_request.has_value());
    withdraw_removed();
    if (drain_requested_ || (moved && !drag_active()))
        flush_and_drain_crossing();
    conn_.flush();
    emit_events(focus_request.has_value());
    state_.thaw();

    configure_replies_.clear();
    restack_requested_ = false;
    drain_requested_ = false;
    presentation_dirty_ = false;
    LWM_ASSERT_INVARIANTS(state_);
}

// Layout

// The drag preview is the only geometry input outside the domain model.
std::optional<Geometry> WindowManager::drag_preview(Client const& client) const
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

Geometry WindowManager::presentation_geometry(Client const& client) const
{
    if (auto preview = drag_preview(client))
        return *preview;
    return state_.presentation_geometry(client);
}

uint32_t WindowManager::border_width(Client const& client) const
{
    return client.fullscreen || client.borderless ? 0U : config_.appearance.border_width;
}

uint32_t WindowManager::border_color(Client const& client) const
{
    if (client.id == state_.active_window())
        return config_.appearance.border_color;
    if (client.urgency.active())
        return config_.appearance.urgent_border_color;
    return conn_.screen()->black_pixel;
}

// Client publication

// Hides, shows, configures and maps clients. Returns whether anything moved
// under the pointer, whether tiled or floating.
bool WindowManager::publish_clients(std::vector<State::Projected> const& clients)
{
    bool moved = false;
    for (auto const& [client, geometry] : clients)
    {
        auto& output = outputs_[client->id];
        if (geometry || output.hidden)
            continue;
        output.hidden = true;
        output.geometry.reset();
        uint32_t x = static_cast<uint32_t>(OFF_SCREEN_X);
        xcb_configure_window(conn_.get(), client->id, XCB_CONFIG_WINDOW_X, &x);
        moved = true;
    }
    // Keep a split resize's configure requests together on the server.
    bool resizing_tiles = drag_ && std::holds_alternative<TiledResize>(drag_->operation);
    if (resizing_tiles)
        xcb_grab_server(conn_.get());
    for (auto const& [client, geometry] : clients)
    {
        if (!geometry)
            continue;
        bool changed = write_geometry(*client, outputs_.at(client->id), *geometry, border_width(*client));
        moved |= changed;
        if (configure_replies_.erase(client->id) && !changed)
            send_configure_notify(client->id, *geometry, border_width(*client));
    }
    if (resizing_tiles)
        xcb_ungrab_server(conn_.get());
    for (auto const& projected : clients)
        if (auto& output = outputs_.at(projected.client->id); !output.mapped)
        {
            xcb_map_window(conn_.get(), projected.client->id);
            output.mapped = true;
        }
    return moved;
}

// Owns WM-driven configure requests, sync notifications and synthetic
// ConfigureNotify replies. An unchanged rectangle and border are skipped.
bool WindowManager::write_geometry(Client const& client, Output& output, Geometry geometry, uint32_t border)
{
    geometry.width = std::max<uint16_t>(1, geometry.width);
    geometry.height = std::max<uint16_t>(1, geometry.height);
    if (!output.hidden && output.geometry == geometry && output.border_width == border)
        return false;
    LWM_LOG_TRACE(
        "Geometry submitted: window={:#x} x={} y={} width={} height={} border={}",
        client.id,
        geometry.x,
        geometry.y,
        geometry.width,
        geometry.height,
        border
    );
    output.hidden = false;
    output.geometry = geometry;
    output.border_width = border;
    if (output.sync_counter)
    {
        uint64_t value = ++output.sync_value;
        send_protocol_message(
            client.id,
            ewmh_.get()->_NET_WM_SYNC_REQUEST,
            last_event_time_,
            static_cast<uint32_t>(value),
            static_cast<uint32_t>(value >> 32)
        );
    }
    uint32_t values[] = { static_cast<uint32_t>(geometry.x),
                          static_cast<uint32_t>(geometry.y),
                          geometry.width,
                          geometry.height,
                          border };
    xcb_configure_window(
        conn_.get(),
        client.id,
        XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y | XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT
            | XCB_CONFIG_WINDOW_BORDER_WIDTH,
        values
    );
    send_configure_notify(client.id, geometry, border);
    return true;
}

void WindowManager::send_configure_notify(xcb_window_t window, Geometry geometry, uint32_t border)
{
    xcb_configure_notify_event_t event{ };
    event.response_type = XCB_CONFIGURE_NOTIFY;
    event.event = window;
    event.window = window;
    event.x = geometry.x;
    event.y = geometry.y;
    event.width = std::max<uint16_t>(1, geometry.width);
    event.height = std::max<uint16_t>(1, geometry.height);
    event.border_width = static_cast<uint16_t>(border);
    xcb_send_event(conn_.get(), 0, window, XCB_EVENT_MASK_STRUCTURE_NOTIFY, reinterpret_cast<char*>(&event));
}

// Per-window properties. Returns whether published urgency changed, which
// panels observe through a client-list notification.
bool WindowManager::publish_properties(Client const& client, Output& output, StateUpdates& states)
{
    xcb_window_t id = client.id;
    if (auto color = border_color(client); output.border_color != color)
    {
        xcb_change_window_attributes(conn_.get(), id, XCB_CW_BORDER_PIXEL, &color);
        output.border_color = color;
    }
    bool urgency_changed = output.urgent != client.urgency.active();
    if (urgency_changed)
        publish_urgency(client, output);
    uint32_t desktop = client.sticky ? 0xFFFFFFFF : desktop_index(client.monitor, client.workspace);
    if (output.desktop != desktop)
    {
        xcb_ewmh_set_wm_desktop(ewmh_.get(), id, desktop);
        output.desktop = desktop;
    }
    uint32_t wm_state = client.iconic ? WM_STATE_ICONIC : WM_STATE_NORMAL;
    if (output.wm_state != wm_state)
    {
        uint32_t data[] = { wm_state, 0 };
        xcb_change_property(conn_.get(), XCB_PROP_MODE_REPLACE, id, atoms_.wm_state, atoms_.wm_state, 32, 2, data);
        output.wm_state = wm_state;
    }
    if (char const* kind = client_kind_str(client.kind()); output.window_class != kind)
    {
        auto* e = ewmh_.get();
        std::vector<xcb_atom_t> actions = { e->_NET_WM_ACTION_CLOSE,          e->_NET_WM_ACTION_CHANGE_DESKTOP,
                                            e->_NET_WM_ACTION_MINIMIZE,       e->_NET_WM_ACTION_STICK,
                                            e->_NET_WM_ACTION_FULLSCREEN,     e->_NET_WM_ACTION_ABOVE,
                                            e->_NET_WM_ACTION_BELOW,          e->_NET_WM_ACTION_MAXIMIZE_VERT,
                                            e->_NET_WM_ACTION_MAXIMIZE_HORZ };
        if (client.kind() == Client::Kind::Floating)
        {
            actions.push_back(e->_NET_WM_ACTION_MOVE);
            actions.push_back(e->_NET_WM_ACTION_RESIZE);
        }
        xcb_ewmh_set_wm_allowed_actions(ewmh_.get(), id, actions.size(), actions.data());
        publish_window_class(id, kind);
        output.window_class = kind;
    }
    if (output.fullscreen_monitors != client.fullscreen_monitors)
    {
        if (auto const& m = client.fullscreen_monitors)
            xcb_ewmh_set_wm_fullscreen_monitors(ewmh_.get(), id, m->top, m->bottom, m->left, m->right);
        else if (output.fullscreen_monitors)
            xcb_delete_property(conn_.get(), id, ewmh_.get()->_NET_WM_FULLSCREEN_MONITORS);
        output.fullscreen_monitors = client.fullscreen_monitors;
    }
    auto layer = effective_layer(client);
    // Same order as owned_state_atoms(); bit i of Output::states is atom i.
    bool const enabled[] = { client.fullscreen,
                             layer == LayerHint::Above,
                             layer == LayerHint::Below,
                             client.sticky,
                             client.modal,
                             skips_taskbar(client),
                             skips_pager(client),
                             client.maximized_horz,
                             client.maximized_vert,
                             client.iconic,
                             client.urgency.active(),
                             id == state_.active_window() };
    uint32_t bits = 0;
    for (size_t i = 0; i < std::size(enabled); ++i) bits |= enabled[i] ? 1U << i : 0;
    if (output.states != bits)
    {
        auto owned = owned_state_atoms();
        std::vector<xcb_atom_t> atoms;
        for (size_t i = 0; i < owned.size(); ++i)
            if (enabled[i])
                atoms.push_back(owned[i]);
        states.emplace_back(id, std::move(atoms));
        output.states = bits;
    }
    return urgency_changed;
}

// The _NET_WM_STATE atoms LWM owns; other atoms on a window are preserved.
std::array<xcb_atom_t, 12> WindowManager::owned_state_atoms() const
{
    auto* e = ewmh_.get();
    return { e->_NET_WM_STATE_FULLSCREEN,       e->_NET_WM_STATE_ABOVE,          e->_NET_WM_STATE_BELOW,
             e->_NET_WM_STATE_STICKY,           e->_NET_WM_STATE_MODAL,          e->_NET_WM_STATE_SKIP_TASKBAR,
             e->_NET_WM_STATE_SKIP_PAGER,       e->_NET_WM_STATE_MAXIMIZED_HORZ, e->_NET_WM_STATE_MAXIMIZED_VERT,
             e->_NET_WM_STATE_HIDDEN,           e->_NET_WM_STATE_DEMANDS_ATTENTION, atoms_.net_wm_state_focused };
}

void WindowManager::publish_window_class(xcb_window_t window, char const* kind)
{
    xcb_change_property(
        conn_.get(),
        XCB_PROP_MODE_REPLACE,
        window,
        atoms_.lwm_window_class,
        ewmh_.get()->UTF8_STRING,
        8,
        static_cast<uint32_t>(std::strlen(kind)),
        kind
    );
}

// Mirrors urgency into ICCCM WM_HINTS for panels that read it. An LWM-only
// write echoes back as PropertyNotify; the echo must not look app-originated.
void WindowManager::publish_urgency(Client const& client, Output& output)
{
    bool urgent = client.urgency.active();
    output.urgent = urgent;
    if (!urgent)
        output.ignore_urgency_echo = false;
    bool arm_echo = urgent && client.urgency.has(UrgencySource::WmInitiated) && !client.urgency.has(UrgencySource::App);
    xcb_icccm_wm_hints_t hints{ };
    bool present =
        xcb_icccm_get_wm_hints_reply(conn_.get(), xcb_icccm_get_wm_hints(conn_.get(), client.id), &hints, nullptr);
    if (!present && !urgent)
        return;
    if (((hints.flags & XUrgencyHint) != 0) == urgent)
        return;
    if (urgent)
        hints.flags |= XUrgencyHint;
    else
        hints.flags &= ~XUrgencyHint;
    output.ignore_urgency_echo = arm_echo;
    xcb_icccm_set_wm_hints(conn_.get(), client.id, &hints);
}

void WindowManager::publish_fixtures()
{
    for (auto const& [id, fixture] : state_.fixtures())
    {
        auto& output = outputs_[id];
        if (output.wm_state != WM_STATE_NORMAL)
        {
            uint32_t data[] = { WM_STATE_NORMAL, 0 };
            xcb_change_property(conn_.get(), XCB_PROP_MODE_REPLACE, id, atoms_.wm_state, atoms_.wm_state, 32, 2, data);
            output.wm_state = WM_STATE_NORMAL;
        }
        if (char const* role = fixture_role_str(fixture.role); output.window_class != role)
        {
            publish_window_class(id, role);
            output.window_class = role;
        }
        if (!output.mapped)
        {
            xcb_map_window(conn_.get(), id);
            output.mapped = true;
        }
    }
}

void WindowManager::commit_focus(uint32_t time)
{
    // Explicit same-window focus still reasserts input focus and WM_TAKE_FOCUS.
    if (auto const* client = state_.find(state_.active_window()))
    {
        if (client->supports_take_focus)
            send_protocol_message(client->id, atoms_.wm_take_focus, time ? time : last_event_time_);
        xcb_set_input_focus(conn_.get(), XCB_INPUT_FOCUS_POINTER_ROOT, client->id, time);
    }
    else
        xcb_set_input_focus(conn_.get(), XCB_INPUT_FOCUS_POINTER_ROOT, conn_.screen()->root, XCB_CURRENT_TIME);
}

// Root publication

uint32_t WindowManager::desktop_index(size_t monitor, size_t workspace) const
{
    return ewmh_policy::desktop_index(monitor, workspace, config_.workspaces.count);
}

// Monitor-major flat desktops. Workareas and viewports are relative to the
// origin of the combined monitor bounds.
WindowManager::DesktopLayout WindowManager::desktop_layout() const
{
    auto const& monitors = state_.monitors();
    int32_t min_x = monitors.front().geometry.x, min_y = monitors.front().geometry.y;
    int32_t max_x = min_x, max_y = min_y;
    for (auto const& m : monitors)
    {
        min_x = std::min<int32_t>(min_x, m.geometry.x);
        min_y = std::min<int32_t>(min_y, m.geometry.y);
        max_x = std::max<int32_t>(max_x, m.geometry.x + m.geometry.width);
        max_y = std::max<int32_t>(max_y, m.geometry.y + m.geometry.height);
    }
    DesktopLayout layout;
    layout.count = static_cast<uint32_t>(monitors.size() * config_.workspaces.count);
    layout.width = static_cast<uint32_t>(std::max<int32_t>(1, max_x - min_x));
    layout.height = static_cast<uint32_t>(std::max<int32_t>(1, max_y - min_y));
    for (auto const& m : monitors)
    {
        Geometry area = m.working_area();
        area.x = static_cast<int16_t>(std::clamp<int32_t>(area.x - min_x, 0, std::numeric_limits<int16_t>::max()));
        area.y = static_cast<int16_t>(std::clamp<int32_t>(area.y - min_y, 0, std::numeric_limits<int16_t>::max()));
        std::pair<uint32_t, uint32_t> viewport{ static_cast<uint32_t>(std::max<int32_t>(0, m.geometry.x - min_x)),
                                                static_cast<uint32_t>(std::max<int32_t>(0, m.geometry.y - min_y)) };
        for (auto const& name : config_.workspaces.names)
        {
            layout.names.push_back(name);
            layout.viewports.push_back(viewport);
            layout.workareas.push_back(area);
        }
    }
    return layout;
}

void WindowManager::publish_root(std::vector<State::Projected> const& clients, bool urgency_changed)
{
    // Clients arrive in registration order; fixtures merge into it.
    std::vector<Fixture const*> fixtures;
    for (auto const& [id, fixture] : state_.fixtures()) fixtures.push_back(&fixture);
    std::ranges::sort(fixtures, { }, &Fixture::order);
    std::vector<xcb_window_t> client_list;
    client_list.reserve(clients.size() + fixtures.size());
    auto fixture = fixtures.begin();
    for (auto const& projected : clients)
    {
        for (; fixture != fixtures.end() && (*fixture)->order < projected.client->order; ++fixture)
            client_list.push_back((*fixture)->id);
        client_list.push_back(projected.client->id);
    }
    for (; fixture != fixtures.end(); ++fixture) client_list.push_back((*fixture)->id);
    // Panels use client-list changes to refresh urgency.
    if (client_list != root_.client_list || urgency_changed)
    {
        xcb_ewmh_set_client_list(ewmh_.get(), 0, client_list.size(), client_list.data());
        root_.client_list = std::move(client_list);
    }
    if (auto layout = desktop_layout(); root_.desktops != layout)
    {
        xcb_ewmh_set_number_of_desktops(ewmh_.get(), 0, layout.count);
        ewmh_.set_desktop_names(layout.names);
        xcb_ewmh_set_desktop_geometry(ewmh_.get(), 0, layout.width, layout.height);
        ewmh_.set_desktop_viewport(layout.viewports);
        ewmh_.set_workarea(layout.workareas);
        root_.desktops = std::move(layout);
    }
    auto const& focused = state_.monitors()[state_.focused_monitor()];
    if (auto desktop = desktop_index(state_.focused_monitor(), focused.current_workspace); root_.current_desktop != desktop)
    {
        xcb_ewmh_set_current_desktop(ewmh_.get(), 0, desktop);
        root_.current_desktop = desktop;
    }
    if (root_.active != state_.active_window())
    {
        xcb_ewmh_set_active_window(ewmh_.get(), 0, state_.active_window());
        root_.active = state_.active_window();
    }
    if (root_.showing_desktop != state_.showing_desktop())
    {
        xcb_ewmh_set_showing_desktop(ewmh_.get(), 0, state_.showing_desktop());
        root_.showing_desktop = state_.showing_desktop();
    }
}

// A changed desired order, a forwarded restack, or an explicit focus request
// reads the tree and repairs the server order, including external restacks.
// Other operations leave an unchanged order alone.
void WindowManager::reconcile_stacking(State::FullscreenVisibility const& fullscreen, bool reassert)
{
    auto order = stacking::compute_order(state_, fullscreen);
    if (order == root_.stacking && !restack_requested_ && !reassert)
        return;
    if (order.size() > 1)
    {
        auto tree = xcb_query_tree_reply(conn_.get(), xcb_query_tree(conn_.get(), conn_.screen()->root), nullptr);
        std::vector<stacking::StackMove> moves;
        if (tree)
            moves = stacking::plan_moves(
                { xcb_query_tree_children(tree), static_cast<size_t>(xcb_query_tree_children_length(tree)) },
                order
            );
        else
            for (size_t i = 1; i < order.size(); ++i) moves.push_back({ order[i], order[i - 1], XCB_STACK_MODE_ABOVE });
        free(tree);
        for (auto const& move : moves)
        {
            uint32_t values[] = { move.sibling, move.mode };
            xcb_configure_window(conn_.get(), move.window, XCB_CONFIG_WINDOW_SIBLING | XCB_CONFIG_WINDOW_STACK_MODE, values);
        }
    }
    if (order != root_.stacking)
    {
        xcb_ewmh_set_client_list_stacking(ewmh_.get(), 0, order.size(), order.data());
        root_.stacking = std::move(order);
    }
}

// Unmanaged windows are withdrawn: WM_STATE says so and the focused state no
// longer applies. Other state atoms stay for a later remap.
void WindowManager::withdraw_removed()
{
    StateUpdates unfocused;
    for (auto it = outputs_.begin(); it != outputs_.end();)
    {
        if (state_.find(it->first) || state_.find_fixture(it->first))
        {
            ++it;
            continue;
        }
        uint32_t withdrawn[] = { WM_STATE_WITHDRAWN, 0 };
        xcb_change_property(conn_.get(), XCB_PROP_MODE_REPLACE, it->first, atoms_.wm_state, atoms_.wm_state, 32, 2, withdrawn);
        if (it->second.states)
            unfocused.emplace_back(it->first, std::vector<xcb_atom_t>{ });
        it = outputs_.erase(it);
    }
    xcb_atom_t const focused[] = { atoms_.net_wm_state_focused };
    ewmh_.update_window_states(unfocused, focused);
}

// A round trip makes the server generate crossing events for what LWM just
// moved; they are dropped so stale pointer focus cannot override the result.
// Other events are deferred to the outer loop, which never re-enters handlers.
void WindowManager::flush_and_drain_crossing()
{
    conn_.sync();
    while (auto* event = xcb_poll_for_queued_event(conn_.get()))
    {
        uint8_t type = event->response_type & ~0x80;
        if (type != XCB_ENTER_NOTIFY && type != XCB_LEAVE_NOTIFY && type != XCB_MOTION_NOTIFY)
            deferred_events_.push_back(*event);
        free(event);
    }
}

// Subscription events

void WindowManager::queue_event(Event event) { events_.push_back(std::move(event)); }

// Workspace switches and focus are derived from what changed since the last
// operation; other facts were recorded where they happened. state_change
// fires when the exposed state snapshot actually differs.
void WindowManager::emit_events(bool focus_requested)
{
    auto const& monitors = state_.monitors();
    std::map<std::string, size_t> workspaces;
    for (size_t m = 0; m < monitors.size(); ++m)
    {
        auto current = monitors[m].current_workspace;
        if (auto it = root_.workspaces.find(monitors[m].name); it != root_.workspaces.end() && it->second != current)
            ipc_.emit(event::workspace_switch{ m, it->second, current });
        workspaces.emplace(monitors[m].name, current);
    }
    root_.workspaces = std::move(workspaces);
    if (auto const* active = state_.find(state_.active_window()); active && focus_requested)
        ipc_.emit(event::focus_change{ active->id, active->wm_class, active->name });
    // Map/unmap facts precede outcomes; each group retains occurrence order.
    for (bool mappings : { true, false })
        for (auto const& event : events_)
            if ((std::holds_alternative<event::window_map>(event) || std::holds_alternative<event::window_unmap>(event)) == mappings)
                ipc_.emit(event);
    events_.clear();
    // Only a new revision can change the exposed state.
    if (ipc_.has_subscribers(event_mask<event::state_change>) && state_.revision() != root_.snapshot_revision)
    {
        root_.snapshot_revision = state_.revision();
        if (auto snapshot = state_json(); snapshot != root_.snapshot)
        {
            root_.snapshot = std::move(snapshot);
            ipc_.emit(event::state_change{});
        }
    }
}

} // namespace lwm
