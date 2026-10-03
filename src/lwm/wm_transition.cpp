#include "lwm/core/invariants.hpp"
#include "lwm/core/classification.hpp"
#include "lwm/core/log.hpp"
#include "lwm/core/stacking.hpp"
#include "wm.hpp"
#include <algorithm>
#include <xcb/xcb_icccm.h>

namespace lwm {

// Resolve domain consequences before freezing State; publication only reads it.
// Compare complete projections with prior output, so mutations need no change lists.
void WindowManager::complete_transition()
{
    bool obligations = !configure_replies_.empty() || !events_.empty() || restack_requested_
        || drain_requested_ || presentation_dirty_;
    if (!obligations && state_.revision() == published_revision_)
        return;

    // Inputs and domain resolution; State may still change.
    auto focus_request = state_.settle(last_input_time_);
    if (!state_.drag())
        release_pointer();
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
    bool moved = publish_clients(clients);
    // Visible acknowledgements use the projection. Hidden clients still owe a
    // geometry reply even though they have no display rectangle.
    for (auto window : configure_replies_)
        if (auto const* client = state_.find(window))
            send_configure_notify(window, state_.presentation_geometry(*client), border_width(*client));
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
    ewmh_.update_window_states(states, state_atoms_);
    publish_fixtures();
    publish_root(clients, urgency_changed);
    reconcile_stacking(fullscreen, focus_request.has_value());
    withdraw_removed();
    if (drain_requested_ || (moved && !state_.drag()))
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

uint32_t WindowManager::border_width(Client const& client) const
{
    return client.fullscreen || client.borderless ? 0U : config().appearance.border_width;
}

uint32_t WindowManager::border_color(Client const& client) const
{
    if (client.id == state_.active_window())
        return config().appearance.border_color;
    if (client.urgency.active())
        return config().appearance.urgent_border_color;
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
    auto const& drag = state_.drag();
    bool resizing_tiles = drag && std::holds_alternative<State::SplitDrag>(drag->operation);
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

// Writes a property unless the same bytes were last written there; nullopt deletes it.
bool WindowManager::publish(
    xcb_window_t window, xcb_atom_t property, xcb_atom_t type, uint8_t format, std::optional<std::string_view> bytes
)
{
    auto [it, inserted] = properties_.try_emplace({ window, property });
    if (!inserted && (it->second && bytes ? *it->second == *bytes : !it->second && !bytes))
        return false;
    if (bytes)
        xcb_change_property(
            conn_.get(), XCB_PROP_MODE_REPLACE, window, property, type, format, bytes->size() * 8 / format, bytes->data()
        );
    else
        xcb_delete_property(conn_.get(), window, property);
    it->second = bytes;
    return true;
}

bool WindowManager::publish(xcb_window_t window, xcb_atom_t property, xcb_atom_t type, std::span<uint32_t const> words)
{
    return publish(window, property, type, 32, std::string_view(reinterpret_cast<char const*>(words.data()), words.size_bytes()));
}

// Per-window properties. Returns whether published urgency changed, which
// panels observe through a client-list notification.
bool WindowManager::publish_properties(Client const& client, Output& output, StateUpdates& updates)
{
    auto* e = ewmh_.get();
    xcb_window_t id = client.id;
    if (auto color = border_color(client); output.border_color != color)
    {
        xcb_change_window_attributes(conn_.get(), id, XCB_CW_BORDER_PIXEL, &color);
        output.border_color = color;
    }
    bool urgency_changed = output.urgent != client.urgency.active();
    if (urgency_changed)
        publish_urgency(client, output);
    uint32_t const desktop[] = { client.sticky ? 0xFFFFFFFF : state_.desktop_index(client.monitor, client.workspace) };
    publish(id, e->_NET_WM_DESKTOP, XCB_ATOM_CARDINAL, desktop);
    uint32_t const wm_state[] = { client.iconic ? WM_STATE_ICONIC : WM_STATE_NORMAL, 0 };
    publish(id, atoms_.wm_state, atoms_.wm_state, wm_state);
    std::vector<xcb_atom_t> actions = { e->_NET_WM_ACTION_CLOSE,      e->_NET_WM_ACTION_CHANGE_DESKTOP, e->_NET_WM_ACTION_MINIMIZE,
                                        e->_NET_WM_ACTION_STICK,      e->_NET_WM_ACTION_FULLSCREEN,     e->_NET_WM_ACTION_ABOVE,
                                        e->_NET_WM_ACTION_BELOW,      e->_NET_WM_ACTION_MAXIMIZE_VERT,  e->_NET_WM_ACTION_MAXIMIZE_HORZ };
    if (client.kind() == Client::Kind::Floating)
        actions.insert(actions.end(), { e->_NET_WM_ACTION_MOVE, e->_NET_WM_ACTION_RESIZE });
    publish(id, e->_NET_WM_ALLOWED_ACTIONS, XCB_ATOM_ATOM, actions);
    publish(id, atoms_.lwm_window_class, e->UTF8_STRING, 8, client_kind_str(client.kind()));
    if (auto const& m = client.fullscreen_monitors)
    {
        uint32_t const indices[] = { m->top, m->bottom, m->left, m->right };
        publish(id, e->_NET_WM_FULLSCREEN_MONITORS, XCB_ATOM_CARDINAL, indices);
    }
    else
        publish(id, e->_NET_WM_FULLSCREEN_MONITORS, XCB_ATOM_CARDINAL, 32, std::nullopt);
    if (auto states = published_states(client, id == state_.active_window()); output.states != states)
    {
        std::vector<xcb_atom_t> atoms;
        for (size_t i = 0; i < state_atoms_.size(); ++i)
            if (states.has(static_cast<WindowState>(i)))
                atoms.push_back(state_atoms_[i]);
        updates.emplace_back(id, std::move(atoms));
        output.states = states;
    }
    return urgency_changed;
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
        uint32_t const wm_state[] = { WM_STATE_NORMAL, 0 };
        publish(id, atoms_.wm_state, atoms_.wm_state, wm_state);
        publish(id, atoms_.lwm_window_class, ewmh_.get()->UTF8_STRING, 8, fixture_role_str(fixture.role));
        if (auto& output = outputs_[id]; !output.mapped)
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

void WindowManager::publish_root(std::vector<State::Projected> const& clients, bool urgency_changed)
{
    auto* e = ewmh_.get();
    xcb_window_t root = conn_.screen()->root;
    // Clients arrive in registration order; fixtures merge into it.
    std::vector<Fixture const*> fixtures;
    for (auto const& [id, fixture] : state_.fixtures()) fixtures.push_back(&fixture);
    std::ranges::sort(fixtures, { }, &Fixture::order);
    std::vector<xcb_window_t> client_list;
    auto fixture = fixtures.begin();
    for (auto const& projected : clients)
    {
        for (; fixture != fixtures.end() && (*fixture)->order < projected.client->order; ++fixture)
            client_list.push_back((*fixture)->id);
        client_list.push_back(projected.client->id);
    }
    for (; fixture != fixtures.end(); ++fixture) client_list.push_back((*fixture)->id);
    // Panels use client-list changes to refresh urgency.
    if (urgency_changed)
        properties_.erase({ root, e->_NET_CLIENT_LIST });
    publish(root, e->_NET_CLIENT_LIST, XCB_ATOM_WINDOW, client_list);

    // Monitor-major flat desktops. Workareas and viewports are relative to the
    // origin of the combined monitor bounds.
    auto const& monitors = state_.monitors();
    int32_t min_x = INT32_MAX, min_y = INT32_MAX, max_x = INT32_MIN, max_y = INT32_MIN;
    for (auto const& m : monitors)
    {
        min_x = std::min<int32_t>(min_x, m.geometry.x);
        min_y = std::min<int32_t>(min_y, m.geometry.y);
        max_x = std::max<int32_t>(max_x, m.geometry.x + m.geometry.width);
        max_y = std::max<int32_t>(max_y, m.geometry.y + m.geometry.height);
    }
    std::string names;
    std::vector<uint32_t> viewports, workareas;
    for (auto const& m : monitors)
    {
        Geometry area = m.working_area();
        for (auto const& name : config().workspaces.names)
        {
            names += name + '\0';
            viewports.insert(viewports.end(), { static_cast<uint32_t>(std::max(0, m.geometry.x - min_x)),
                                                static_cast<uint32_t>(std::max(0, m.geometry.y - min_y)) });
            workareas.insert(workareas.end(), { static_cast<uint32_t>(std::clamp(area.x - min_x, 0, INT16_MAX)),
                                                static_cast<uint32_t>(std::clamp(area.y - min_y, 0, INT16_MAX)),
                                                area.width,
                                                area.height });
        }
    }
    auto const& focused = monitors[state_.focused_monitor()];
    uint32_t const count[] = { static_cast<uint32_t>(monitors.size() * config().workspaces.count) };
    uint32_t const size[] = { static_cast<uint32_t>(std::max(1, max_x - min_x)), static_cast<uint32_t>(std::max(1, max_y - min_y)) };
    uint32_t const current[] = { state_.desktop_index(state_.focused_monitor(), focused.current_workspace) };
    uint32_t const active[] = { state_.active_window() };
    uint32_t const showing[] = { state_.showing_desktop() };
    publish(root, e->_NET_NUMBER_OF_DESKTOPS, XCB_ATOM_CARDINAL, count);
    publish(root, e->_NET_DESKTOP_NAMES, e->UTF8_STRING, 8, names);
    publish(root, e->_NET_DESKTOP_GEOMETRY, XCB_ATOM_CARDINAL, size);
    publish(root, e->_NET_DESKTOP_VIEWPORT, XCB_ATOM_CARDINAL, viewports);
    publish(root, e->_NET_WORKAREA, XCB_ATOM_CARDINAL, workareas);
    publish(root, e->_NET_CURRENT_DESKTOP, XCB_ATOM_CARDINAL, current);
    publish(root, e->_NET_ACTIVE_WINDOW, XCB_ATOM_WINDOW, active);
    publish(root, e->_NET_SHOWING_DESKTOP, XCB_ATOM_CARDINAL, showing);
}

// A changed desired order, a forwarded restack, or an explicit focus request
// reads the tree and repairs the server order, including external restacks.
// Other operations leave an unchanged order alone.
void WindowManager::reconcile_stacking(State::FullscreenVisibility const& fullscreen, bool reassert)
{
    auto order = stacking::compute_order(state_, fullscreen);
    if (!publish(conn_.screen()->root, ewmh_.get()->_NET_CLIENT_LIST_STACKING, XCB_ATOM_WINDOW, order) && !restack_requested_
        && !reassert)
        return;
    if (order.size() < 2)
        return;
    auto tree = xcb_query_tree_reply(conn_.get(), xcb_query_tree(conn_.get(), conn_.screen()->root), nullptr);
    std::vector<stacking::StackMove> moves;
    if (tree)
        moves = stacking::plan_moves(
            { xcb_query_tree_children(tree), static_cast<size_t>(xcb_query_tree_children_length(tree)) }, order
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
        uint32_t const withdrawn[] = { WM_STATE_WITHDRAWN, 0 };
        publish(it->first, atoms_.wm_state, atoms_.wm_state, withdrawn);
        properties_.erase(properties_.lower_bound({ it->first, 0 }), properties_.lower_bound({ it->first + 1, 0 }));
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
