#include "lwm/core/focus.hpp"
#include "lwm/core/log.hpp"
#include "lwm/core/policy.hpp"
#include "wm.hpp"
#include <algorithm>
#include <xcb/xcb_icccm.h>

namespace lwm {

namespace {

char const* classification_name(WindowClassification::Kind kind)
{
    switch (kind)
    {
        case WindowClassification::Kind::Tiled:
            return "tiled";
        case WindowClassification::Kind::Floating:
            return "floating";
        case WindowClassification::Kind::Dock:
            return "dock";
        case WindowClassification::Kind::Desktop:
            return "desktop";
        case WindowClassification::Kind::Popup:
            return "popup";
    }
    return "unknown";
}

} // namespace

// ---------------------------------------------------------------------------
// Adoption and classification
// ---------------------------------------------------------------------------

void WindowManager::scan_existing_windows(bool handoff)
{
    // Windows without a saved record join the restored current workspace.
    if (handoff_)
        state_.restore_workspaces(*handoff_);
    auto* tree = xcb_query_tree_reply(conn_.get(), xcb_query_tree(conn_.get(), conn_.screen()->root), nullptr);
    if (tree)
    {
        auto* children = xcb_query_tree_children(tree);
        for (int i = 0; i < xcb_query_tree_children_length(tree); ++i)
        {
            auto* attributes = xcb_get_window_attributes_reply(
                conn_.get(),
                xcb_get_window_attributes(conn_.get(), children[i]),
                nullptr
            );
            bool adopt = attributes && attributes->map_state == XCB_MAP_STATE_VIEWABLE && !attributes->override_redirect;
            free(attributes);
            if (adopt)
                manage_window(children[i], true);
        }
        free(tree);
    }

    if (handoff_)
    {
        state_.restore_membership(*handoff_);
        if (auto const* active = state_.find(handoff_->active); active && state_.focusable(*active))
            focus_window(active->id, false);
        else
            focus_fallback(state_.focused_monitor(), false);
        handoff_.reset();
        return;
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
    focus_fallback(state_.focused_monitor(), false);
    if (!handoff)
        for (auto const& command : config_.autostart.commands) launch_program(command, "autostart");
}

// One set of property values drives classification, rules and registration.
ClassificationResult WindowManager::classify_window(xcb_window_t window)
{
    ClassificationResult result;
    result.transient_for = read_transient_for(window).value_or(XCB_NONE);
    auto [instance, name] = read_wm_class(window);
    result.properties = { std::move(name),
                          std::move(instance),
                          read_window_name(window),
                          ewmh_.get_window_type_enum(window),
                          result.transient_for != XCB_NONE };
    result.classification = classify_window_type(result.properties.ewmh_type, result.properties.is_transient);
    result.rule = match_window_rules(config_.rules, result.properties);
    auto natural = result.classification.kind;
    bool normal = natural == WindowClassification::Kind::Tiled || natural == WindowClassification::Kind::Floating;
    if (normal && result.rule && result.rule->floating)
        result.classification.kind =
            *result.rule->floating ? WindowClassification::Kind::Floating : WindowClassification::Kind::Tiled;
    LWM_LOG_DEBUG(
        "Classification resolved: window={:#x} transient_for={:#x} natural={} resolved={} rules_matched={}",
        window,
        result.transient_for,
        classification_name(natural),
        classification_name(result.classification.kind),
        result.rule != nullptr
    );
    return result;
}

void WindowManager::manage_window(xcb_window_t window, bool adopting)
{
    auto initial = classify_window(window);
    switch (initial.classification.kind)
    {
        case WindowClassification::Kind::Desktop:
            manage_fixture(window, Fixture::Role::Desktop, adopting);
            return;
        case WindowClassification::Kind::Dock:
            manage_fixture(window, Fixture::Role::Dock, adopting);
            return;
        case WindowClassification::Kind::Popup:
            // Popup-only types are mapped directly and never registered.
            if (!adopting)
                xcb_map_window(conn_.get(), window);
            return;
        case WindowClassification::Kind::Tiled:
        case WindowClassification::Kind::Floating:
            break;
    }
    // A new window claimed by a named scratchpad starts hidden and floating, so
    // it never enters the tiled layout.
    auto scratchpad = adopting ? std::nullopt : match_scratchpad(initial.properties, initial.rule);
    if (scratchpad)
        initial.classification.kind = WindowClassification::Kind::Floating;
    manage_client(window, initial, scratchpad.has_value(), adopting);
    if (scratchpad)
        claim_scratchpad(window, *scratchpad);
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

// ---------------------------------------------------------------------------
// Client registration
// ---------------------------------------------------------------------------

void WindowManager::read_initial_state(Client& client, bool honor_initial_state)
{
    auto* ewmh = ewmh_.get();
    xcb_ewmh_get_atoms_reply_t states;
    if (xcb_ewmh_get_wm_state_reply(ewmh, xcb_ewmh_get_wm_state(ewmh, client.id), &states, nullptr))
    {
        for (uint32_t i = 0; i < states.atoms_len; ++i)
        {
            xcb_atom_t atom = states.atoms[i];
            if (atom == ewmh->_NET_WM_STATE_ABOVE)
                client.preferences.layer = LayerHint::Above;
            else if (atom == ewmh->_NET_WM_STATE_BELOW && client.preferences.layer != LayerHint::Above)
                client.preferences.layer = LayerHint::Below;
            else if (atom == ewmh->_NET_WM_STATE_STICKY)
                client.sticky = true;
            else if (atom == ewmh->_NET_WM_STATE_MODAL)
                client.modal = true;
            else if (atom == ewmh->_NET_WM_STATE_SKIP_TASKBAR)
                client.preferences.skip_taskbar = true;
            else if (atom == ewmh->_NET_WM_STATE_SKIP_PAGER)
                client.preferences.skip_pager = true;
            else if (atom == ewmh->_NET_WM_STATE_FULLSCREEN)
                client.fullscreen = true;
            else if (atom == ewmh->_NET_WM_STATE_MAXIMIZED_HORZ)
                client.maximized_horz = true;
            else if (atom == ewmh->_NET_WM_STATE_MAXIMIZED_VERT)
                client.maximized_vert = true;
            else if (atom == ewmh->_NET_WM_STATE_HIDDEN)
                client.iconic = true;
            else if (atom == ewmh->_NET_WM_STATE_DEMANDS_ATTENTION)
                client.urgency.add(UrgencySource::App);
        }
        xcb_ewmh_get_atoms_reply_wipe(&states);
    }
    // Fullscreen supersedes maximize.
    if (client.fullscreen)
        client.maximized_horz = client.maximized_vert = false;

    xcb_icccm_wm_hints_t hints;
    if (xcb_icccm_get_wm_hints_reply(conn_.get(), xcb_icccm_get_wm_hints(conn_.get(), client.id), &hints, nullptr))
    {
        client.accepts_input = !(hints.flags & XCB_ICCCM_WM_HINT_INPUT) || hints.input;
        // Adoption preserves the current minimized state instead of the initial hint.
        if (honor_initial_state && (hints.flags & XCB_ICCCM_WM_HINT_STATE)
            && hints.initial_state == XCB_ICCCM_WM_STATE_ICONIC)
            client.iconic = true;
        if (hints.flags & XUrgencyHint)
            client.urgency.add(UrgencySource::App);
    }
    client.supports_take_focus = supports_protocol(client.id, atoms_.wm_take_focus);
    client.user_time_window = read_user_time_window(client.id);
    client.user_time = read_user_time(client.id, client.user_time_window);
    client.fullscreen_monitors = read_fullscreen_monitors(client.id);
}

// A concrete _NET_WM_DESKTOP places the client; a missing or sticky hint means
// the focused monitor's current workspace.
Client WindowManager::initial_client(xcb_window_t window, ClassificationResult const& initial)
{
    Client client;
    client.id = window;
    client.monitor = state_.focused_monitor();
    client.workspace = state_.monitors()[client.monitor].current_workspace;
    if (auto desktop = read_window_desktop(window))
    {
        if (*desktop == 0xFFFFFFFF)
            client.sticky = true;
        else if (auto indices = ewmh_policy::desktop_to_indices(*desktop, config_.workspaces.count);
                 indices && indices->first < state_.monitors().size())
        {
            client.monitor = indices->first;
            client.workspace = indices->second;
            client.desktop_pinned = true;
        }
        else
            LWM_LOG_WARN("Ignoring out-of-range _NET_WM_DESKTOP: window={:#x} desktop={}", window, *desktop);
    }
    client.name = initial.properties.title;
    client.wm_class = initial.properties.wm_class;
    client.wm_class_name = initial.properties.wm_class_name;
    client.ewmh_type = initial.properties.ewmh_type;
    client.transient_for = initial.transient_for;
    if (initial.classification.kind == WindowClassification::Kind::Floating)
        client.mode = FloatingMode{ initial_floating_geometry(window, initial, client) };
    else
        // A tile keeps its requested rectangle until its workspace is arranged.
        client.mode = TiledMode{ std::nullopt, read_window_geometry(window).value_or(Geometry{ 0, 0, 300, 200 }) };
    return client;
}

// Transients are centered on their managed parent's workspace. Position hints
// may choose the monitor of an unanchored window; see X11.md.
Geometry WindowManager::initial_floating_geometry(xcb_window_t window, ClassificationResult const& initial, Client& candidate)
{
    std::optional<Geometry> parent_geometry;
    bool transient = initial.transient_for != XCB_NONE;
    if (transient)
    {
        if (auto const* parent = state_.find(initial.transient_for))
        {
            candidate.monitor = parent->monitor;
            candidate.workspace = parent->workspace;
        }
        parent_geometry = placement_parent_geometry(initial.transient_for);
    }

    auto geometry = read_window_geometry(window).value_or(Geometry{ 0, 0, 300, 200 });
    std::optional<std::pair<int16_t, int16_t>> position;
    xcb_size_hints_t hints;
    if (xcb_icccm_get_wm_normal_hints_reply(conn_.get(), xcb_icccm_get_wm_normal_hints(conn_.get(), window), &hints, nullptr))
    {
        // User positions always apply; program positions only for untransient windows.
        if ((hints.flags & XCB_ICCCM_SIZE_HINT_US_POSITION) || ((hints.flags & XCB_ICCCM_SIZE_HINT_P_POSITION) && !transient))
            position = { geometry_coordinate(hints.x), geometry_coordinate(hints.y) };
        if (hints.flags & (XCB_ICCCM_SIZE_HINT_US_SIZE | XCB_ICCCM_SIZE_HINT_P_SIZE))
        {
            if (hints.width > 0)
                geometry.width = geometry_extent(hints.width);
            if (hints.height > 0)
                geometry.height = geometry_extent(hints.height);
        }
    }
    geometry.width = std::max<uint16_t>(1, geometry.width);
    geometry.height = std::max<uint16_t>(1, geometry.height);

    auto const& monitors = state_.monitors();
    if (position)
    {
        Geometry hinted{ position->first, position->second, geometry.width, geometry.height };
        if (!transient && !candidate.desktop_pinned)
            if (auto monitor = focus::monitor_index_at_point(
                    monitors,
                    static_cast<int32_t>(hinted.x) + hinted.width / 2,
                    static_cast<int32_t>(hinted.y) + hinted.height / 2
                ))
            {
                candidate.monitor = *monitor;
                candidate.workspace = monitors[*monitor].current_workspace;
            }
        if (floating::hint_targets_monitor(monitors[candidate.monitor].geometry(), hinted.x, hinted.y, hinted.width, hinted.height))
            return hinted;
    }
    return floating::place_floating(monitors[candidate.monitor].working_area(), geometry.width, geometry.height, parent_geometry);
}

void WindowManager::manage_client(xcb_window_t window, ClassificationResult const& initial, bool start_iconic, bool adopting)
{
    // Placement below reads workareas; docks registered earlier in this operation count.
    if (std::exchange(workareas_dirty_, false))
        refresh_workareas();
    Client candidate = initial_client(window, initial);
    read_initial_state(candidate, !adopting);
    candidate.iconic |= start_iconic;

    auto const* saved = adopting && handoff_ ? handoff_->find(window) : nullptr;
    if (saved && saved->monitor < state_.monitors().size()
        && saved->workspace < state_.monitors()[saved->monitor].workspaces.size())
    {
        // Restart restores saved intent directly rather than replaying rules.
        candidate.monitor = saved->monitor;
        candidate.workspace = saved->workspace;
        if (saved->kind == Client::Kind::Floating)
            candidate.mode = FloatingMode{ saved->geometry };
        else
            candidate.mode = TiledMode{ saved->floating, saved->geometry };
        candidate.preferences = saved->preferences;
        candidate.urgency.sources = saved->urgency;
        candidate.borderless = saved->borderless;
        candidate.desktop_pinned = saved->desktop_pinned;
    }
    else
        saved = nullptr;
    state_.insert(std::move(candidate));

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
    ewmh_.set_frame_extents(window, 0, 0, 0, 0);
    read_sync_counter(window);
    outputs_[window].mapped = adopting;

    // The matched rule is remembered either way, so later metadata changes
    // apply rules only when the result changes.
    state_.rule(window, initial.rule ? std::optional{ *initial.rule } : std::nullopt);
    if (initial.rule && !saved)
        apply_rule(window, *initial.rule);

    auto const& client = state_.require(window);
    if (!adopting && client.monitor == state_.focused_monitor() && state_.focusable(client))
        focus_window(window);
}

void WindowManager::unmanage_window(xcb_window_t window)
{
    if (auto const* fixture = state_.find_fixture(window))
    {
        if (fixture->role == Fixture::Role::Dock)
            workareas_dirty_ = true;
        state_.erase(window);
        return;
    }
    auto const* client = state_.find(window);
    if (!client)
        return;
    size_t monitor = client->monitor;
    bool active = window == state_.active_window();
    pending_kills_.erase(window);
    state_.erase(window);
    if (!active)
        return;
    if (monitor == state_.focused_monitor())
        focus_fallback(monitor);
    else
        clear_focus();
}

// ---------------------------------------------------------------------------
// Rules
// ---------------------------------------------------------------------------

// Rules apply only the actions they specify.
void WindowManager::apply_rule(xcb_window_t window, RuleActions const& rule)
{
    LWM_LOG_DEBUG("Applying matched rule: window={:#x}", window);
    if (rule.floating)
        state_.floating(window, *rule.floating);

    auto const& client = state_.require(window);
    auto monitor = resolve_rule_monitor(rule, state_.monitors());
    if (monitor || rule.workspace)
    {
        size_t target = monitor.value_or(client.monitor);
        size_t workspace = std::min(rule.workspace.value_or(client.workspace), state_.monitors()[target].workspaces.size() - 1);
        if ((target != client.monitor || workspace != client.workspace)
            && state_.relocate(window, target, workspace, State::RelocationGeometry::Center)
            && window == state_.active_window())
            state_.remember_focus(window);
    }

    if (auto const* floating = floating_mode(state_.require(window)))
    {
        auto geometry = rule.geometry.value_or(floating->geometry);
        if (rule.center)
        {
            Geometry area = state_.monitors()[client.monitor].working_area();
            geometry.x = static_cast<int16_t>(area.x + (area.width - geometry.width) / 2);
            geometry.y = static_cast<int16_t>(area.y + (area.height - geometry.height) / 2);
        }
        state_.geometry(window, geometry);
        state_.configure_suppression(window, rule.geometry || rule.center);
    }

    if (rule.skip_taskbar)
        state_.skip_taskbar(window, *rule.skip_taskbar);
    if (rule.skip_pager)
        state_.skip_pager(window, *rule.skip_pager);
    if (rule.sticky)
        state_.sticky(window, *rule.sticky);
    if (rule.layer)
        state_.layer(window, *rule.layer);
    if (rule.borderless)
        state_.borderless(window, *rule.borderless);
    if (rule.fullscreen)
        set_fullscreen(window, *rule.fullscreen);
}

// Metadata changes apply a rule only when its result changes; losing a match
// leaves previous actions in place. A pending named scratchpad can claim the
// window regardless.
void WindowManager::reevaluate_metadata(xcb_window_t window)
{
    auto const& client = state_.require(window);
    auto properties = window_match_info(client);
    auto const* rule = match_window_rules(config_.rules, properties);
    std::optional<RuleActions> current = rule ? std::optional{ *rule } : std::nullopt;
    bool changed = current != client.rule;
    state_.rule(window, current);
    if (claim_pending_scratchpad(window, properties, rule))
        return;
    if (rule && changed)
        apply_rule(window, *rule);
}

// Explicit reload reapplies every matching rule, including unchanged placement.
void WindowManager::reapply_rules()
{
    std::vector<Client const*> clients;
    for (auto const& [id, client] : state_.clients()) clients.push_back(&client);
    std::ranges::sort(clients, { }, &Client::order);
    std::vector<xcb_window_t> order;
    for (auto const* client : clients) order.push_back(client->id);
    for (auto window : order)
    {
        auto const* rule = match_window_rules(config_.rules, window_match_info(state_.require(window)));
        state_.rule(window, rule ? std::optional{ *rule } : std::nullopt);
        if (rule)
            apply_rule(window, *rule);
    }
    presentation_dirty_ = true;
}

void WindowManager::relocate_to_transient_parent(xcb_window_t window, xcb_window_t previous_transient_for)
{
    auto const& client = state_.require(window);
    if (client.transient_for == previous_transient_for || client.transient_for == XCB_NONE)
        return;
    auto const* parent = state_.find(client.transient_for);
    if (!parent || !state_.relocate(window, parent->monitor, parent->workspace))
        return;
    if (auto const* floating = floating_mode(state_.require(window)))
        state_.geometry(
            window,
            floating::place_floating(
                state_.monitors()[parent->monitor].working_area(),
                floating->geometry.width,
                floating->geometry.height,
                placement_parent_geometry(client.transient_for)
            )
        );
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
    auto const& g = floating->geometry;
    auto monitor = focus::monitor_index_at_point(
        state_.monitors(),
        static_cast<int32_t>(g.x) + g.width / 2,
        static_cast<int32_t>(g.y) + g.height / 2
    );
    if (!monitor || *monitor == client.monitor)
        return;
    state_.relocate(window, *monitor, state_.monitors()[*monitor].current_workspace);
    if (window == state_.active_window() && !state_.focusable(state_.require(window)))
        focus_fallback(*monitor, false);
}

} // namespace lwm
