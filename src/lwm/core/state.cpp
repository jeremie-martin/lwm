#include "state.hpp"
#include "classification.hpp"
#include "floating.hpp"
#include "focus.hpp"
#include "log.hpp"
#include "workarea.hpp"
#include <algorithm>
#include <cassert>
#include <tuple>

namespace lwm {

namespace {

// An index stepped any distance around a cycle of `size` positions.
size_t wrap(int64_t index, size_t size)
{
    auto count = static_cast<int64_t>(size);
    return static_cast<size_t>((index % count + count) % count);
}

// One geometry policy for clients affected by live or restart-time topology changes.
Geometry fit_floating(Geometry rectangle, Geometry area, bool displaced)
{
    return displaced ? floating::place_floating(area, rectangle.width, rectangle.height, std::nullopt)
                     : floating::clamp_to_area(area, rectangle);
}

void merge_membership(Workspace& target, Workspace& source)
{
    target.windows.insert(target.windows.end(), source.windows.begin(), source.windows.end());
    if (target.preferred_tile == XCB_NONE)
        target.preferred_tile = source.preferred_tile;
}

// Transfer complete workspace state for surviving outputs; discovery supplies
// fresh geometry. A smaller count folds removed workspaces into the last one;
// additional workspaces keep defaults. Removed outputs fall back to output 0
// after surviving members, keeping their relative order.
std::vector<size_t> preserve_workspaces(std::span<Monitor> previous, std::span<Monitor> discovered)
{
    std::vector<size_t> destinations(previous.size(), 0);
    for (size_t old = 0; old < previous.size(); ++old)
    {
        auto target = std::ranges::find(discovered, previous[old].name, &Monitor::name);
        if (target == discovered.end())
            continue;
        destinations[old] = static_cast<size_t>(target - discovered.begin());
        auto& source = previous[old];
        size_t last = target->workspaces.size() - 1;
        for (size_t w = 0; w < source.workspaces.size(); ++w)
            if (w <= last)
                target->workspaces[w] = std::move(source.workspaces[w]);
            else
                merge_membership(target->workspaces[last], source.workspaces[w]);
        source.workspaces.clear();
        target->current_workspace = std::min(source.current_workspace, last);
        target->previous_workspace = std::min(source.previous_workspace, last);
    }
    for (auto& source : previous)
        for (size_t w = 0; w < source.workspaces.size(); ++w)
            merge_membership(discovered[0].workspaces[std::min(w, discovered[0].workspaces.size() - 1)], source.workspaces[w]);
    return destinations;
}

} // namespace


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

std::vector<xcb_window_t> State::registered_windows() const
{
    std::vector<std::pair<uint64_t, xcb_window_t>> ranked;
    for (auto const& [id, client] : clients_) ranked.emplace_back(client.order, id);
    for (auto const& [id, fixture] : fixtures_) ranked.emplace_back(fixture.order, id);
    std::ranges::sort(ranked);
    std::vector<xcb_window_t> windows;
    for (auto const& [order, id] : ranked) windows.push_back(id);
    return windows;
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
    client.order = register_window(client.id);
    client.mru_order = 0;
    if (client.fullscreen())
        client.fullscreen_claim = next_claim_++;
    insert_registered(std::move(client));
}

void State::insert_registered(Client client)
{
    assert(client.monitor < monitors_.size() && client.workspace < monitors_[client.monitor].workspaces.size());
    assert(client.order < next_order_);
    mutated();
    forget_missing_tile_slot(client);
    if (client.fullscreen())
        client.maximized_horz = client.maximized_vert = false;
    auto [it, inserted] = clients_.emplace(client.id, std::move(client));
    assert(inserted);
    if (it->second.tiled())
        attach(it->second);
}

void State::insert_fixture(xcb_window_t id, Fixture::Role role, DockStrut strut)
{
    mutated();
    fixtures_.emplace(id, Fixture{ { id, role, register_window(id) }, strut });
}

void State::reserve(xcb_window_t id, DockStrut strut)
{
    auto it = fixtures_.find(id);
    if (it == fixtures_.end() || it->second.strut == strut)
        return;
    mutated();
    it->second.strut = strut;
}

void State::erase(xcb_window_t id)
{
    closing_.erase(id);
    if (fixtures_.erase(id))
    {
        mutated();
        return;
    }
    auto const* client = find(id);
    if (!client)
        return;
    mutated();
    if (client->tiled())
        detach(*client);
    release_scratchpad(id);
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
    if (ws.preferred_tile == client.id)
        ws.preferred_tile = XCB_NONE;
    return slot;
}

// Derived views

bool State::shows(size_t monitor, size_t workspace) const
{
    return workspace == monitors_[monitor].current_workspace;
}

bool State::in_view(Client const& client) const
{
    return !client.iconic && (client.sticky || shows(client.monitor, client.workspace));
}

// The most recent fullscreen claim among clients in view owns each monitor.
std::vector<xcb_window_t> State::fullscreen_owners() const
{
    std::vector<xcb_window_t> owners(monitors_.size(), XCB_NONE);
    std::vector<uint64_t> claims(monitors_.size(), 0);
    for (auto const& [id, client] : clients_)
        if (client.fullscreen_claim > claims[client.monitor] && in_view(client))
        {
            claims[client.monitor] = client.fullscreen_claim;
            owners[client.monitor] = id;
        }
    return owners;
}

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
    return accepts_focus(client) && visible(client, fullscreen);
}

