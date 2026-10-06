#include "lwm/core/xproperty.hpp"
#include "lwm/core/invariants.hpp"
#include "lwm/core/classification.hpp"
#include "lwm/core/log.hpp"
#include "lwm/core/stacking.hpp"
#include "wm.hpp"
#include <algorithm>
#include <ranges>
#include <xcb/xcb_icccm.h>

namespace lwm {

// Resolve domain consequences before freezing State; publication only reads it.
// Compare complete projections with prior output, so mutations need no change lists.
void WindowManager::complete_transition()
{
    bool obligations = !configure_replies_.empty() || restack_requested_
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
    for (size_t m = 0; m < std::max(owners.size(), fullscreen_owners_.size()); ++m)
    {
        auto before = m < fullscreen_owners_.size() ? fullscreen_owners_[m] : XCB_NONE;
        auto after = m < owners.size() ? owners[m] : XCB_NONE;
        if (before != after)
            LWM_LOG_DEBUG("Fullscreen owner changed: monitor={} window={:#x} -> {:#x}", m, before, after);
    }
    fullscreen_owners_ = owners;
    auto clients = state_.project(fullscreen);
    bool moved = publish_clients(clients);
    // ConfigureRequests that no geometry write acknowledged, including those of
    // hidden clients, are answered with the presentation they would have.
    for (auto window : configure_replies_)
        if (auto const* client = state_.find(window))
            send_configure_notify(window, state_.presentation(*client));
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
    ewmh_.update_window_states(states, WindowStates{ UINT16_MAX }); // LWM owns every value it publishes
    publish_fixtures();
    publish_root(clients, urgency_changed);
    reconcile_stacking(fullscreen, focus_request.has_value());
    withdraw_removed();
    if (drain_requested_ || (moved && !state_.drag()))
        flush_and_drain_crossing();
    conn_.flush();
    state_.thaw();

    configure_replies_.clear();
    restack_requested_ = false;
    drain_requested_ = false;
    presentation_dirty_ = false;
    LWM_ASSERT_INVARIANTS(state_);
}

// Client publication

// Hides, shows, configures and maps clients. Returns whether anything moved
// under the pointer, whether tiled or floating.
bool WindowManager::publish_clients(std::vector<State::Projected> const& clients)
{
    bool moved = false;
    for (auto const& [client, presentation] : clients)
    {
        auto& output = outputs_[client->id];
        if (presentation || output.hidden)
            continue;
        output.hidden = true;
        output.presentation.reset();
        uint32_t x = static_cast<uint32_t>(OFF_SCREEN_X);
        xcb_configure_window(conn_.get(), client->id, XCB_CONFIG_WINDOW_X, &x);
        moved = true;
    }
    // Keep a split resize's configure requests together on the server.
    auto const& drag = state_.drag();
    bool resizing_tiles = drag && std::holds_alternative<State::SplitDrag>(drag->operation);
    if (resizing_tiles)
        xcb_grab_server(conn_.get());
    for (auto const& [client, presentation] : clients)
    {
        if (!presentation)
            continue;
        if (write_geometry(client->id, outputs_.at(client->id), *presentation))
        {
            moved = true;
            configure_replies_.erase(client->id);
        }
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

// Owns WM-driven configure requests and synthetic ConfigureNotify replies.
// An unchanged rectangle and border are skipped.
bool WindowManager::write_geometry(xcb_window_t window, Output& output, State::Presentation const& presentation)
{
    if (!output.hidden && output.presentation == presentation)
        return false;
    auto [geometry, border] = presentation;
    LWM_LOG_TRACE(
        "Geometry submitted: window={:#x} x={} y={} width={} height={} border={}",
        window,
        geometry.x,
        geometry.y,
        geometry.width,
        geometry.height,
        border
    );
    output.hidden = false;
    output.presentation = presentation;
    uint32_t values[] = { static_cast<uint32_t>(geometry.x),
                          static_cast<uint32_t>(geometry.y),
                          geometry.width,
                          geometry.height,
                          border };
    xcb_configure_window(
        conn_.get(),
        window,
        XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y | XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT
            | XCB_CONFIG_WINDOW_BORDER_WIDTH,
        values
    );
    send_configure_notify(window, presentation);
    return true;
}

void WindowManager::send_configure_notify(xcb_window_t window, State::Presentation const& presentation)
{
    auto [geometry, border] = presentation;
    xcb_configure_notify_event_t event{ };
    event.response_type = XCB_CONFIGURE_NOTIFY;
    event.event = window;
    event.window = window;
    event.x = geometry.x;
    event.y = geometry.y;
    event.width = geometry.width;
    event.height = geometry.height;
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
    if (bytes && !conn_.fits_property(bytes->size()))
    {
        LWM_LOG_WARN_LIMIT(std::chrono::seconds(5), "Property too large to publish: window={:#x} bytes={}", window, bytes->size());
        if (inserted)
            properties_.erase(it);
        return false;
    }
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
    if (auto color = state_.border_color(client); output.border_color != color)
    {
        xcb_change_window_attributes(conn_.get(), id, XCB_CW_BORDER_PIXEL, &color);
        output.border_color = color;
    }
    bool urgency_changed = output.urgent != client.urgency.active();
    if (urgency_changed)
        publish_urgency(client, output);
    uint32_t const desktop[] = { client.sticky ? STICKY_DESKTOP : state_.desktop_index(client.monitor, client.workspace) };
    publish(id, e->_NET_WM_DESKTOP, XCB_ATOM_CARDINAL, desktop);
    uint32_t const wm_state[] = { client.iconic ? XCB_ICCCM_WM_STATE_ICONIC : XCB_ICCCM_WM_STATE_NORMAL, 0 };
    publish(id, atoms_.wm_state, atoms_.wm_state, wm_state);
    // Floating clients add the trailing move and resize actions.
    xcb_atom_t const actions[] = { e->_NET_WM_ACTION_CLOSE,         e->_NET_WM_ACTION_CHANGE_DESKTOP,
                                   e->_NET_WM_ACTION_STICK,         e->_NET_WM_ACTION_FULLSCREEN,
                                   e->_NET_WM_ACTION_ABOVE,         e->_NET_WM_ACTION_BELOW,
                                   e->_NET_WM_ACTION_MAXIMIZE_VERT, e->_NET_WM_ACTION_MAXIMIZE_HORZ,
                                   e->_NET_WM_ACTION_MOVE,          e->_NET_WM_ACTION_RESIZE };
    auto allowed = std::span(actions).first(!client.tiled() ? 10 : 8);
    publish(id, e->_NET_WM_ALLOWED_ACTIONS, XCB_ATOM_ATOM, allowed);
    publish(id, atoms_.lwm_window_class, e->UTF8_STRING, 8, client_kind_str(client));
    if (auto const& m = client.fullscreen_monitors)
    {
        uint32_t const indices[] = { m->top, m->bottom, m->left, m->right };
        publish(id, e->_NET_WM_FULLSCREEN_MONITORS, XCB_ATOM_CARDINAL, indices);
    }
    else
        publish(id, e->_NET_WM_FULLSCREEN_MONITORS, XCB_ATOM_CARDINAL, 32, std::nullopt);
    if (auto states = published_states(client, id == state_.active_window()); output.states != states)
    {
        updates.emplace_back(id, states);
        output.states = states;
    }
    return urgency_changed;
}

// Mirrors urgency into ICCCM WM_HINTS for panels that read it.
void WindowManager::publish_urgency(Client const& client, Output& output)
{
    bool urgent = client.urgency.active();
    output.urgent = urgent;
    xcb_icccm_wm_hints_t hints{ };
    bool present =
        xcb_icccm_get_wm_hints_reply(conn_.get(), xcb_icccm_get_wm_hints(conn_.get(), client.id), &hints, nullptr);
    if ((!present && !urgent) || (xcb_icccm_wm_hints_get_urgency(&hints) != 0) == urgent)
        return;
    if (urgent)
        xcb_icccm_wm_hints_set_urgency(&hints);
    else
        hints.flags &= ~XCB_ICCCM_WM_HINT_X_URGENCY;
    xcb_icccm_set_wm_hints(conn_.get(), client.id, &hints);
}

void WindowManager::publish_fixtures()
{
    for (auto const& [id, fixture] : state_.fixtures())
    {
        uint32_t const wm_state[] = { XCB_ICCCM_WM_STATE_NORMAL, 0 };
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
    auto desktop = *bounds(monitors | std::views::transform(&Monitor::geometry));
    std::string names;
    std::vector<uint32_t> viewports, workareas;
    for (auto const& m : monitors)
    {
        Geometry area = m.working_area();
        for (auto const& name : config().workspaces)
        {
            names.append(name).push_back('\0');
            viewports.insert(viewports.end(), { static_cast<uint32_t>(std::max(0, m.geometry.x - desktop.x)),
                                                static_cast<uint32_t>(std::max(0, m.geometry.y - desktop.y)) });
            workareas.insert(workareas.end(), { static_cast<uint32_t>(std::clamp(area.x - desktop.x, 0, INT16_MAX)),
                                                static_cast<uint32_t>(std::clamp(area.y - desktop.y, 0, INT16_MAX)),
                                                area.width,
                                                area.height });
        }
    }
    auto const& focused = monitors[state_.focused_monitor()];
    uint32_t const count[] = { static_cast<uint32_t>(monitors.size() * config().workspaces.size()) };
    uint32_t const size[] = { desktop.width, desktop.height };
    uint32_t const current[] = { state_.desktop_index(state_.focused_monitor(), focused.current_workspace) };
    uint32_t const active[] = { state_.active_window() };
    publish(root, e->_NET_NUMBER_OF_DESKTOPS, XCB_ATOM_CARDINAL, count);
    publish(root, e->_NET_DESKTOP_NAMES, e->UTF8_STRING, 8, names);
    publish(root, e->_NET_DESKTOP_GEOMETRY, XCB_ATOM_CARDINAL, size);
    publish(root, e->_NET_DESKTOP_VIEWPORT, XCB_ATOM_CARDINAL, viewports);
    publish(root, e->_NET_WORKAREA, XCB_ATOM_CARDINAL, workareas);
    publish(root, e->_NET_CURRENT_DESKTOP, XCB_ATOM_CARDINAL, current);
    publish(root, e->_NET_ACTIVE_WINDOW, XCB_ATOM_WINDOW, active);
    // Drags change geometry, which the exposed state omits, every motion; the
    // completion that ends a drag publishes its monitor or ratio outcome.
    if (!state_.drag())
        publish(wm_window_, atoms_.lwm_state, e->UTF8_STRING, 8, state_json());
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
    auto tree = reply(xcb_query_tree_reply(conn_.get(), xcb_query_tree(conn_.get(), conn_.screen()->root), nullptr));
    std::vector<stacking::StackMove> moves;
    if (tree)
        moves = stacking::plan_moves(
            { xcb_query_tree_children(tree.get()), static_cast<size_t>(xcb_query_tree_children_length(tree.get())) },
            order
        );
    else
        for (size_t i = 1; i < order.size(); ++i) moves.push_back({ order[i], order[i - 1], XCB_STACK_MODE_ABOVE });
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
        uint32_t const withdrawn[] = { XCB_ICCCM_WM_STATE_WITHDRAWN, 0 };
        publish(it->first, atoms_.wm_state, atoms_.wm_state, withdrawn);
        properties_.erase(properties_.lower_bound({ it->first, 0 }), properties_.lower_bound({ it->first + 1, 0 }));
        if (it->second.states)
            unfocused.emplace_back(it->first, WindowStates{ });
        it = outputs_.erase(it);
    }
    WindowStates focused;
    focused.set(WindowState::Focused);
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

} // namespace lwm
