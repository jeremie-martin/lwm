#include "state.hpp"
#include "classification.hpp"
#include "floating.hpp"
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

// ---------------------------------------------------------------------------
// Registry
// ---------------------------------------------------------------------------

uint64_t State::register_window(xcb_window_t id, std::span<xcb_window_t const> registration_order)
{
    assert(id != XCB_NONE && !find(id) && !find_fixture(id));
    // Fixtures may be admitted before clients. Reserve every saved rank so a
    // newcomer cannot take the rank of a survivor that has not been admitted yet.
    next_order_ = std::max<uint64_t>(next_order_, registration_order.size());
    auto saved = std::ranges::find(registration_order, id);
    return saved == registration_order.end() ? next_order_++ : static_cast<uint64_t>(saved - registration_order.begin());
}

void State::insert(Client client, std::span<xcb_window_t const> registration_order)
{
    assert(client.monitor < monitors_.size() && client.workspace < monitors_[client.monitor].workspaces.size());
    mutated();
    forget_missing_tile_slot(client);
    client.order = register_window(client.id, registration_order);
    if (client.kind() == Client::Kind::Floating)
        client.mru_order = next_recency_++;
    client.fullscreen_claim = 0;
    auto [it, inserted] = clients_.emplace(client.id, std::move(client));
    assert(inserted);
    if (it->second.kind() == Client::Kind::Tiled)
        attach(it->second);
    if (it->second.fullscreen)
        request_fullscreen(it->first);
}

void State::insert_fixture(xcb_window_t id, Fixture::Role role, std::span<xcb_window_t const> registration_order)
{
    mutated();
    fixtures_.emplace(id, Fixture{ id, role, register_window(id, registration_order) });
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
    if (active_window_ == id)
        active_window_ = XCB_NONE;
    clients_.erase(id);
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
    workspace_policy::remove_from_focus_history(ws, client.id);
    workspace_policy::fixup_workspace_focus(
        ws,
        client.id,
        [this](xcb_window_t id)
        {
            auto const* c = find(id);
            return c && c->iconic;
        }
    );
    return slot;
}

// ---------------------------------------------------------------------------
// Derived views
// ---------------------------------------------------------------------------

bool State::shows(size_t monitor, size_t workspace) const
{
    return !showing_desktop_ && workspace == monitors_[monitor].current_workspace;
}

bool State::in_view(Client const& client) const
{
    return !client.iconic && (client.sticky || shows(client.monitor, client.workspace));
}

namespace {
bool claims_before(Client const& a, Client const& b)
{
    return std::tie(a.fullscreen_claim, a.order, a.id) < std::tie(b.fullscreen_claim, b.order, b.id);
}
}

// The most recent fullscreen claim among clients in view owns each monitor.
std::vector<xcb_window_t> State::fullscreen_owners() const
{
    std::vector<Client const*> owners(monitors_.size(), nullptr);
    if (!showing_desktop_)
        for (auto const& [id, client] : clients_)
            if (client.fullscreen && in_view(client)
                && (!owners[client.monitor] || claims_before(*owners[client.monitor], client)))
                owners[client.monitor] = &client;
    std::vector<xcb_window_t> result;
    result.reserve(owners.size());
    for (auto const* owner : owners) result.push_back(owner ? owner->id : XCB_NONE);
    return result;
}

