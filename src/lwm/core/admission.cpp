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

// Registers fixtures directly and returns the client candidate, or nothing for
// fixtures and directly mapped popups.
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
        client_kind_str(client.kind()),
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
    client.fullscreen = states.has(WindowState::Fullscreen);
    client.maximized_horz = states.has(WindowState::MaximizedHorz);
    client.maximized_vert = states.has(WindowState::MaximizedVert);
    // Adoption preserves the current minimized state instead of the initial hint.
    client.iconic = states.has(WindowState::Hidden) || (!adopting && window.initially_iconic);
    if (states.has(WindowState::DemandsAttention) || window.urgent)
        client.urgency.add(UrgencySource::App);
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
    insert(std::move(*candidate));
    place(window.id, window.unmanaged_parent);
    if (auto const& admitted = require(window.id); admitted.monitor == focused_monitor_ && focusable(admitted))
        focus(window.id);
    if (scratchpad)
        claim_scratchpad(window.id, *scratchpad);
}

// Startup registers the complete scene before any placement, so every dock
// reservation shapes the workareas. Tiled placement precedes floating placement,
// and floating parents are placed before their children, visiting each window
// once even with malformed transient cycles.
void State::adopt(
    std::vector<WindowObservation> const& windows,
    restart::Snapshot const* handoff,
    std::optional<std::pair<int16_t, int16_t>> pointer
)
{
    std::vector<Client> candidates;
    std::unordered_map<xcb_window_t, std::optional<Geometry>> parents;
    for (auto const& window : windows)
        if (auto candidate = classify(window, handoff, true))
        {
            parents[window.id] = window.unmanaged_parent;
            candidates.push_back(std::move(*candidate));
        }
    // Saved clients keep their restored placement; only newcomers are placed.
    auto newcomer = [&](xcb_window_t id) { return !handoff || !handoff->find(id); };
    if (handoff)
        restore_graph(*handoff, std::move(candidates));
    else
        for (auto& candidate : candidates)
        {
            auto id = candidate.id;
            insert(std::move(candidate));
            if (!floating_mode(require(id)))
                place(id, parents[id]);
        }
    auto order = clients_by_order();
    if (handoff)
        for (auto const* client : order)
            if (!floating_mode(*client) && newcomer(client->id))
                place(client->id, parents[client->id]);
    std::unordered_set<xcb_window_t> pending;
    for (auto const* client : order)
        if (floating_mode(*client) && newcomer(client->id))
            pending.insert(client->id);
    for (auto const* client : order)
    {
        std::vector<xcb_window_t> chain;
        for (auto next = client->id; pending.erase(next); next = require(next).transient_for)
            chain.push_back(next);
        for (auto it = chain.rbegin(); it != chain.rend(); ++it)
            place(*it, parents[*it]);
    }

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

// Initial placement and later size-hint updates share one geometry policy.
void State::place(xcb_window_t id, std::optional<Geometry> unmanaged_parent)
{
    apply_size_hints(id, true, unmanaged_parent);
    apply_initial_rule(id);
}

void State::size_hints(xcb_window_t id, SizeHints hints, std::optional<Geometry> unmanaged_parent)
{
    clients_.at(id).size_hints = hints;
    apply_size_hints(id, false, unmanaged_parent);
}

// Initial placement joins a managed parent's workspace and centers without an
// accepted position hint. Later updates keep the chosen origin for size-only
// changes. Position hints of transients count only when the user supplied them.
void State::apply_size_hints(xcb_window_t id, bool initial, std::optional<Geometry> unmanaged_parent)
{
    auto const& client = require(id);
    auto const* parent = find(client.transient_for);
    if (initial && parent)
        relocate(id, parent->monitor, parent->workspace);
    auto const* floating = floating_mode(client);
    if (!floating)
        return;
    bool anchored = client.transient_for != XCB_NONE;
    auto const& hints = client.size_hints;
    auto window = inset(floating->geometry, border(client));
    window.width = hints.width.value_or(window.width);
    window.height = hints.height.value_or(window.height);
    auto geometry = outset(window, border(client));
    auto position = hints.user_position ? hints.user_position : anchored ? std::nullopt : hints.program_position;
    auto monitor = client.monitor;
    bool center = initial;
    if (position)
    {
        Geometry hinted{ position->first, position->second, geometry.width, geometry.height };
        auto target = floating::resolve_position_hint(monitors_, monitor, anchored || client.desktop_pinned, hinted);
        monitor = target.monitor;
        center = !target.accepted;
        if (target.accepted)
        {
            geometry = hinted;
            if (initial && monitor != client.monitor)
                relocate(id, monitor, monitors_[monitor].current_workspace);
        }
    }
    if (center)
    {
        auto parent_geometry = parent ? std::optional{ frame(*parent) } : unmanaged_parent;
        geometry = floating::place_floating(
            monitors_[monitor].working_area(), geometry.width, geometry.height, anchored ? parent_geometry : std::nullopt
        );
    }
    if (initial)
        this->geometry(id, geometry);
    else
        request_geometry(id, geometry);
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


void State::apply_rule(xcb_window_t window, RuleActions const& rule)
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
    if (auto const* floating = floating_mode(require(window)))
    {
        auto rectangle = rule.geometry ? outset(*rule.geometry, border(client)) : floating->geometry;
        if (rule.center)
            rectangle = floating::place_floating(
                monitors()[client.monitor].working_area(),
                rectangle.width,
                rectangle.height,
                std::nullopt
            );
        geometry(window, rectangle);
    }

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
                geometry(
                    id,
                    floating::place_floating(
                        monitors_[target->monitor].working_area(),
                        floating->geometry.width,
                        floating->geometry.height,
                        frame(*target)
                    )
                );
    }
    reconcile_metadata(id);
}

