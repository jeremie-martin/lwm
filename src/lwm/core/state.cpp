#include "state.hpp"
#include "classification.hpp"
#include "floating.hpp"
#include "focus.hpp"
#include "log.hpp"
#include "policy.hpp"
#include <algorithm>
#include <cassert>
#include <tuple>

namespace lwm {

Client const* State::find(xcb_window_t id) const
{
    auto it = clients_.find(id);
    return it == clients_.end() ? nullptr : &it->second;
}

Client const& State::require(xcb_window_t id) const { return clients_.at(id); }

std::vector<Client const*> State::clients_by_order() const
{
    std::vector<Client const*> ordered;
    ordered.reserve(clients_.size());
    for (auto const& [id, client] : clients_) ordered.push_back(&client);
    std::ranges::sort(ordered, { }, &Client::order);
    return ordered;
}

Fixture const* State::find_fixture(xcb_window_t id) const
{
    auto it = fixtures_.find(id);
    return it == fixtures_.end() ? nullptr : &it->second;
}

void State::mutated()
{
    assert(!frozen_);
    ++revision_;
}

Client& State::edit(xcb_window_t id)
{
    mutated();
    return clients_.at(id);
}

Workspace& State::edit_workspace(size_t monitor, size_t workspace)
{
    mutated();
    return monitors_.at(monitor).workspaces.at(workspace);
}

// Registry

uint64_t State::register_window(xcb_window_t id)
{
    focus_cycle_.clear();
    assert(id != XCB_NONE && !find(id) && !find_fixture(id));
    return next_order_++;
}

void State::insert(Client client)
{
    assert(client.monitor < monitors_.size() && client.workspace < monitors_[client.monitor].workspaces.size());
    mutated();
    forget_missing_tile_slot(client);
    client.order = register_window(client.id);
    client.mru_order = 0;
    auto [it, inserted] = clients_.emplace(client.id, std::move(client));
    assert(inserted);
    if (it->second.kind() == Client::Kind::Tiled)
        attach(it->second);
    if (it->second.fullscreen)
        request_fullscreen(it->first);
}

void State::insert_fixture(xcb_window_t id, Fixture::Role role)
{
    mutated();
    fixtures_.emplace(id, Fixture{ id, role, register_window(id) });
}

void State::erase(xcb_window_t id)
{
    if (fixtures_.erase(id))
    {
        mutated();
        return;
    }
    auto const* client = find(id);
    if (!client)
        return;
    mutated();
    if (client->kind() == Client::Kind::Tiled)
        detach(*client);
    release_scratchpad(id);
    bool active = active_window_ == id;
    clients_.erase(id);
    std::erase(fullscreen_claims_, id);
    if (active)
        focus_fallback(focused_monitor_);
}

void State::attach(Client const& client, std::optional<size_t> index)
{
    auto& windows = monitors_[client.monitor].workspaces[client.workspace].windows;
    auto position = std::min(index.value_or(windows.size()), windows.size());
    windows.insert(windows.begin() + static_cast<std::ptrdiff_t>(position), client.id);
}

std::optional<TileSlot> State::detach(Client const& client)
{
    auto& ws = monitors_[client.monitor].workspaces[client.workspace];
    auto it = ws.find_window(client.id);
    if (it == ws.windows.end())
        return std::nullopt;
    TileSlot slot{ static_cast<size_t>(it - ws.windows.begin()), monitors_[client.monitor].name, client.workspace };
    ws.windows.erase(it);
    if (ws.preferred_tile == client.id)
        ws.preferred_tile = XCB_NONE;
    return slot;
}

// Derived views

bool State::shows(size_t monitor, size_t workspace) const
{
    return !showing_desktop_ && workspace == monitors_[monitor].current_workspace;
}

bool State::in_view(Client const& client) const
{
    return !client.iconic && (client.sticky || shows(client.monitor, client.workspace));
}

// The most recent fullscreen claim among clients in view owns each monitor.
std::vector<xcb_window_t> State::fullscreen_owners() const
{
    std::vector<xcb_window_t> owners(monitors_.size(), XCB_NONE);
    if (!showing_desktop_)
        for (auto id : fullscreen_claims_)
            if (auto const& client = require(id); in_view(client))
                owners[client.monitor] = id;
    return owners;
}

xcb_window_t State::fullscreen_owner(size_t monitor) const { return fullscreen_owners().at(monitor); }

State::FullscreenVisibility State::fullscreen_visibility() const
{
    FullscreenVisibility result{ fullscreen_owners(), { } };
    if (std::ranges::all_of(result.owners, [](auto owner) { return owner == XCB_NONE; }))
        return result;

    // Reverse the managed parent links once. Walking outward from each owner
    // visits every descendant at most once per owner, even with cyclic hints.
    std::unordered_map<xcb_window_t, std::vector<xcb_window_t>> children;
    for (auto const& [id, client] : clients_)
        if (client.transient_for != XCB_NONE)
            children[client.transient_for].push_back(id);
    std::unordered_set<xcb_window_t> visited;
    std::vector<xcb_window_t> pending;
    for (size_t monitor = 0; monitor < result.owners.size(); ++monitor)
    {
        auto owner = result.owners[monitor];
        if (owner == XCB_NONE)
            continue;
        visited.clear();
        visited.insert(owner);
        pending.push_back(owner);
        while (!pending.empty())
        {
            auto window = pending.back();
            pending.pop_back();
            // Intermediate windows may live elsewhere or be hidden. Only the
            // descendant's own monitor determines which owner exempts it.
            if (require(window).monitor == monitor)
                result.exempt.insert(window);
            if (auto it = children.find(window); it != children.end())
                for (auto child : it->second)
                    if (visited.insert(child).second)
                        pending.push_back(child);
        }
    }
    return result;
}

bool State::suppressed(Client const& client) const { return fullscreen_visibility().suppressed(client); }

bool State::visible(Client const& client) const { return visible(client, fullscreen_visibility()); }

bool State::visible(Client const& client, FullscreenVisibility const& fullscreen) const
{
    return in_view(client) && !fullscreen.suppressed(client);
}

bool State::focusable(Client const& client) const
{
    return focusable(client, fullscreen_visibility());
}

bool State::focusable(Client const& client, FullscreenVisibility const& fullscreen) const
{
    return accepts_focus(client) && !showing_desktop_ && visible(client, fullscreen);
}

// Focus

void State::focus(xcb_window_t id, uint32_t time, bool record_user_time)
{
    if (id != XCB_NONE)
    {
        auto const* client = find(id);
        if (showing_desktop_ || !client || !accepts_focus(*client))
            return;
        iconic(id, false);
        focus_monitor(client->monitor);
        if (!client->sticky)
            switch_workspace(client->monitor, client->workspace);
        if (!visible(*client))
            return focus_fallback(client->monitor, false);
    }
    select_focus(id, time, record_user_time);
}

void State::focus_fallback(size_t monitor, bool record_user_time)
{
    select_focus(focus::fallback(*this, monitor), 0, record_user_time);
}

void State::restore(xcb_window_t id, bool activate)
{
    auto const* client = find(id);
    if (!client)
        return;
    iconic(id, false);
    if ((activate || client->fullscreen) && client->monitor == focused_monitor_ && in_view(*client))
        focus(id);
}

// Traversal retains one MRU order. Operations that change its context discard it;
// eligibility is still evaluated on every step.
bool State::cycle_focus(bool forward)
{
    if (focus_cycle_.empty())
        focus_cycle_ = focus::recent_order(*this);
    auto target = focus::cycle_target(focus_cycle_, *this, focused_monitor_, active_window_, forward);
    if (target == XCB_NONE)
    {
        focus_cycle_.clear();
        return false;
    }
    auto order = std::move(focus_cycle_);
    focus(target);
    focus_cycle_ = std::move(order);
    return true;
}

void State::select_focus(xcb_window_t id, uint32_t time, bool record_user_time)
{
    mutated();
    focus_cycle_.clear();
    focus_request_ = FocusRequest{ time, record_user_time };
    if (active_window_ != id)
        LWM_LOG_DEBUG("Focus changed: window={:#x} -> {:#x}", active_window_, id);
    active_window_ = id;
    if (auto const* c = find(id))
        focus_monitor(c->monitor);
}

void State::focus_monitor(size_t monitor)
{
    if (monitor < monitors_.size() && focused_monitor_ != monitor)
    {
        mutated();
        focused_monitor_ = monitor;
        focus_cycle_.clear();
    }
}

std::optional<uint32_t> State::complete_focus(uint32_t input_time)
{
    bool requested = std::exchange(repair_focus_, false);
    auto const* active = find(active_window_);
    if (active ? !focusable(*active)
               : active_window_ != XCB_NONE || (requested && !focus_request_ && !showing_desktop_))
        focus_fallback(focused_monitor_, false);
    auto request = std::exchange(focus_request_, std::nullopt);
    if (request && find(active_window_))
    {
        auto& client = edit(active_window_);
        client.mru_order = next_recency_++;
        uint32_t time = request->time ? request->time : input_time;
        if (request->record_user_time && time
            && (!client.user_time || !ewmh_policy::timestamp_is_before(time, client.user_time)))
            client.user_time = time;
        if (client.kind() == Client::Kind::Tiled)
            edit_workspace(client.monitor, client.workspace).preferred_tile = XCB_NONE;
        clear_urgency(client.id);
    }
    return request ? std::optional{ request->time } : std::nullopt;
}

void State::show_desktop(bool enabled)
{
    if (showing_desktop_ == enabled)
        return;
    mutated();
    showing_desktop_ = enabled;
    if (enabled)
        select_focus(XCB_NONE);
    else
        focus_fallback(focused_monitor_);
}

// Placement and mode

bool State::relocate(
    xcb_window_t id,
    size_t monitor,
    size_t workspace,
    RelocationGeometry geometry,
    std::optional<size_t> tile_index
)
{
    if (monitor >= monitors_.size() || workspace >= monitors_[monitor].workspaces.size())
        return false;
    auto& client = edit(id);
    bool tiled = client.kind() == Client::Kind::Tiled;
    size_t source = client.monitor;
    if (source == monitor && client.workspace == workspace)
    {
        if (!tiled || !tile_index)
            return true;
        auto& windows = monitors_[monitor].workspaces[workspace].windows;
        auto from = std::ranges::find(windows, client.id);
        auto target = windows.begin() + static_cast<std::ptrdiff_t>(std::min(*tile_index, windows.size() - 1));
        // Reordering keeps membership and destination preference.
        if (from < target)
            std::rotate(from, from + 1, target + 1);
        else if (target < from)
            std::rotate(target, from, from + 1);
        return true;
    }
    if (tiled)
        detach(client);
    else if (source != monitor && geometry != RelocationGeometry::Preserve)
    {
        auto& rectangle = floating_mode(client)->geometry;
        auto from = monitors_[source].working_area();
        auto to = monitors_[monitor].working_area();
        rectangle = geometry == RelocationGeometry::Translate
            ? floating::translate_to_area(rectangle, from, to)
            : floating::place_floating(to, rectangle.width, rectangle.height, std::nullopt);
    }
    LWM_LOG_DEBUG(
        "Client relocated: window={:#x} monitor={} -> {} workspace={} -> {}",
        id,
        source,
        monitor,
        client.workspace,
        workspace
    );
    client.monitor = monitor;
    client.workspace = workspace;
    if (tiled)
        attach(client, tile_index);
    bool active = active_window_ == id;
    if (tiled && !client.iconic && (active || !shows(monitor, workspace)))
        edit_workspace(monitor, workspace).preferred_tile = id;
    if (active)
    {
        if (in_view(client))
            focus_monitor(monitor);
        else
            focus_fallback(focused_monitor_);
    }
    return true;
}

// Slots refer to the original workspace, even while the client lives elsewhere.
// Once that workspace disappears, a returning output must not resurrect the slot.
void State::forget_missing_tile_slot(Client& client) const
{
    auto* mode = floating_mode(client);
    if (!mode || !mode->tile_slot)
        return;
    auto const& slot = *mode->tile_slot;
    auto output = std::ranges::find(monitors_, slot.output, &Monitor::name);
    if (output == monitors_.end() || slot.workspace >= output->workspaces.size())
        mode->tile_slot.reset();
}

void State::set_mode(xcb_window_t id, bool floating)
{
    auto& client = edit(id);
    if (floating == (client.kind() == Client::Kind::Floating))
        return;
    LWM_LOG_DEBUG("Client kind changed: window={:#x} floating={}", id, floating);
    if (auto* tiled = tiled_mode(client))
    {
        auto rectangle = floating::recover_to_area(
            monitors_[client.monitor].working_area(),
            tiled->floating ? *tiled->floating : normal_geometry(client)
        );
        auto slot = detach(client);
        client.mode = FloatingMode{ rectangle, slot };
        return;
    }
    auto const& mode = std::get<FloatingMode>(client.mode);
    std::optional<size_t> index;
    if (mode.tile_slot && mode.tile_slot->output == monitors_[client.monitor].name
        && mode.tile_slot->workspace == client.workspace)
        index = mode.tile_slot->index;
    client.mode = TiledMode{ mode.geometry };
    attach(client, index);
}

void State::floating(xcb_window_t id, bool enabled)
{
    auto preferences = require(id).preferences;
    preferences.floating = enabled;
    assign(id, &Client::preferences, preferences);
    set_mode(id, enabled);
}

void State::geometry(xcb_window_t id, Geometry rectangle)
{
    auto* mode = floating_mode(clients_.at(id));
    if (!mode || mode->geometry == rectangle)
        return;
    mutated();
    mode->geometry = rectangle;
}

void State::swap_tiles(size_t monitor, size_t a, size_t b)
{
    auto& windows = edit_workspace(monitor, monitors_.at(monitor).current_workspace).windows;
    std::swap(windows.at(a), windows.at(b));
}

// Client state

void State::iconic(xcb_window_t id, bool enabled)
{
    if (require(id).iconic == enabled)
        return;
    auto& c = edit(id);
    c.iconic = enabled;
    auto& workspace = monitors_[c.monitor].workspaces[c.workspace];
    if (enabled && workspace.preferred_tile == id)
        workspace.preferred_tile = XCB_NONE;
    // Restoring a fullscreen client makes it the preferred owner again.
    if (!enabled && c.fullscreen)
        request_fullscreen(id);
    if (enabled && active_window_ == id)
        focus_fallback(focused_monitor_);
}

void State::sticky(xcb_window_t id, bool enabled)
{
    if (require(id).sticky == enabled)
        return;
    auto& c = edit(id);
    c.sticky = enabled;
    if (enabled)
        c.desktop_pinned = false;
}

void State::fullscreen(xcb_window_t id, bool enabled)
{
    if (require(id).fullscreen == enabled)
        return;
    if (enabled)
        return request_fullscreen(id);
    auto& c = edit(id);
    LWM_LOG_DEBUG("Fullscreen changed: window={:#x} enabled={}", id, false);
    c.fullscreen = false;
    std::erase(fullscreen_claims_, id);
    repair_focus_ = true;
}

void State::request_fullscreen(xcb_window_t id)
{
    auto& c = edit(id);
    if (!c.fullscreen)
        LWM_LOG_DEBUG("Fullscreen changed: window={:#x} enabled={}", id, true);
    c.fullscreen = true;
    c.maximized_horz = c.maximized_vert = false;
    std::erase(fullscreen_claims_, id);
    fullscreen_claims_.push_back(id);
    repair_focus_ = true;
}

void State::maximize(xcb_window_t id, bool horizontal, bool vertical)
{
    auto const& current = require(id);
    if (current.fullscreen)
        horizontal = vertical = false;
    if (current.maximized_horz == horizontal && current.maximized_vert == vertical)
        return;
    auto& c = edit(id);
    c.maximized_horz = horizontal;
    c.maximized_vert = vertical;
}

void State::modal(xcb_window_t id, bool enabled) { assign(id, &Client::modal, enabled); }

void State::layer(xcb_window_t id, LayerHint hint)
{
    auto preferences = require(id).preferences;
    preferences.layer = hint;
    assign(id, &Client::preferences, preferences);
}

void State::skip_taskbar(xcb_window_t id, bool enabled)
{
    auto preferences = require(id).preferences;
    preferences.skip_taskbar = enabled;
    assign(id, &Client::preferences, preferences);
}

void State::skip_pager(xcb_window_t id, bool enabled)
{
    auto preferences = require(id).preferences;
    preferences.skip_pager = enabled;
    assign(id, &Client::preferences, preferences);
}

void State::urgency(xcb_window_t id, UrgencySource source, bool enabled)
{
    auto urgency = require(id).urgency;
    if (enabled ? urgency.add(source) : urgency.remove(source))
        edit(id).urgency = urgency;
}

void State::clear_urgency(xcb_window_t id)
{
    if (require(id).urgency.active())
        edit(id).urgency.clear();
}

void State::fullscreen_monitors(xcb_window_t id, std::optional<FullscreenMonitors> value)
{
    assign(id, &Client::fullscreen_monitors, value);
}

void State::pin_desktop(xcb_window_t id, bool pinned) { assign(id, &Client::desktop_pinned, pinned); }

// Metadata

// Type and transient updates change classification defaults. The default mode
// applies unless the user chose one or a scratchpad owns the representation.
void State::apply_default_mode(xcb_window_t id)
{
    auto const& c = require(id);
    if (!c.preferences.floating && !scratchpad_claim(id) && !pooled(id))
        if (auto floating = default_floating(c))
            set_mode(id, *floating);
}

void State::focus_hints(xcb_window_t id, bool input, bool take_focus)
{
    bool changed = assign(id, &Client::accepts_input, input);
    changed |= assign(id, &Client::supports_take_focus, take_focus);
    repair_focus_ |= changed;
}

// Activation-time bookkeeping is not exposed or published, so it is not a revision.
void State::user_time(xcb_window_t id, uint32_t time, xcb_window_t window)
{
    assert(!frozen_);
    auto& c = clients_.at(id);
    c.user_time = time;
    c.user_time_window = window;
}

// Workspaces and monitors

bool State::switch_workspace(size_t monitor, size_t workspace)
{
    if (monitor >= monitors_.size())
        return false;
    auto& m = monitors_[monitor];
    if (!workspace_policy::validate_workspace_switch(m, workspace))
        return false;
    mutated();
    LWM_LOG_DEBUG("Workspace changed: monitor={} workspace={} -> {}", monitor, m.current_workspace, workspace);
    m.previous_workspace = m.current_workspace;
    m.current_workspace = workspace;
    if (monitor == focused_monitor_)
        focus_fallback(monitor);
    return true;
}

void State::layout(size_t monitor, LayoutStrategy strategy)
{
    if (monitors_.at(monitor).current().layout_strategy != strategy)
        edit_workspace(monitor, monitors_[monitor].current_workspace).layout_strategy = strategy;
}

void State::ratio(size_t monitor, SplitAddress address, double value)
{
    auto const& ratios = monitors_.at(monitor).current().split_ratios;
    if (auto it = ratios.find(address); it == ratios.end() || it->second != value)
        edit_workspace(monitor, monitors_[monitor].current_workspace).split_ratios[address] = value;
}

void State::erase_ratio(size_t monitor, SplitAddress address)
{
    if (monitors_.at(monitor).current().split_ratios.contains(address))
        edit_workspace(monitor, monitors_[monitor].current_workspace).split_ratios.erase(address);
}

void State::reset_ratios(size_t monitor)
{
    if (!monitors_.at(monitor).current().split_ratios.empty())
        edit_workspace(monitor, monitors_[monitor].current_workspace).split_ratios.clear();
}

void State::workarea(size_t monitor, Strut strut)
{
    if (monitors_.at(monitor).strut == strut)
        return;
    mutated();
    monitors_[monitor].strut = strut;
}

void State::replace_monitors(std::vector<Monitor> monitors) { replace_monitors(std::move(monitors), true); }

void State::replace_monitors(std::vector<Monitor> monitors, bool fit_unchanged)
{
    assert(!monitors.empty());
    mutated();
    focus_cycle_.clear();
    bool topology_changed = monitors_.size() != monitors.size()
        || !std::ranges::equal(monitors_,
                               monitors,
                               [](auto const& a, auto const& b)
                               { return a.name == b.name && a.geometry == b.geometry; });
    if (topology_changed)
    {
        LWM_LOG_INFO("Monitor topology changed: count={} -> {}", monitors_.size(), monitors.size());
        for (size_t i = 0; i < monitors.size(); ++i)
        {
            auto const& m = monitors[i];
            LWM_LOG_INFO(
                "Monitor: index={} name={} x={} y={} width={} height={}",
                i,
                m.name,
                m.geometry.x,
                m.geometry.y,
                m.geometry.width,
                m.geometry.height
            );
        }
    }
    auto previous = std::move(monitors_);
    auto destinations = hotplug_policy::preserve_workspaces(previous, monitors);
    focused_monitor_ = focused_monitor_ < destinations.size() ? destinations[focused_monitor_] : 0;
    monitors_ = std::move(monitors);
    auto survives = [&](size_t index)
    {
        return index < previous.size()
            && std::ranges::any_of(monitors_, [&](auto const& m) { return m.name == previous[index].name; });
    };
    for (auto& [id, c] : clients_)
    {
        bool displaced = !survives(c.monitor);
        c.monitor = c.monitor < destinations.size() ? destinations[c.monitor] : 0;
        c.workspace = std::min(c.workspace, monitors_[c.monitor].workspaces.size() - 1);
        forget_missing_tile_slot(c);
        // Restart with unchanged topology preserves intentional off-workarea
        // geometry and monitor hints; live refresh still fits to current struts.
        if (topology_changed || fit_unchanged)
        {
            c.fullscreen_monitors.reset();
            if (auto* mode = floating_mode(c))
                mode->geometry = hotplug_policy::fit_floating(mode->geometry, monitors_[c.monitor].working_area(), displaced);
        }
    }
}

// Scratchpads

NamedScratchpad const* State::named_scratchpad(std::string_view name) const
{
    auto it = std::ranges::find(named_scratchpads_, name, &NamedScratchpad::name);
    return it == named_scratchpads_.end() ? nullptr : &*it;
}

NamedScratchpad const* State::scratchpad_claim(xcb_window_t id) const
{
    if (id == XCB_NONE)
        return nullptr;
    auto it = std::ranges::find(named_scratchpads_, id, &NamedScratchpad::claimed_window);
    return it == named_scratchpads_.end() ? nullptr : &*it;
}

bool State::pooled(xcb_window_t id) const { return std::ranges::find(scratchpad_pool_, id) != scratchpad_pool_.end(); }

void State::release_scratchpad(xcb_window_t id)
{
    for (auto& slot : named_scratchpads_)
        if (slot.claimed_window() == id)
            slot.window = XCB_NONE;
    std::erase(scratchpad_pool_, id);
}

// Surviving names keep claims and pending launches; removed names release
// their windows and deiconify them; workspace/fullscreen visibility still applies.
void State::configure_scratchpads(std::span<ScratchpadConfig const> configs)
{
    mutated();
    std::vector<NamedScratchpad> slots;
    for (auto const& config : configs)
    {
        auto const* existing = named_scratchpad(config.name);
        slots.push_back(existing ? *existing : NamedScratchpad{ config.name });
    }
    std::vector<xcb_window_t> released;
    for (auto const& slot : named_scratchpads_)
        if (slot.claimed_window() != XCB_NONE && std::ranges::find(configs, slot.name, &ScratchpadConfig::name) == configs.end())
            released.push_back(slot.claimed_window());
    named_scratchpads_ = std::move(slots);
    for (auto id : released) iconic(id, false);
}

void State::pool_scratchpad(xcb_window_t id)
{
    if (scratchpad_claim(id) || pooled(id))
        return;
    mutated();
    scratchpad_pool_.push_back(id);
}

// The back is the recall target. Advancing preserves every member in rotation.
void State::advance_scratchpad_pool()
{
    if (scratchpad_pool_.size() < 2)
        return;
    mutated();
    std::rotate(scratchpad_pool_.begin(), scratchpad_pool_.end() - 1, scratchpad_pool_.end());
}

void State::scratchpad_pending(std::string_view name, bool pending)
{
    auto it = std::ranges::find(named_scratchpads_, name, &NamedScratchpad::name);
    if (it == named_scratchpads_.end() || it->claimed_window() != XCB_NONE)
        return;
    mutated();
    it->window = pending ? std::nullopt : std::optional<xcb_window_t>{ XCB_NONE };
}

// Exec handoff

restart::Snapshot State::snapshot() const
{
    restart::Snapshot snapshot;
    for (auto const& [id, fixture] : fixtures_) snapshot.fixtures.push_back(fixture);
    std::ranges::sort(snapshot.fixtures, {}, &Fixture::order);
    snapshot.focused_monitor = focused_monitor_;
    snapshot.active = active_window_;
    snapshot.showing_desktop = showing_desktop_;
    for (auto const& monitor : monitors_) snapshot.monitors.push_back(monitor);
    for (auto const* client : clients_by_order()) snapshot.clients.push_back(*client);
    for (auto const& slot : named_scratchpads_)
        if (slot.claimed_window() != XCB_NONE || slot.pending_launch())
            snapshot.named_scratchpads.push_back(slot);
    snapshot.pool = scratchpad_pool_;
    snapshot.fullscreen_claims = fullscreen_claims_;
    return snapshot;
}

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
    auto discovered = std::move(monitors_);
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
    replace_monitors(std::move(discovered), false);
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