// Focus

void State::focus(xcb_window_t id, uint32_t time, bool record_user_time)
{
    if (id != XCB_NONE)
    {
        auto const* client = find(id);
        if (!client || !accepts_focus(*client))
            return;
        iconic(id, false);
        focus_monitor(client->monitor);
        if (!client->sticky)
            switch_workspace(client->monitor, client->workspace);
        if (!visible(*client))
            return focus_fallback(client->monitor, false);
    }
    select_focus(id, time, record_user_time);
    focus_released_ = id == XCB_NONE;
}

void State::focus_fallback(size_t monitor, bool record_user_time)
{
    select_focus(focus::fallback(*this, monitor), 0, record_user_time);
}

// Shows a hidden client and focuses it when it is in view on the focused monitor.
void State::restore(xcb_window_t id)
{
    auto const* client = find(id);
    if (!client)
        return;
    iconic(id, false);
    if (client->monitor == focused_monitor_ && in_view(*client))
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
    focus_released_ = false;
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

std::optional<uint32_t> State::settle(uint32_t input_time)
{
    if (drag_ && !drag_valid())
        end_drag(false);
    return complete_focus(input_time);
}

std::optional<uint32_t> State::complete_focus(uint32_t input_time)
{
    // Repair a selection that lost its client or eligibility, and fill an empty
    // one unless the user released focus.
    auto const* active = find(active_window_);
    if (active ? !focusable(*active) : active_window_ != XCB_NONE || !focus_released_)
        if (auto target = focus::fallback(*this, focused_monitor_); target != active_window_)
            select_focus(target, 0, false);
    auto request = std::exchange(focus_request_, std::nullopt);
    if (request && find(active_window_))
    {
        auto& client = edit(active_window_);
        client.mru_order = next_recency_++;
        uint32_t time = request->time ? request->time : input_time;
        if (request->record_user_time && time
            && (!client.user_time || !timestamp_is_before(time, client.user_time)))
            client.user_time = time;
        if (client.tiled())
            edit_workspace(client.monitor, client.workspace).preferred_tile = XCB_NONE;
        clear_urgency(client.id);
    }
    return request ? std::optional{ request->time } : std::nullopt;
}

void State::hover(xcb_window_t window, int16_t x, int16_t y)
{
    if (drag_)
        return;
    if (auto const* client = find(window))
    {
        if (window != active_window_ && visible(*client))
            focus(window);
        return;
    }
    auto monitor = monitor_at(monitors_, x, y);
    if (!monitor || *monitor == focused_monitor_)
        return;
    LWM_LOG_TRACE("Pointer changed monitor: {} -> {}", focused_monitor_, *monitor);
    focus_monitor(*monitor);
    focus(XCB_NONE);
}

bool State::focus_adjacent_monitor(int direction)
{
    if (monitors_.size() <= 1)
        return false;
    size_t target = wrap(static_cast<int>(focused_monitor_) + direction, monitors_.size());
    focus_monitor(target);
    focus_fallback(target);
    return true;
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
    auto& client = clients_.at(id);
    bool tiled = client.tiled();
    size_t source = client.monitor;
    if (source == monitor && client.workspace == workspace)
    {
        if (!tiled || !tile_index)
            return true;
        auto& windows = monitors_[monitor].workspaces[workspace].windows;
        auto from = std::ranges::find(windows, client.id);
        auto target = windows.begin() + static_cast<std::ptrdiff_t>(std::min(*tile_index, windows.size() - 1));
        // Reordering keeps membership and destination preference.
        if (from != target)
            mutated();
        if (from < target)
            std::rotate(from, from + 1, target + 1);
        else if (target < from)
            std::rotate(target, from, from + 1);
        return true;
    }
    mutated();
    if (tiled)
        detach(client);
    else if (source != monitor && geometry != RelocationGeometry::Preserve)
    {
        auto& rectangle = floating_mode(client)->geometry;
        auto from = working_area(monitors_[source]);
        auto to = working_area(monitors_[monitor]);
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
    if (active && in_view(client))
        focus_monitor(monitor);
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
    auto& client = clients_.at(id);
    if (floating != client.tiled())
        return;
    mutated();
    LWM_LOG_DEBUG("Client kind changed: window={:#x} floating={}", id, floating);
    if (auto* tiled = tiled_mode(client))
    {
        auto rectangle = floating::recover_to_area(
            working_area(monitors_[client.monitor]),
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

// Explicit preferences replace classification defaults; an unchanged one is no mutation.
template <typename T> void State::prefer(xcb_window_t id, std::optional<T> ClientPreferences::* field, T value)
{
    auto preferences = require(id).preferences;
    preferences.*field = value;
    assign(id, &Client::preferences, preferences);
}

void State::floating(xcb_window_t id, bool enabled)
{
    prefer(id, &ClientPreferences::floating, enabled);
    set_mode(id, enabled);
}

void State::toggle_floating(xcb_window_t id)
{
    auto const& client = require(id);
    if (client.fullscreen() || client.iconic)
        return;
    bool floating = !client.tiled();
    if (floating)
        maximize(id, false, false);
    this->floating(id, !floating);
    focus(id);
}

bool State::move_to_monitor(int direction)
{
    auto const* client = find(active_window_);
    if (!client || monitors_.size() <= 1)
        return false;
    size_t target = wrap(static_cast<int>(client->monitor) + direction, monitors_.size());
    if (!relocate(client->id, target, monitors_[target].current_workspace, RelocationGeometry::Center))
        return false;
    focus(client->id);
    return true;
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

void State::swap_tile(int offset)
{
    auto const& workspace = monitors_[focused_monitor_].current();
    auto fullscreen = fullscreen_visibility();
    std::vector<size_t> eligible;
    for (size_t i = 0; i < workspace.windows.size(); ++i)
        if (auto const& client = require(workspace.windows[i]); !client.fullscreen() && visible(client, fullscreen))
            eligible.push_back(i);
    auto tile = focus::tile(*this, focused_monitor_, fullscreen);
    auto it = std::ranges::find_if(eligible, [&](auto i) { return workspace.windows[i] == tile; });
    if (it == eligible.end() || eligible.size() < 2)
        return;
    size_t other = eligible[wrap(static_cast<int>(it - eligible.begin()) + offset, eligible.size())];
    if (workspace.layout_strategy == LayoutStrategy::Monocle)
        focus(workspace.windows[other]);
    else
        swap_tiles(focused_monitor_, *it, other);
}

// Client state

void State::iconic(xcb_window_t id, bool enabled)
{
    if (require(id).iconic == enabled)
        return;
    assert(!enabled || scratchpad_claim(id) || pooled(id));
    auto& c = edit(id);
    c.iconic = enabled;
    auto& workspace = monitors_[c.monitor].workspaces[c.workspace];
    if (enabled && workspace.preferred_tile == id)
        workspace.preferred_tile = XCB_NONE;
    // Restoring a fullscreen client makes it the preferred owner again.
    if (!enabled && c.fullscreen())
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
    if (require(id).fullscreen() == enabled)
        return;
    if (enabled)
        return request_fullscreen(id);
    auto& c = edit(id);
    LWM_LOG_DEBUG("Fullscreen changed: window={:#x} enabled={}", id, false);
    c.fullscreen_claim = 0;
}

void State::request_fullscreen(xcb_window_t id)
{
    auto& c = edit(id);
    if (!c.fullscreen())
        LWM_LOG_DEBUG("Fullscreen changed: window={:#x} enabled={}", id, true);
    c.fullscreen_claim = next_claim_++;
    c.maximized_horz = c.maximized_vert = false;
}

void State::maximize(xcb_window_t id, bool horizontal, bool vertical)
{
    auto const& current = require(id);
    if (current.fullscreen())
        horizontal = vertical = false;
    if (current.maximized_horz == horizontal && current.maximized_vert == vertical)
        return;
    auto& c = edit(id);
    c.maximized_horz = horizontal;
    c.maximized_vert = vertical;
}

void State::modal(xcb_window_t id, bool enabled) { assign(id, &Client::modal, enabled); }

void State::layer(xcb_window_t id, LayerHint hint) { prefer(id, &ClientPreferences::layer, hint); }

void State::skip_taskbar(xcb_window_t id, bool enabled) { prefer(id, &ClientPreferences::skip_taskbar, enabled); }

void State::skip_pager(xcb_window_t id, bool enabled) { prefer(id, &ClientPreferences::skip_pager, enabled); }

// The active window already has the user's attention.
void State::urgency(xcb_window_t id, UrgencySource source, bool enabled)
{
    if (enabled && id == active_window_)
        return;
    auto urgency = require(id).urgency;
    urgency.set(source, enabled);
    assign(id, &Client::urgency, urgency);
}

void State::clear_urgency(xcb_window_t id) { assign(id, &Client::urgency, Urgency{ }); }

void State::fullscreen_monitors(xcb_window_t id, std::optional<FullscreenMonitors> value)
{
    assign(id, &Client::fullscreen_monitors, value);
}

void State::pin_desktop(xcb_window_t id, bool pinned) { assign(id, &Client::desktop_pinned, pinned); }

// Metadata

void State::focus_hints(xcb_window_t id, bool input, bool take_focus)
{
    assign(id, &Client::accepts_input, input);
    assign(id, &Client::supports_take_focus, take_focus);
}

// Activation-time bookkeeping is not exposed or published, so it is not a revision.
// _NET_WM_USER_TIME lives on a client's user-time window when it names one.
void State::user_time(xcb_window_t source, uint32_t time)
{
    assert(!frozen_);
    for (auto& [id, client] : clients_)
        if ((client.user_time_window != XCB_NONE ? client.user_time_window : id) == source)
            client.user_time = time;
}

void State::user_time_window(xcb_window_t id, xcb_window_t window, uint32_t time)
{
    assert(!frozen_);
    auto& client = clients_.at(id);
    client.user_time_window = window;
    client.user_time = time;
}

// Workspaces and monitors

uint32_t State::desktop_index(size_t monitor, size_t workspace) const
{
    return static_cast<uint32_t>(monitor * config_.workspaces.size() + workspace);
}

std::optional<std::pair<size_t, size_t>> State::desktop_placement(uint32_t desktop) const
{
    size_t count = config_.workspaces.size();
    if (desktop == STICKY_DESKTOP || desktop / count >= monitors_.size())
        return std::nullopt;
    return std::pair<size_t, size_t>{ desktop / count, desktop % count };
}

bool State::switch_workspace(size_t monitor, size_t workspace)
{
    if (monitor >= monitors_.size())
        return false;
    auto& m = monitors_[monitor];
    if (workspace >= m.workspaces.size() || workspace == m.current_workspace)
        return false;
    mutated();
    LWM_LOG_DEBUG("Workspace changed: monitor={} workspace={} -> {}", monitor, m.current_workspace, workspace);
    m.previous_workspace = m.current_workspace;
    m.current_workspace = workspace;
    if (monitor == focused_monitor_)
        focus_fallback(monitor);
    return true;
}

void State::cycle_workspace(int step)
{
    auto const& monitor = monitors_[focused_monitor_];
    switch_workspace(focused_monitor_, wrap(static_cast<int>(monitor.current_workspace) + step, monitor.workspaces.size()));
}

void State::toggle_workspace() { switch_workspace(focused_monitor_, monitors_[focused_monitor_].previous_workspace); }

void State::layout(size_t monitor, LayoutStrategy strategy)
{
    if (monitors_.at(monitor).current().layout_strategy != strategy)
        edit_workspace(monitor, monitors_[monitor].current_workspace).layout_strategy = strategy;
}

void State::ratio(size_t monitor, SplitAddress address, double value)
{
    value = config_.layout.clamp_ratio(value);
    auto const& ratios = monitors_.at(monitor).current().split_ratios;
    if (auto it = ratios.find(address); it == ratios.end() || it->second != value)
        edit_workspace(monitor, monitors_[monitor].current_workspace).split_ratios[address] = value;
}

bool State::set_ratio(double value)
{
    if (!config_.layout.accepts_ratio(value))
        return false;
    ratio(focused_monitor_, SplitAddress{ 0 }, value);
    return true;
}

void State::adjust_ratio(double delta)
{
    auto const& ratios = monitors_[focused_monitor_].current().split_ratios;
    auto it = ratios.find(SplitAddress{ 0 });
    double current = it == ratios.end() ? config_.layout.default_ratio : it->second;
    if (double adjusted = config_.layout.clamp_ratio(current + delta); adjusted != current)
        ratio(focused_monitor_, SplitAddress{ 0 }, adjusted);
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

// Configuration

void State::configure(Config config)
{
    mutated();
    end_drag(false);
    config_ = std::move(config);
    // A changed workspace count rebinds like a topology change: windows beyond
    // the new count fold into the last workspace.
    if (!monitors_.empty())
    {
        std::vector<Monitor> monitors;
        for (auto const& monitor : monitors_) monitors.push_back(fresh_monitor(monitor.name, monitor.geometry));
        rebind(std::move(monitors));
    }
    reconcile_scratchpads();
    reapply_rules();
}

// Topology

Monitor State::fresh_monitor(std::string name, Geometry geometry) const
{
    Monitor monitor;
    monitor.name = std::move(name);
    monitor.geometry = geometry;
    Workspace workspace;
    workspace.layout_strategy = config_.layout.strategy;
    monitor.workspaces.assign(config_.workspaces.size(), workspace);
    return monitor;
}

void State::replace_topology(Topology topology)
{
    assert(!topology.outputs.empty());
    std::vector<Monitor> monitors;
    for (auto& output : topology.outputs) monitors.push_back(fresh_monitor(std::move(output.name), output.geometry));
    // The root extent shapes dock projections even when the outputs are unchanged.
    if (!(screen_ == topology.screen))
        mutated();
    screen_ = topology.screen;
    rebind(std::move(monitors));
}

// Each monitor reserves the deepest projection of any dock on each edge.
Geometry State::working_area(Monitor const& monitor) const
{
    Strut strut;
    for (auto const& [id, fixture] : fixtures_)
    {
        if (fixture.role != Fixture::Role::Dock)
            continue;
        auto projected = monitor_strut(fixture.strut, screen_, monitor.geometry);
        strut.left = std::max(strut.left, projected.left);
        strut.right = std::max(strut.right, projected.right);
        strut.top = std::max(strut.top, projected.top);
        strut.bottom = std::max(strut.bottom, projected.bottom);
    }
    return lwm::working_area(monitor.geometry, strut);
}

void State::rebind(std::vector<Monitor> monitors)
{
    assert(!monitors.empty());
    // Configuration changes and restoration share reconciliation before the
    // unchanged-topology fast path; inactive workspaces obey the same bounds.
    for (auto& monitor : monitors_)
        for (auto& workspace : monitor.workspaces)
            for (auto& [address, ratio] : workspace.split_ratios)
                if (auto bounded = config_.layout.clamp_ratio(ratio); bounded != ratio)
                {
                    mutated();
                    ratio = bounded;
                }
    bool topology_changed = monitors_.size() != monitors.size()
        || !std::ranges::equal(monitors_,
                               monitors,
                               [](auto const& a, auto const& b)
                               { return a.name == b.name && a.geometry == b.geometry; });
    if (!topology_changed && std::ranges::equal(monitors_, monitors, [](auto const& a, auto const& b)
        { return a.workspaces.size() == b.workspaces.size(); }))
    {
        // The root extent can change without changing the outputs; its only
        // domain consequence is the derived dock projection. Settle validates drags.
        return;
    }
    mutated();
    end_drag(false);
    focus_cycle_.clear();
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
    auto destinations = preserve_workspaces(previous, monitors);
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
        // An unchanged topology preserves intentional off-workarea geometry
        // and index-based monitor hints.
        if (topology_changed)
        {
            c.fullscreen_monitors.reset();
            if (auto* mode = floating_mode(c))
                mode->geometry = fit_floating(mode->geometry, working_area(monitors_[c.monitor]), displaced);
        }
    }
}

// Exec handoff

restart::Snapshot State::snapshot() const
{
    restart::Snapshot snapshot;
    for (auto const& [id, fixture] : fixtures_) snapshot.fixtures.push_back(fixture);
    std::ranges::sort(snapshot.fixtures, {}, &Fixture::order);
    snapshot.focused_monitor = focused_monitor_;
    snapshot.active = active_window_;
    for (auto const& monitor : monitors_) snapshot.monitors.push_back(monitor);
    for (auto const* client : clients_by_order()) snapshot.clients.push_back(*client);
    for (auto const& slot : named_scratchpads_)
        if (slot.claimed_window() != XCB_NONE || slot.pending_launch())
            snapshot.named_scratchpads.push_back(slot);
    snapshot.pool = scratchpad_pool_;
    return snapshot;
}

} // namespace lwm
