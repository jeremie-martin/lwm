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

void State::insert(Client client)
{
    assert(client.monitor < monitors_.size() && client.workspace < monitors_[client.monitor].workspaces.size());
    mutated();
    client.order = next_order_++;
    if (client.kind() == Client::Kind::Floating)
        client.mru_order = next_recency_++;
    if (client.fullscreen)
        client.fullscreen_claim = ++next_fullscreen_claim_;
    auto [it, inserted] = clients_.emplace(client.id, std::move(client));
    assert(inserted);
    if (it->second.kind() == Client::Kind::Tiled)
        attach(it->second);
}

void State::insert_fixture(xcb_window_t id, Fixture::Role role)
{
    mutated();
    fixtures_.emplace(id, Fixture{ id, role, next_order_++ });
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
    TileSlot slot{ static_cast<size_t>(it - ws.windows.begin()), client.monitor, client.workspace };
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

xcb_window_t State::fullscreen_owner(size_t monitor) const { return fullscreen_owners().at(monitor); }

bool State::suppressed(Client const& client, xcb_window_t owner)
{
    return owner != XCB_NONE && owner != client.id && owner != client.transient_for;
}

bool State::visible(Client const& client) const { return visible(client, fullscreen_owner(client.monitor)); }

bool State::visible(Client const& client, xcb_window_t owner) const
{
    return in_view(client) && !suppressed(client, owner);
}

bool State::focusable(Client const& client) const
{
    return accepts_focus(client) && !showing_desktop_ && visible(client);
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

void State::set_mode(xcb_window_t id, bool floating)
{
    auto& client = edit(id);
    if (floating == (client.kind() == Client::Kind::Floating))
        return;
    LWM_LOG_DEBUG("Client kind changed: window={:#x} floating={}", id, floating);
    client.suppress_next_configure_request = false;
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
    if (mode.tile_slot && mode.tile_slot->monitor == client.monitor && mode.tile_slot->workspace == client.workspace)
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
        c.fullscreen_claim = ++next_fullscreen_claim_;
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
    if (!enabled && !require(id).fullscreen)
        return;
    auto& c = edit(id);
    if (c.fullscreen != enabled)
        LWM_LOG_DEBUG("Fullscreen changed: window={:#x} enabled={}", id, enabled);
    if (enabled)
    {
        // Fullscreen supersedes maximize; an explicit request claims ownership.
        c.maximized_horz = c.maximized_vert = false;
        c.fullscreen_claim = ++next_fullscreen_claim_;
    }
    c.fullscreen = enabled;
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

void State::configure_suppression(xcb_window_t id, bool enabled)
{
    assign(id, &Client::suppress_next_configure_request, enabled);
}

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
        if (auto* mode = floating_mode(c))
        {
            auto area = monitors_[c.monitor].working_area();
            mode->geometry = displaced
                ? floating::place_floating(area, mode->geometry.width, mode->geometry.height, std::nullopt)
                : floating::clamp_to_area(area, mode->geometry);
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
    snapshot.focused_monitor = focused_monitor_;
    snapshot.active = active_window_;
    snapshot.showing_desktop = showing_desktop_;
    for (auto const& monitor : monitors_)
    {
        auto& record = snapshot.monitors.emplace_back();
        record.current = monitor.current_workspace;
        record.previous = monitor.previous_workspace;
        for (auto const& workspace : monitor.workspaces)
            record.workspaces.push_back(
                { workspace.layout_strategy, workspace.split_ratios, workspace.windows, workspace.focused_window }
            );
    }
    std::vector<Client const*> recency;
    for (auto const& [id, client] : clients_) recency.push_back(&client);
    std::ranges::sort(recency, {}, [](Client const* c) { return std::tie(c->mru_order, c->order); });
    for (auto const* c : recency)
    {
        auto const* tiled = tiled_mode(*c);
        snapshot.clients.push_back({ c->id,
                                     c->monitor,
                                     c->workspace,
                                     c->kind(),
                                     tiled ? tiled->layout : floating_mode(*c)->geometry,
                                     tiled ? tiled->floating : std::nullopt,
                                     c->preferences,
                                     c->urgency.sources,
                                     c->borderless,
                                     c->desktop_pinned });
    }
    for (auto const& slot : named_scratchpads_)
        if (slot.window() != XCB_NONE)
            snapshot.named_scratchpads.push_back({ slot.name, slot.window() });
    snapshot.pool = scratchpad_pool_;
    return snapshot;
}

void State::restore_workspaces(restart::Snapshot const& snapshot)
{
    mutated();
    focused_monitor_ = snapshot.focused_monitor < monitors_.size() ? snapshot.focused_monitor : 0;
    showing_desktop_ = snapshot.showing_desktop;
    for (size_t m = 0; m < std::min(monitors_.size(), snapshot.monitors.size()); ++m)
    {
        auto& monitor = monitors_[m];
        auto const& record = snapshot.monitors[m];
        size_t last = monitor.workspaces.size() - 1;
        monitor.current_workspace = std::min(record.current, last);
        monitor.previous_workspace = std::min(record.previous, last);
        for (size_t w = 0; w < std::min(monitor.workspaces.size(), record.workspaces.size()); ++w)
        {
            monitor.workspaces[w].layout_strategy = record.workspaces[w].strategy;
            monitor.workspaces[w].split_ratios = record.workspaces[w].ratios;
        }
    }
}

void State::restore_membership(restart::Snapshot const& snapshot)
{
    mutated();
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
        if (find(named.window) && named_scratchpad(named.name))
            claim_scratchpad(named.name, named.window);
    for (auto window : snapshot.pool)
        if (find(window))
            pool_scratchpad(window);
}

} // namespace lwm