// Persistence records claim order, not an owner or process-local counter values.
std::vector<xcb_window_t> State::fullscreen_claim_order() const
{
    std::vector<Client const*> clients;
    for (auto const& [id, client] : clients_)
        if (client.fullscreen)
            clients.push_back(&client);
    std::ranges::sort(clients, [](auto const* a, auto const* b) { return claims_before(*a, *b); });
    std::vector<xcb_window_t> order;
    order.reserve(clients.size());
    for (auto const* client : clients) order.push_back(client->id);
    return order;
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

// ---------------------------------------------------------------------------
// Focus
// ---------------------------------------------------------------------------

void State::focus(xcb_window_t id, uint32_t time)
{
    mutated();
    focus_request_ = time;
    if (active_window_ != id)
        LWM_LOG_DEBUG("Focus changed: window={:#x} -> {:#x}", active_window_, id);
    active_window_ = id;
    if (auto const* c = find(id))
    {
        focus_monitor(c->monitor);
        remember_focus(id);
        touch(id);
    }
}

void State::focus_monitor(size_t monitor)
{
    if (monitor < monitors_.size() && focused_monitor_ != monitor)
    {
        mutated();
        focused_monitor_ = monitor;
    }
}

// Remembered focus names a tile that could take focus again, never an iconic one.
void State::remember_focus(xcb_window_t id)
{
    auto const& client = require(id);
    if (client.kind() == Client::Kind::Tiled && !client.iconic)
        workspace_policy::set_workspace_focus(edit_workspace(client.monitor, client.workspace), id);
}

void State::touch(xcb_window_t id) { edit(id).mru_order = next_recency_++; }

void State::show_desktop(bool enabled)
{
    if (showing_desktop_ == enabled)
        return;
    mutated();
    showing_desktop_ = enabled;
}

// ---------------------------------------------------------------------------
// Placement and mode
// ---------------------------------------------------------------------------

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
        // Reordering keeps membership and remembered focus.
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
        // An unarranged tile can still hold an off-monitor initial or hotplug rectangle.
        auto rectangle = floating::recover_to_area(
            monitors_[client.monitor].working_area(),
            tiled->floating.value_or(tiled->layout)
        );
        auto slot = detach(client);
        client.mode = FloatingMode{ rectangle, slot };
        touch(id);
        return;
    }
    auto const& mode = std::get<FloatingMode>(client.mode);
    std::optional<size_t> index;
    if (mode.tile_slot && mode.tile_slot->output == monitors_[client.monitor].name
        && mode.tile_slot->workspace == client.workspace)
        index = mode.tile_slot->index;
    // Until layout runs, the floating rectangle is also the tile's best known geometry.
    client.mode = TiledMode{ mode.geometry, mode.geometry };
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

void State::place_tile(xcb_window_t id, Geometry rectangle)
{
    assert(!frozen_);
    // Layout targets are recomputed from state and are not part of the revision.
    if (auto* mode = tiled_mode(clients_.at(id)))
        mode->layout = rectangle;
}

void State::swap_tiles(size_t monitor, size_t a, size_t b)
{
    auto& windows = edit_workspace(monitor, monitors_.at(monitor).current_workspace).windows;
    std::swap(windows.at(a), windows.at(b));
}

// ---------------------------------------------------------------------------
// Client state
// ---------------------------------------------------------------------------

void State::iconic(xcb_window_t id, bool enabled)
{
    if (require(id).iconic == enabled)
        return;
    auto& c = edit(id);
    c.iconic = enabled;
    if (enabled && c.kind() == Client::Kind::Tiled)
        workspace_policy::fixup_workspace_focus(
            monitors_[c.monitor].workspaces[c.workspace],
            id,
            [this](auto w) { return require(w).iconic; }
        );
    // Restoring a fullscreen client makes it the preferred owner again.
    if (!enabled && c.fullscreen)
        request_fullscreen(id);
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
    c.fullscreen_claim = 0;
    repair_focus_ = true;
}