// Exec handoff

void State::restore_graph(restart::Snapshot const& snapshot, std::vector<Client> observed)
{
    assert(clients_.empty());
    mutated();
    // Fixture observations precede workarea discovery. Restore their ranks here,
    // reserving the complete saved range before allocating newcomer ranks.
    uint64_t bound = 0;
    for (auto const& client : snapshot.clients) bound = std::max(bound, client.order + 1);
    for (auto const& fixture : snapshot.fixtures) bound = std::max(bound, fixture.order + 1);
    for (auto& [id, fixture] : fixtures_)
        if (auto const* saved = snapshot.find_fixture(id))
            fixture.order = saved->order;
        else
            fixture.order += bound;
    next_order_ += bound;
    std::vector<Monitor> discovered;
    for (auto const& monitor : monitors_) discovered.push_back(fresh_monitor(monitor.name, monitor.geometry));
    monitors_.clear();
    for (auto const& monitor : snapshot.monitors) monitors_.push_back(Monitor{ monitor });
    focused_monitor_ = snapshot.focused_monitor;
    showing_desktop_ = snapshot.showing_desktop;
    for (auto& client : observed)
    {
        auto const* saved = snapshot.find(client.id);
        if (!saved)
            continue;
        static_cast<ClientIntent&>(client) = *saved;
        if (client.fullscreen)
            client.maximized_horz = client.maximized_vert = false;
        next_recency_ = std::max(next_recency_, client.mru_order + 1);
        clients_.emplace(client.id, std::move(client));
    }
    for (auto id : snapshot.fullscreen_claims)
        if (auto const* client = find(id); client && client->fullscreen)
            fullscreen_claims_.push_back(id);
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
    // Newcomers choose the restored current workspace. New fullscreen requests
    // follow saved claims in scan order, including those from saved clients.
    for (auto& observation : observed)
    {
        if (auto const* client = find(observation.id))
        {
            if (client->fullscreen && !std::ranges::contains(fullscreen_claims_, client->id))
                request_fullscreen(client->id);
        }
        else
        {
            if (!observation.desktop_pinned)
            {
                observation.monitor = focused_monitor_;
                observation.workspace = monitors_[focused_monitor_].current_workspace;
            }
            insert(std::move(observation));
        }
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
