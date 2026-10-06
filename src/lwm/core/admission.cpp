// Admission turns observations into registered windows: it classifies roles,
// restores a predecessor's graph, and places newcomers. Metadata updates reuse
// the same classification defaults, rules and pending scratchpad claims.

#include "state.hpp"
#include "classification.hpp"
#include "floating.hpp"
#include "focus.hpp"
#include "log.hpp"
#include "window_rules.hpp"
#include <algorithm>
#include <cassert>

namespace lwm {

namespace {
// The frame around an observed window's current server rectangle.
Geometry observed_frame(WindowObservation const& window, uint32_t border)
{
    return outset(window.geometry.value_or(Geometry{ 0, 0, 300, 200 }), border);
}
}

WindowRole State::role(xcb_window_t id, WindowType type, bool transient, restart::Snapshot const* handoff)
{
    if (auto const* fixture = handoff ? handoff->find_fixture(id) : nullptr)
        return fixture->role == Fixture::Role::Dock ? WindowRole::Dock : WindowRole::Desktop;
    if (handoff && handoff->find(id))
        return WindowRole::Client;
    return classify_window_type(type, transient).role;
}

// Registration order is reserved in observation order, before placement or
// restoration can reorder clients. Fixtures are installed immediately.
std::optional<Client> State::classify(WindowObservation const& window, restart::Snapshot const* handoff, bool adopting)
{
    switch (role(window.id, window.type, window.transient_for != XCB_NONE, handoff))
    {
        case WindowRole::Dock:
            insert_fixture(window.id, Fixture::Role::Dock, window.strut);
            return std::nullopt;
        case WindowRole::Desktop:
            insert_fixture(window.id, Fixture::Role::Desktop);
            return std::nullopt;
        case WindowRole::Popup:
            return std::nullopt;
        case WindowRole::Client:
            break;
    }
    Client client;
    client.id = window.id;
    client.order = register_window(window.id);
    client.transient_for = window.transient_for;
    client.wm_class_name = window.wm_class_name;
    client.wm_class = window.wm_class;
    client.name = window.name;
    client.ewmh_type = window.type;
    auto natural = default_floating(client);
    auto const* rule = match_window_rules(config_.rules, client);
    client.rule = rule ? std::optional{ *rule } : std::nullopt;
    // Mode and border shape the first frame; later rule actions refine its placement.
    client.borderless = rule && rule->borderless.value_or(false);
    if (rule && rule->floating ? *rule->floating : natural.value_or(false))
        client.mode = FloatingMode{ observed_frame(window, border(client)) };
    LWM_LOG_DEBUG(
        "Client observed: window={:#x} transient_for={:#x} natural={} resolved={} rules_matched={}",
        window.id,
        window.transient_for,
        natural ? (*natural ? "floating" : "tiled") : "retained",
        client_kind_str(client),
        rule != nullptr
    );
    client.monitor = focused_monitor_;
    client.workspace = monitors_[client.monitor].current_workspace;
    if (window.desktop)
    {
        if (*window.desktop == STICKY_DESKTOP)
            client.sticky = true;
        else if (auto placement = desktop_placement(*window.desktop))
        {
            std::tie(client.monitor, client.workspace) = *placement;
            client.desktop_pinned = true;
        }
        else
            LWM_LOG_WARN("Ignoring out-of-range _NET_WM_DESKTOP: window={:#x} desktop={}", window.id, *window.desktop);
    }
    auto const& states = window.states;
    if (states.has(WindowState::Above) || states.has(WindowState::Below))
        client.preferences.layer = states.has(WindowState::Above) ? LayerHint::Above : LayerHint::Below;
    if (states.has(WindowState::SkipTaskbar))
        client.preferences.skip_taskbar = true;
    if (states.has(WindowState::SkipPager))
        client.preferences.skip_pager = true;
    client.sticky |= states.has(WindowState::Sticky);
    client.modal = states.has(WindowState::Modal);
    client.fullscreen_claim = states.has(WindowState::Fullscreen) ? next_claim_++ : 0;
    client.maximized_horz = states.has(WindowState::MaximizedHorz);
    client.maximized_vert = states.has(WindowState::MaximizedVert);
    // Applications cannot minimize themselves; only LWM hides windows, and adoption
    // restores what a predecessor published.
    client.iconic = adopting && states.has(WindowState::Hidden);
    client.urgency.set(UrgencySource::App, states.has(WindowState::DemandsAttention) || window.urgent);
    client.accepts_input = window.accepts_input;
    client.supports_take_focus = window.supports_take_focus;
    client.user_time_window = window.user_time_window;
    client.user_time = window.user_time;
    client.fullscreen_monitors = window.fullscreen_monitors;
    client.size_hints = window.size_hints;
    return client;
}

// A live map: placement, rules and focus follow registration. An unrequested
// scratchpad match starts hidden and floating, so it neither tiles nor takes
// focus before its claim.
void State::admit(WindowObservation const& window)
{
    auto candidate = classify(window, nullptr, false);
    if (!candidate)
        return;
    auto const* scratchpad = match_scratchpad(*candidate);
    if (scratchpad)
    {
        candidate->iconic = true;
        if (!floating_mode(*candidate))
            candidate->mode = FloatingMode{ observed_frame(window, border(*candidate)) };
    }
    insert_registered(std::move(*candidate));
    if (auto placement = prepare_placement(window.id))
        finish_placement(window.id, *placement, window.unmanaged_parent);
    if (auto const& admitted = require(window.id); admitted.monitor == focused_monitor_ && focusable(admitted))
        focus(window.id);
    if (scratchpad)
        claim_scratchpad(window.id, *scratchpad);
}

// Startup registers the complete scene before any placement, so every dock
// reservation shapes the workareas. Placement and rule effects establish the
// tiled scene before floating geometry is resolved in parent order. Each pass
// visits a newcomer at most once, even with malformed transient cycles.
void State::adopt(
    std::vector<WindowObservation> const& windows,
    restart::Snapshot const* handoff,
    std::optional<std::pair<int16_t, int16_t>> pointer
)
{
    assert(clients_.empty() && fixtures_.empty());
    // New observations follow the entire saved rank range, including vanished
    // windows. Surviving identities recover their ranks during graph restoration.
    if (handoff)
    {
        for (auto const& client : handoff->clients)
        {
            next_order_ = std::max(next_order_, client.order + 1);
            next_claim_ = std::max(next_claim_, client.fullscreen_claim + 1);
        }
        for (auto const& fixture : handoff->fixtures) next_order_ = std::max(next_order_, fixture.order + 1);
    }
    auto first_new_claim = next_claim_;
    std::vector<Client> candidates;
    std::unordered_map<xcb_window_t, WindowObservation const*> pending;
    for (auto const& window : windows)
        if (auto candidate = classify(window, handoff, true))
        {
            candidates.push_back(std::move(*candidate));
            if (!handoff || !handoff->find(window.id))
                pending.emplace(window.id, &window);
        }
    // Saved clients keep their restored placement; only newcomers are placed.
    if (handoff)
        restore_graph(*handoff, candidates);
    for (auto& candidate : candidates)
        if (pending.contains(candidate.id))
        {
            if (!candidate.desktop_pinned)
            {
                candidate.monitor = focused_monitor_;
                candidate.workspace = monitors_[focused_monitor_].current_workspace;
            }
            insert_registered(std::move(candidate));
        }

    // Resolve every newcomer's placement and rule effects before deriving any
    // parent rectangle. Parents precede children in both passes; the complete
    // tiled scene therefore shapes even mixed-mode transient chains.
    std::vector<std::pair<WindowObservation const*, PlacementGeometry>> placements;
    for (auto const& window : windows)
    {
        std::vector<WindowObservation const*> chain;
        for (auto next = window.id; auto node = pending.extract(next); next = node.mapped()->transient_for)
            chain.push_back(node.mapped());
        for (auto it = chain.rbegin(); it != chain.rend(); ++it)
            if (auto placement = prepare_placement((*it)->id))
                placements.emplace_back(*it, *placement);
    }
    // Claims made during adoption, by applications or rules, rank in observation
    // order rather than dependency order, after every surviving claim.
    for (auto const& window : windows)
        if (auto const* client = find(window.id); client && client->fullscreen_claim >= first_new_claim)
            edit(window.id).fullscreen_claim = next_claim_++;
    for (auto const& [window, placement] : placements)
        finish_placement(window->id, placement, window->unmanaged_parent);

    if (pointer)
        focus_monitor(monitor_at(monitors_, pointer->first, pointer->second).value_or(0));
    // Startup always commits one explicit focus, even when it is the root.
    auto const* active = handoff ? find(handoff->active) : nullptr;
    if (active && focusable(*active))
        focus(active->id, 0, false);
    else
        focus_fallback(focused_monitor_, false);
    // Saved claims precede pending launches that mapped while the WM was absent.
    if (handoff)
        for (auto const* client : clients_by_order())
            claim_pending_scratchpad(client->id);
}

// Live admission and batch adoption share preparation and geometry resolution.
std::optional<State::PlacementGeometry> State::prepare_placement(xcb_window_t id)
{
    if (auto const* parent = find(require(id).transient_for))
        relocate(id, parent->monitor, parent->workspace);
    auto placement = size_hint_geometry(id, true);
    if (auto const& rule = require(id).rule)
    {
        auto monitor = require(id).monitor;
        apply_rule_state(id, *rule);
        if (placement && floating_mode(require(id)))
        {
            // Rule relocation centers on the destination workarea, overriding
            // the hinted origin or parent anchor only when the monitor changes.
            if (monitor != require(id).monitor)
                *placement = std::visit([](auto const& frame) -> PlacementGeometry
                { return CenteredSize{ frame.width, frame.height }; }, *placement);
            *placement = rule_geometry(id, *rule, std::move(*placement));
        }
    }
    return placement;
}

void State::size_hints(xcb_window_t id, SizeHints hints, std::optional<Geometry> unmanaged_parent)
{
    clients_.at(id).size_hints = hints;
    if (auto placement = size_hint_geometry(id, false))
        request_geometry(id, resolve_geometry(id, *placement, unmanaged_parent));
}

// Initial hints choose a monitor and an origin or anchor. Later size-only
// updates keep the origin. Transients accept only user-supplied positions.
std::optional<State::PlacementGeometry> State::size_hint_geometry(xcb_window_t id, bool initial)
{
    auto const& client = require(id);
    auto const* floating = floating_mode(client);
    if (!floating)
        return std::nullopt;
    bool anchored = client.transient_for != XCB_NONE;
    auto const& hints = client.size_hints;
    auto rectangle = resize_frame(client, floating->geometry, hints.width, hints.height);
    auto position = hints.user_position ? hints.user_position : anchored ? std::nullopt : hints.program_position;
    bool center = initial;
    if (position)
    {
        Geometry hinted{ position->first, position->second, rectangle.width, rectangle.height };
        auto target = floating::resolve_position_hint(monitors_, client.monitor, anchored || client.desktop_pinned, hinted);
        center = !target.accepted;
        if (target.accepted)
        {
            rectangle = hinted;
            if (initial && target.monitor != client.monitor)
                relocate(id, target.monitor, monitors_[target.monitor].current_workspace);
        }
    }
    if (center)
        return CenteredSize{ rectangle.width, rectangle.height, client.transient_for };
    return rectangle;
}

Geometry State::resolve_geometry(
    xcb_window_t id, PlacementGeometry const& placement, std::optional<Geometry> unmanaged_parent
) const
{
    if (auto const* rectangle = std::get_if<Geometry>(&placement))
        return *rectangle;
    auto const& size = std::get<CenteredSize>(placement);
    auto const* parent = find(size.parent);
    return floating::place_floating(
        monitors_[require(id).monitor].working_area(), size.width, size.height,
        parent ? std::optional{ frame(*parent) } : size.parent != XCB_NONE ? unmanaged_parent : std::nullopt
    );
}

void State::finish_placement(xcb_window_t id, PlacementGeometry const& placement, std::optional<Geometry> unmanaged_parent)
{
    auto rectangle = resolve_geometry(id, placement, unmanaged_parent);
    if (floating_mode(require(id)))
        geometry(id, rectangle);
    else // A scratchpad rule can tile its initially floating candidate.
        edit(id).mode = TiledMode{ rectangle };
}

// Type and transient updates change classification defaults. The default mode
// applies unless the user chose one or a scratchpad owns the representation.
void State::apply_default_mode(xcb_window_t id)
{
    auto const& c = require(id);
    if (!c.preferences.floating && !scratchpad_claim(id) && !pooled(id))
        if (auto floating = default_floating(c))
            set_mode(id, *floating);
}


void State::apply_rule_state(xcb_window_t window, RuleActions const& rule)
{
    LWM_LOG_DEBUG("Applying matched rule: window={:#x}", window);
    if (rule.floating)
        floating(window, *rule.floating);

    auto const& client = require(window);
    auto monitor = resolve_rule_monitor(rule, monitors());
    if (monitor || rule.workspace)
    {
        size_t target = monitor.value_or(client.monitor);
        size_t workspace = std::min(rule.workspace.value_or(client.workspace), monitors()[target].workspaces.size() - 1);
        relocate(window, target, workspace, RelocationGeometry::Center);
    }

    if (rule.borderless)
        assign(window, &Client::borderless, *rule.borderless);
    if (rule.skip_taskbar)
        skip_taskbar(window, *rule.skip_taskbar);
    if (rule.skip_pager)
        skip_pager(window, *rule.skip_pager);
    if (rule.sticky)
        sticky(window, *rule.sticky);
    if (rule.layer)
        layer(window, *rule.layer);
    if (rule.fullscreen)
        fullscreen(window, *rule.fullscreen);
}

State::PlacementGeometry State::rule_geometry(xcb_window_t id, RuleActions const& rule, PlacementGeometry placement) const
{
    if (rule.geometry)
        placement = outset(*rule.geometry, border(require(id)));
    if (rule.center)
        placement = std::visit([](auto const& frame) -> PlacementGeometry
        { return CenteredSize{ frame.width, frame.height }; }, placement);
    return placement;
}

void State::apply_rule(xcb_window_t id, RuleActions const& rule)
{
    apply_rule_state(id, rule);
    if (auto const* floating = floating_mode(require(id)))
        geometry(id, resolve_geometry(id, rule_geometry(id, rule, floating->geometry), std::nullopt));
}

// Metadata reconciles changed rule actions; losing a match leaves prior actions.
void State::reconcile_metadata(xcb_window_t id)
{
    bool changed = match_rule(id);
    if (!claim_pending_scratchpad(id) && changed)
        apply_initial_rule(id);
}

bool State::match_rule(xcb_window_t id)
{
    auto const* rule = match_window_rules(config_.rules, require(id));
    return assign(id, &Client::rule, rule ? std::optional{ *rule } : std::nullopt);
}

void State::apply_initial_rule(xcb_window_t id)
{
    if (auto const& rule = require(id).rule)
        apply_rule(id, *rule);
}

// Reload deliberately reapplies unchanged actions and does not claim pending launches.
void State::reapply_rules()
{
    for (auto const* client : clients_by_order())
    {
        match_rule(client->id);
        apply_initial_rule(client->id);
    }
}

void State::title(xcb_window_t id, std::string value)
{
    if (assign(id, &Client::name, std::move(value)))
        reconcile_metadata(id);
}

void State::window_class(xcb_window_t id, std::string instance, std::string name)
{
    bool changed = assign(id, &Client::wm_class_name, std::move(instance));
    changed |= assign(id, &Client::wm_class, std::move(name));
    if (changed)
        reconcile_metadata(id);
}

void State::window_type(xcb_window_t id, WindowType type)
{
    if (assign(id, &Client::ewmh_type, type))
        apply_default_mode(id);
    reconcile_metadata(id);
}

// Following a managed parent derives its presentation after relocation.
void State::transient(xcb_window_t id, xcb_window_t parent)
{
    if (assign(id, &Client::transient_for, parent))
    {
        apply_default_mode(id);
        if (auto const* target = find(parent); target && relocate(id, target->monitor, target->workspace))
            if (auto const* floating = floating_mode(require(id)))
            {
                CenteredSize size{ floating->geometry.width, floating->geometry.height, parent };
                geometry(id, resolve_geometry(id, size, std::nullopt));
            }
    }
    reconcile_metadata(id);
}

// Exec handoff

void State::restore_graph(restart::Snapshot const& snapshot, std::span<Client> observed)
{
    assert(clients_.empty());
    mutated();
    for (auto& [id, fixture] : fixtures_)
        if (auto const* saved = snapshot.find_fixture(id))
            fixture.order = saved->order;
    std::vector<Monitor> discovered;
    for (auto const& monitor : monitors_) discovered.push_back(fresh_monitor(monitor.name, monitor.geometry));
    monitors_.clear();
    for (auto const& monitor : snapshot.monitors) monitors_.push_back(Monitor{ monitor });
    focused_monitor_ = snapshot.focused_monitor;
    for (auto& client : observed)
    {
        auto const* saved = snapshot.find(client.id);
        if (!saved)
            continue;
        // The application's current state decides fullscreen; a surviving claim
        // keeps its saved priority.
        auto observed_claim = client.fullscreen_claim;
        static_cast<ClientIntent&>(client) = *saved;
        if (!observed_claim || !client.fullscreen_claim)
            client.fullscreen_claim = observed_claim;
        forget_missing_tile_slot(client);
        if (client.fullscreen())
            client.maximized_horz = client.maximized_vert = false;
        next_recency_ = std::max(next_recency_, client.mru_order + 1);
        clients_.emplace(client.id, std::move(client));
    }
    rebind(std::move(discovered));
    // Filter only after merging: surviving workspace preferences have precedence
    // over incoming ones, even when their former target disappeared during exec.
    for (auto& monitor : monitors_)
        for (auto& workspace : monitor.workspaces)
        {
            std::erase_if(workspace.windows, [&](auto id) { return !find(id); });
            auto const* preferred = find(workspace.preferred_tile);
            if (!preferred || preferred->iconic)
                workspace.preferred_tile = XCB_NONE;
        }
    for (auto& slot : named_scratchpads_)
    {
        auto saved = std::ranges::find(snapshot.named_scratchpads, slot.name, &NamedScratchpad::name);
        if (saved != snapshot.named_scratchpads.end() && (!saved->window || find(*saved->window)))
            slot.window = saved->window;
    }
    for (auto window : snapshot.pool)
        if (find(window))
            pool_scratchpad(window);
}

} // namespace lwm