void State::request_fullscreen(xcb_window_t id)
{
    auto& c = edit(id);
    if (!c.fullscreen)
        LWM_LOG_DEBUG("Fullscreen changed: window={:#x} enabled={}", id, true);
    c.fullscreen = true;
    c.maximized_horz = c.maximized_vert = false;
    c.fullscreen_claim = ++next_fullscreen_claim_;
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
void State::borderless(xcb_window_t id, bool enabled) { assign(id, &Client::borderless, enabled); }

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

// ---------------------------------------------------------------------------
// Metadata
// ---------------------------------------------------------------------------

void State::title(xcb_window_t id, std::string value) { assign(id, &Client::name, std::move(value)); }

void State::window_class(xcb_window_t id, std::string instance, std::string name)
{
    assign(id, &Client::wm_class_name, std::move(instance));
    assign(id, &Client::wm_class, std::move(name));
}

void State::window_type(xcb_window_t id, WindowType type)
{
    if (assign(id, &Client::ewmh_type, type))
        apply_default_mode(id);
}

void State::transient(xcb_window_t id, xcb_window_t parent)
{
    if (assign(id, &Client::transient_for, parent))
        apply_default_mode(id);
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

void State::rule(xcb_window_t id, std::optional<RuleActions> actions) { assign(id, &Client::rule, std::move(actions)); }

// ---------------------------------------------------------------------------
// Workspaces and monitors
// ---------------------------------------------------------------------------

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

void State::replace_monitors(std::vector<Monitor> monitors)
{
    assert(!monitors.empty());
    mutated();
    bool topology_changed = monitors_.size() != monitors.size()
        || !std::ranges::equal(monitors_,
                               monitors,
                               [](auto const& a, auto const& b)
                               { return a.name == b.name && a.geometry() == b.geometry(); });
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
                m.x,
                m.y,
                m.width,
                m.height
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
        // Monitor indices in the hint may name different outputs now.
        c.fullscreen_monitors.reset();
        forget_missing_tile_slot(c);
        if (auto* mode = floating_mode(c))
        {
            auto area = monitors_[c.monitor].working_area();
            mode->geometry = hotplug_policy::fit_floating(mode->geometry, area, displaced);
        }
    }
}

// ---------------------------------------------------------------------------
// Scratchpads
// ---------------------------------------------------------------------------

State::NamedScratchpad const* State::named_scratchpad(std::string_view name) const
{
    auto it = std::ranges::find(named_scratchpads_, name, &NamedScratchpad::name);
    return it == named_scratchpads_.end() ? nullptr : &*it;
}

State::NamedScratchpad const* State::scratchpad_claim(xcb_window_t id) const
{
    if (id == XCB_NONE)
        return nullptr;
    auto it = std::ranges::find(named_scratchpads_, id, &NamedScratchpad::window);
    return it == named_scratchpads_.end() ? nullptr : &*it;
}

bool State::pooled(xcb_window_t id) const { return std::ranges::find(scratchpad_pool_, id) != scratchpad_pool_.end(); }

void State::release_scratchpad(xcb_window_t id)
{
    for (auto& slot : named_scratchpads_)
        if (slot.window() == id)
            slot.state = NamedScratchpad::Empty{ };
    std::erase(scratchpad_pool_, id);
}

// Surviving names keep claims and pending launches; removed names release
// their windows, which become ordinary visible clients again.
void State::configure_scratchpads(std::span<std::string const> names)
{
    mutated();
    std::vector<NamedScratchpad> slots;
    for (auto const& name : names)
    {
        auto const* existing = named_scratchpad(name);
        slots.push_back(existing ? *existing : NamedScratchpad{ name });
    }
    std::vector<xcb_window_t> released;
    for (auto const& slot : named_scratchpads_)
        if (slot.window() != XCB_NONE && std::ranges::find(names, slot.name) == names.end())
            released.push_back(slot.window());
    named_scratchpads_ = std::move(slots);
    for (auto id : released) iconic(id, false);
}

void State::claim_scratchpad(std::string_view name, xcb_window_t id)
{
    mutated();
    release_scratchpad(id);
    auto it = std::ranges::find(named_scratchpads_, name, &NamedScratchpad::name);
    assert(it != named_scratchpads_.end());
    it->state = NamedScratchpad::Claimed{ id };
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
    if (it == named_scratchpads_.end() || it->window() != XCB_NONE)
        return;
    mutated();
    if (pending)
        it->state = NamedScratchpad::LaunchPending{ };
    else
        it->state = NamedScratchpad::Empty{ };
}

// ---------------------------------------------------------------------------
// Exec handoff
// ---------------------------------------------------------------------------

restart::Snapshot State::snapshot() const
{
    restart::Snapshot snapshot;
    std::vector<std::pair<uint64_t, xcb_window_t>> registration;
    for (auto const& [id, client] : clients_) registration.emplace_back(client.order, id);
    for (auto const& [id, fixture] : fixtures_) registration.emplace_back(fixture.order, id);
    std::ranges::sort(registration);
    for (auto const& [order, id] : registration) snapshot.registration_order.push_back(id);
    snapshot.focused_monitor = focused_monitor_;
    snapshot.active = active_window_;
    snapshot.showing_desktop = showing_desktop_;
    for (auto const& monitor : monitors_) snapshot.monitors.push_back(restart::capture_monitor(monitor));
    std::vector<Client const*> recency;
    for (auto const& [id, client] : clients_) recency.push_back(&client);
    std::ranges::sort(recency, {}, [](Client const* c) { return std::tie(c->mru_order, c->order); });
    for (auto const* c : recency)
    {
        auto const* tiled = tiled_mode(*c);
        snapshot.clients.push_back(
            { c->id,
              c->monitor,
              c->workspace,
              c->kind(),
              tiled ? tiled->layout : floating_mode(*c)->geometry,
              tiled ? tiled->floating : std::nullopt,
              c->preferences,
              c->urgency.sources,
              c->borderless,
              c->desktop_pinned,
              tiled ? std::nullopt : floating_mode(*c)->tile_slot,
              c->fullscreen_monitors }
        );
    }
    for (auto const& slot : named_scratchpads_)
        if (slot.window() != XCB_NONE || slot.pending_launch())
            snapshot.named_scratchpads.push_back(
                { slot.name, slot.pending_launch() ? std::nullopt : std::optional{ slot.window() } }
            );
    snapshot.pool = scratchpad_pool_;
    snapshot.fullscreen_claims = fullscreen_claim_order();
    return snapshot;
}

void State::restore_workspaces(restart::Snapshot& snapshot)
{
    assert(clients_.empty());
    snapshot.rebind(monitors_);
    mutated();
    focused_monitor_ = snapshot.focused_monitor;
    showing_desktop_ = snapshot.showing_desktop;
    for (size_t m = 0; m < monitors_.size(); ++m)
    {
        auto& monitor = monitors_[m];
        auto const& record = snapshot.monitors[m];
        monitor.current_workspace = record.current;
        monitor.previous_workspace = record.previous;
        for (size_t w = 0; w < monitor.workspaces.size(); ++w)
        {
            monitor.workspaces[w].layout_strategy = record.workspaces[w].strategy;
            monitor.workspaces[w].split_ratios = record.workspaces[w].ratios;
        }
    }
}

void State::restore_membership(restart::Snapshot const& snapshot)
{
    mutated();
    auto adopted_claims = fullscreen_claim_order();
    next_fullscreen_claim_ = 0;
    for (auto id : adopted_claims) clients_.at(id).fullscreen_claim = 0;
    auto restore_claim = [&](xcb_window_t id)
    {
        auto it = clients_.find(id);
        if (it != clients_.end() && it->second.fullscreen && it->second.fullscreen_claim == 0)
            it->second.fullscreen_claim = ++next_fullscreen_claim_;
    };
    for (auto id : snapshot.fullscreen_claims) restore_claim(id);
    // Windows arriving during handoff have newer claims than the saved clients.
    // Keep their adoption order; stale saved IDs cannot steal ownership.
    for (auto id : adopted_claims) restore_claim(id);

    for (size_t m = 0; m < std::min(monitors_.size(), snapshot.monitors.size()); ++m)
        for (size_t w = 0; w < std::min(monitors_[m].workspaces.size(), snapshot.monitors[m].workspaces.size()); ++w)
        {
            auto& workspace = monitors_[m].workspaces[w];
            auto const& saved = snapshot.monitors[m].workspaces[w];
            // Adoption appended tiles in scan order; saved order ranks known members first.
            auto rank = [&](xcb_window_t id)
            { return static_cast<size_t>(std::ranges::find(saved.tiles, id) - saved.tiles.begin()); };
            std::ranges::stable_sort(workspace.windows, {}, rank);
            if (auto const* focused = find(saved.focused);
                focused && workspace.find_window(saved.focused) != workspace.windows.end() && !focused->iconic)
                workspace_policy::set_workspace_focus(workspace, saved.focused);
        }
    for (auto const& record : snapshot.clients)
        if (find(record.window))
            touch(record.window);
    for (auto const& named : snapshot.named_scratchpads)
        if (named_scratchpad(named.name))
        {
            if (!named.window)
                scratchpad_pending(named.name, true);
            else if (find(*named.window))
                claim_scratchpad(named.name, *named.window);
        }
    for (auto window : snapshot.pool)
        if (find(window))
            pool_scratchpad(window);
}

} // namespace lwm
