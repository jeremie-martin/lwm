#include "lwm/core/focus.hpp"
#include "lwm/core/log.hpp"
#include "lwm/core/policy.hpp"
#include "wm.hpp"
#include <algorithm>
#include <xcb/xcb_icccm.h>

namespace lwm {

// Adoption and classification

void WindowManager::scan_existing_windows(bool handoff)
{
    // Observe the display before restoring placement. All dock reservations must
    // be known when a saved floating client is fitted to a changed topology.
    std::vector<Client> clients;
    auto* tree = xcb_query_tree_reply(conn_.get(), xcb_query_tree(conn_.get(), conn_.screen()->root), nullptr);
    if (tree)
    {
        auto* children = xcb_query_tree_children(tree);
        for (int i = 0; i < xcb_query_tree_children_length(tree); ++i)
        {
            auto window = children[i];
            auto* attributes =
                xcb_get_window_attributes_reply(conn_.get(), xcb_get_window_attributes(conn_.get(), window), nullptr);
            bool adopt = attributes && attributes->map_state == XCB_MAP_STATE_VIEWABLE && !attributes->override_redirect;
            free(attributes);
            if (!adopt)
                continue;
            if (auto candidate = admit_window(window, true))
                clients.push_back(std::move(*candidate));
        }
        free(tree);
    }
    if (std::exchange(workareas_dirty_, false))
        refresh_workareas();
    // Restore the surviving graph before newcomers choose their placement.
    // Application properties are live observations; private intent comes from
    // the handoff. Docks above already supplied the discovered workareas.
    if (handoff_)
        state_.restore_graph(*handoff_, std::move(clients));
    else
        for (auto& candidate : clients)
        {
            auto window = candidate.id;
            state_.insert(std::move(candidate));
            if (!floating_mode(state_.require(window)))
                place_new_client(window);
        }

    // Registration and tiled rules establish the complete layout before any
    // initial floating placement. Resolve floating parents before their children;
    // visiting each ID once also bounds malformed transient cycles.
    auto order = state_.clients_by_order();
    for (auto const* client : order)
        if (handoff_ && !floating_mode(*client) && !handoff_->find(client->id))
            place_new_client(client->id);
    std::unordered_set<xcb_window_t> pending;
    for (auto const* client : order)
        if (floating_mode(*client) && !(handoff_ && handoff_->find(client->id)))
            pending.insert(client->id);
    for (auto const* client : order)
    {
        std::vector<xcb_window_t> chain;
        for (auto next = client->id; pending.erase(next); next = state_.require(next).transient_for)
            chain.push_back(next);
        for (auto it = chain.rbegin(); it != chain.rend(); ++it)
            place_new_client(*it);
    }

    if (!handoff)
    {
        auto* pointer =
            xcb_query_pointer_reply(conn_.get(), xcb_query_pointer(conn_.get(), conn_.screen()->root), nullptr);
        if (pointer)
            state_.focus_monitor(
                focus::monitor_index_at_point(state_.monitors(), pointer->root_x, pointer->root_y).value_or(0)
            );
        free(pointer);
    }
    auto const* active = handoff_ ? state_.find(handoff_->active) : nullptr;
    if (active && state_.focusable(*active))
        state_.focus(active->id, 0, false);
    else
        state_.focus_fallback(state_.focused_monitor(), false);
    if (handoff_)
    {
        // Saved claims precede pending launches that mapped while the WM was absent.
        for (auto const* client : state_.clients_by_order())
            state_.claim_pending_scratchpad(client->id, config_.scratchpads);
        handoff_.reset();
    }
    if (!handoff)
        for (auto const& command : config_.autostart) launch_program(command, "autostart");
}

void WindowManager::manage_fixture(xcb_window_t window, Fixture::Role role, bool adopting)
{
    uint32_t mask = role == Fixture::Role::Dock
        ? XCB_EVENT_MASK_ENTER_WINDOW | XCB_EVENT_MASK_POINTER_MOTION | XCB_EVENT_MASK_PROPERTY_CHANGE
        : XCB_EVENT_MASK_PROPERTY_CHANGE;
    xcb_change_window_attributes(conn_.get(), window, XCB_CW_EVENT_MASK, &mask);
    if (role == Fixture::Role::Desktop)
    {
        // Desktops start below every sibling, not just below managed clients.
        uint32_t below = XCB_STACK_MODE_BELOW;
        xcb_configure_window(conn_.get(), window, XCB_CONFIG_WINDOW_STACK_MODE, &below);
    }
    state_.insert_fixture(window, role);
    outputs_[window].mapped = adopting;
    if (role == Fixture::Role::Dock)
        workareas_dirty_ = true;
}

// Client registration

// Initial placement and later hint changes share the same geometry policy.
// Only initial placement follows the parent's workspace and centers by default.
void WindowManager::apply_size_hints(xcb_window_t window, bool initial)
{
    auto const& client = state_.require(window);
    if (!floating_mode(client))
        return;
    bool anchored = client.transient_for != XCB_NONE;
    auto parent_geometry = initial && anchored ? placement_parent_geometry(client.transient_for) : std::nullopt;
    if (initial && anchored)
        if (auto const* parent = state_.find(client.transient_for))
            state_.relocate(window, parent->monitor, parent->workspace);
    auto geometry = floating_mode(client)->geometry;
    auto hints = read_size_hints(window, anchored);
    geometry.width = hints.width.value_or(geometry.width);
    geometry.height = hints.height.value_or(geometry.height);
    if (initial)
    {
        geometry.width = std::max<uint16_t>(1, geometry.width);
        geometry.height = std::max<uint16_t>(1, geometry.height);
    }
    auto monitor = client.monitor;
    bool center = initial;
    if (hints.position)
    {
        Geometry hinted{ hints.position->first, hints.position->second, geometry.width, geometry.height };
        auto target = floating::resolve_position_hint(state_.monitors(), monitor, anchored || client.desktop_pinned, hinted);
        monitor = target.monitor;
        center = !target.accepted;
        if (target.accepted)
        {
            geometry = hinted;
            if (initial && monitor != client.monitor)
                state_.relocate(window, monitor, state_.monitors()[monitor].current_workspace);
        }
    }
    if (center)
    {
        if (!initial && anchored)
            parent_geometry = placement_parent_geometry(client.transient_for);
        geometry = floating::place_floating(
            state_.monitors()[monitor].working_area(), geometry.width, geometry.height, parent_geometry
        );
    }
    if (initial)
        state_.geometry(window, geometry);
    else
        update_floating_geometry(client, geometry);
}

// Position hints of anchored (transient) windows count only when the user supplied them.
WindowManager::SizeHints WindowManager::read_size_hints(xcb_window_t window, bool anchored) const
{
    SizeHints result;
    xcb_size_hints_t hints;
    if (!xcb_icccm_get_wm_normal_hints_reply(conn_.get(), xcb_icccm_get_wm_normal_hints(conn_.get(), window), &hints, nullptr))
        return result;
    if ((hints.flags & XCB_ICCCM_SIZE_HINT_US_POSITION) || ((hints.flags & XCB_ICCCM_SIZE_HINT_P_POSITION) && !anchored))
        result.position = { geometry_coordinate(hints.x), geometry_coordinate(hints.y) };
    // Nonpositive sizes keep the current extent; oversized ones saturate.
    if (hints.flags & (XCB_ICCCM_SIZE_HINT_US_SIZE | XCB_ICCCM_SIZE_HINT_P_SIZE))
    {
        if (hints.width > 0)
            result.width = geometry_extent(hints.width);
        if (hints.height > 0)
            result.height = geometry_extent(hints.height);
    }
    return result;
}

// Admission owns classification, application observations and X resources.
// Startup defers client registration until every dock reservation is known.
std::optional<Client> WindowManager::admit_window(xcb_window_t window, bool adopting)
{
    if (auto const* fixture = handoff_ ? handoff_->find_fixture(window) : nullptr)
    {
        manage_fixture(window, fixture->role, adopting);
        return std::nullopt;
    }
    Client client;
    client.id = window;
    client.transient_for = read_transient_for(window).value_or(XCB_NONE);
    std::tie(client.wm_class_name, client.wm_class) = read_wm_class(window);
    client.name = read_window_name(window);
    client.ewmh_type = ewmh_.get_window_type_enum(window);
    auto const* rule = match_window_rules(config_.rules, client);
    client.rule = rule ? std::optional{ *rule } : std::nullopt;
    auto natural = default_floating(client);
    // Established ownership wins; metadata chooses a role only for newcomers.
    if (!natural && !(handoff_ && handoff_->find(window)))
    {
        if (client.ewmh_type == WindowType::Desktop || client.ewmh_type == WindowType::Dock)
            manage_fixture(window, client.ewmh_type == WindowType::Dock ? Fixture::Role::Dock : Fixture::Role::Desktop, adopting);
        else if (!adopting)
            xcb_map_window(conn_.get(), window);
        return std::nullopt;
    }
    if (rule && rule->floating ? *rule->floating : natural.value_or(false))
        client.mode = FloatingMode{ read_window_geometry(window).value_or(Geometry{ 0, 0, 300, 200 }) };
    LWM_LOG_DEBUG(
        "Client observed: window={:#x} transient_for={:#x} natural={} resolved={} rules_matched={}",
        window,
        client.transient_for,
        natural ? (*natural ? "floating" : "tiled") : "retained",
        client_kind_str(client.kind()),
        rule != nullptr
    );
    client.monitor = state_.focused_monitor();
    client.workspace = state_.monitors()[client.monitor].current_workspace;
    if (auto desktop = read_window_desktop(window))
    {
        if (*desktop == 0xFFFFFFFF)
            client.sticky = true;
        else if (auto placement = ewmh_policy::desktop_placement(*desktop, config_.workspaces.count, state_.monitors().size()))
        {
            std::tie(client.monitor, client.workspace) = *placement;
            client.desktop_pinned = true;
        }
        else
            LWM_LOG_WARN("Ignoring out-of-range _NET_WM_DESKTOP: window={:#x} desktop={}", window, *desktop);
    }
    auto& output = outputs_[window];
    output.mapped = adopting;
    auto* ewmh = ewmh_.get();
    xcb_ewmh_get_atoms_reply_t states;
    if (xcb_ewmh_get_wm_state_reply(ewmh, xcb_ewmh_get_wm_state(ewmh, client.id), &states, nullptr))
    {
        auto atoms = std::span(states.atoms, states.atoms_len);
        auto has = [&](xcb_atom_t atom) { return std::ranges::contains(atoms, atom); };
        if (has(ewmh->_NET_WM_STATE_ABOVE) || has(ewmh->_NET_WM_STATE_BELOW))
            client.preferences.layer = has(ewmh->_NET_WM_STATE_ABOVE) ? LayerHint::Above : LayerHint::Below;
        if (has(ewmh->_NET_WM_STATE_SKIP_TASKBAR))
            client.preferences.skip_taskbar = true;
        if (has(ewmh->_NET_WM_STATE_SKIP_PAGER))
            client.preferences.skip_pager = true;
        client.sticky |= has(ewmh->_NET_WM_STATE_STICKY);
        client.modal = has(ewmh->_NET_WM_STATE_MODAL);
        client.fullscreen = has(ewmh->_NET_WM_STATE_FULLSCREEN);
        client.maximized_horz = has(ewmh->_NET_WM_STATE_MAXIMIZED_HORZ);
        client.maximized_vert = has(ewmh->_NET_WM_STATE_MAXIMIZED_VERT);
        client.iconic = has(ewmh->_NET_WM_STATE_HIDDEN);
        if (has(ewmh->_NET_WM_STATE_DEMANDS_ATTENTION))
            client.urgency.add(UrgencySource::App);
        xcb_ewmh_get_atoms_reply_wipe(&states);
    }
    bool hinted_urgent = false;
    xcb_icccm_wm_hints_t hints;
    if (xcb_icccm_get_wm_hints_reply(conn_.get(), xcb_icccm_get_wm_hints(conn_.get(), client.id), &hints, nullptr))
    {
        client.accepts_input = !(hints.flags & XCB_ICCCM_WM_HINT_INPUT) || hints.input;
        // Adoption preserves the current minimized state instead of the initial hint.
        if (!adopting && (hints.flags & XCB_ICCCM_WM_HINT_STATE)
            && hints.initial_state == XCB_ICCCM_WM_STATE_ICONIC)
            client.iconic = true;
        hinted_urgent = (hints.flags & XUrgencyHint) != 0;
        if (hinted_urgent)
            client.urgency.add(UrgencySource::App);
    }
    client.supports_take_focus = supports_protocol(client.id, atoms_.wm_take_focus);
    client.user_time_window = read_user_time_window(client.id);
    client.user_time = read_user_time(client.id, client.user_time_window);
    client.fullscreen_monitors = read_fullscreen_monitors(client.id);
    output.urgent = hinted_urgent;
    output.fullscreen_monitors.emplace(client.fullscreen_monitors);
    uint32_t mask = kManagedWindowEventMask;
    xcb_change_window_attributes(conn_.get(), window, XCB_CW_EVENT_MASK, &mask);
    xcb_grab_button(
        conn_.get(),
        0,
        window,
        XCB_EVENT_MASK_BUTTON_PRESS,
        XCB_GRAB_MODE_SYNC,
        XCB_GRAB_MODE_ASYNC,
        XCB_NONE,
        XCB_NONE,
        XCB_BUTTON_INDEX_ANY,
        XCB_MOD_MASK_ANY
    );
    xcb_ewmh_set_frame_extents(ewmh_.get(), window, 0, 0, 0, 0);
    read_sync_counter(window);

    if (adopting)
        return client; // Startup registers the complete scene before floating placement.
    auto scratchpad = state_.match_scratchpad(client, config_.scratchpads);
    if (scratchpad)
    {
        client.iconic = true;
        if (!floating_mode(client))
            client.mode = FloatingMode{ read_window_geometry(window).value_or(Geometry{ 0, 0, 300, 200 }) };
    }
    state_.insert(std::move(client));
    place_new_client(window);
    auto const& admitted = state_.require(window);
    if (admitted.monitor == state_.focused_monitor() && state_.focusable(admitted))
        state_.focus(window);
    if (scratchpad)
        state_.claim_scratchpad(window, *scratchpad);
    return std::nullopt;
}

// Both live admission and startup use the same placement, after registration.
void WindowManager::place_new_client(xcb_window_t window)
{
    apply_size_hints(window, true);
    state_.apply_initial_rule(window);
}

// Managed parents use their intended presentation; server reads are for unmanaged parents.
std::optional<Geometry> WindowManager::placement_parent_geometry(xcb_window_t window) const
{
    if (auto const* client = state_.find(window))
        return presentation_geometry(*client);
    return read_window_geometry(window);
}

// A floating window whose center moves onto another monitor joins that
// monitor's current workspace.
void WindowManager::follow_floating_geometry(xcb_window_t window)
{
    auto const& client = state_.require(window);
    auto const* floating = floating_mode(client);
    if (!floating)
        return;
    auto monitor = floating::monitor_at_center(state_.monitors(), floating->geometry);
    if (!monitor || *monitor == client.monitor)
        return;
    state_.relocate(window, *monitor, state_.monitors()[*monitor].current_workspace);
}

} // namespace lwm
