#include "state.hpp"
#include "ewmh.hpp"
#include "floating.hpp"
#include "policy.hpp"
#include <algorithm>
#include <cassert>

namespace lwm {
Client const* State::find(xcb_window_t id) const
{
    auto it = clients_.find(id);
    return it == clients_.end() ? nullptr : &it->second;
}
Client const& State::require(xcb_window_t id) const { return clients_.at(id); }
Client& State::edit(xcb_window_t id)
{
    assert(!publishing_);
    return clients_.at(id);
}
ClientPresentation& State::presentation(xcb_window_t id) { return clients_.at(id).presentation; }
State::TransitionEffects State::begin_publication()
{
    assert(!publishing_);
    assert(effects_.monitors.empty() && effects_.layouts.empty() && !effects_.workareas);
    publishing_ = true;
    return std::exchange(effects_, { });
}
void State::end_publication()
{
    assert(publishing_ && effects_ == TransitionEffects{ });
    publishing_ = false;
}
void State::changed(xcb_window_t) { effects_.state_changed = true; }
void State::request_geometry(xcb_window_t id) { effects_.geometry.push_back(id); }
void State::invalidate(size_t monitor, xcb_window_t preferred)
{
    if (monitor >= monitors_.size())
        return;
    auto [it, inserted] = effects_.monitors.try_emplace(monitor, preferred);
    if (preferred != XCB_NONE)
        it->second = preferred;
    effects_.stacking = true;
}
void State::focus_monitor(size_t monitor)
{
    assert(!publishing_);
    if (monitor < monitors_.size() && focused_monitor_ != monitor)
    {
        focused_monitor_ = monitor;
        effects_.current_desktop = true;
    }
}
void State::focus(xcb_window_t id, uint32_t time)
{
    assert(!publishing_);
    if (!effects_.previous_focus)
        effects_.previous_focus = active_window_;
    effects_.focus_time = time;
    active_window_ = id;
    effects_.current_desktop = true;
    effects_.stacking = true;
    if (auto const* c = find(id))
    {
        focus_monitor(c->monitor);
        if (c->kind() == Client::Kind::Tiled)
            remember_focus(c->monitor, c->workspace, id);
        touch(id);
    }
}
void State::showing_desktop(bool enabled)
{
    assert(!publishing_);
    if (showing_desktop_ == enabled)
        return;
    showing_desktop_ = enabled;
    effects_.showing_desktop = true;
    for (size_t i = 0; i < monitors_.size(); ++i) invalidate(i);
}
void State::restore_focus(size_t monitor, xcb_window_t id, bool desktop)
{
    assert(!publishing_);
    focused_monitor_ = monitor < monitors_.size() ? monitor : 0;
    active_window_ = id;
    showing_desktop_ = desktop;
}
void State::insert(Client client)
{
    assert(!publishing_);
    client.order = next_client_order_++;
    if (client.kind() == Client::Kind::Floating)
        client.mru_order = next_mru_order_++;
    auto [it, inserted] = clients_.emplace(client.id, std::move(client));
    assert(inserted);
    classification(it->first);
    if (it->second.kind() == Client::Kind::Tiled)
        attach(it->second);
    effects_.client_list = true;
    if (it->second.kind() == Client::Kind::Dock)
        effects_.workareas = true;
    else
        invalidate(it->second.monitor);
    if (it->second.kind() == Client::Kind::Tiled || it->second.kind() == Client::Kind::Floating)
        effects_.states.insert(it->first);
    changed(it->first);
}
void State::erase(xcb_window_t id)
{
    assert(!publishing_);
    auto const* client = find(id);
    if (!client)
        return;
    if (client->kind() == Client::Kind::Tiled)
        detach(*client);
    if (client->kind() == Client::Kind::Dock)
        effects_.workareas = true;
    else
        invalidate(client->monitor);
    scratchpad(id, std::nullopt);
    clients_.erase(id);
    effects_.client_list = true;
    effects_.state_changed = true;
}
void State::attach(Client const& client, std::optional<size_t> index)
{
    auto& windows = monitors_[client.monitor].workspaces[client.workspace].windows;
    auto position = std::min(index.value_or(windows.size()), windows.size());
    windows.insert(windows.begin() + static_cast<std::ptrdiff_t>(position), client.id);
}

std::optional<SavedTilePos> State::detach(Client const& client)
{
    auto& ws = monitors_[client.monitor].workspaces[client.workspace];
    auto it = ws.find_window(client.id);
    if (it == ws.windows.end())
        return std::nullopt;
    SavedTilePos position{ static_cast<size_t>(it - ws.windows.begin()), client.monitor, client.workspace };
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
    return position;
}

bool State::relocate(
    xcb_window_t id,
    size_t monitor,
    size_t workspace,
    RelocationGeometry geometry,
    std::optional<size_t> tile_index
)
{
    auto& client = edit(id);
    if (client.kind() != Client::Kind::Tiled && client.kind() != Client::Kind::Floating)
        return false;
    if (monitor >= monitors_.size() || workspace >= monitors_[monitor].workspaces.size())
        return false;
    bool tiled = client.kind() == Client::Kind::Tiled;
    size_t source = client.monitor;
    if (source == monitor && client.workspace == workspace)
    {
        if (!tiled || !tile_index)
            return true;
        auto& windows = monitors_[monitor].workspaces[workspace].windows;
        auto from = std::ranges::find(windows, client.id);
        if (from == windows.end())
            return false;
        auto target = windows.begin() + static_cast<std::ptrdiff_t>(std::min(*tile_index, windows.size() - 1));
        // Reordering does not remove membership or discard remembered focus.
        if (from < target)
            std::rotate(from, from + 1, target + 1);
        else if (target < from)
            std::rotate(target, from, from + 1);
        else
            return true;
        invalidate(monitor);
        return true;
    }
    if (tiled && !detach(client))
        return false;
    if (!tiled && source != monitor && geometry == RelocationGeometry::CenterOnMonitorChange)
    {
        auto& rectangle = floating_geometry(client);
        rectangle = floating::place_floating(
            monitors_[monitor].working_area(),
            rectangle.width,
            rectangle.height,
            std::nullopt
        );
    }
    client.monitor = monitor;
    client.workspace = workspace;
    effects_.desktops.insert(id);
    changed(id);
    if (tiled)
        attach(client, tile_index);
    else
        request_geometry(client.id);
    invalidate(source);
    invalidate(monitor);
    return true;
}

void State::change_kind(xcb_window_t id, ClientState state, std::optional<size_t> tile_index)
{
    auto& client = edit(id);
    assert(client.kind() == Client::Kind::Tiled || client.kind() == Client::Kind::Floating);
    assert(std::holds_alternative<TiledState>(state) || std::holds_alternative<FloatingState>(state));
    bool was_tiled = client.kind() == Client::Kind::Tiled;
    bool tiled = std::holds_alternative<TiledState>(state);
    if (was_tiled && !tiled)
        std::get<FloatingState>(state).saved_tiled_pos = detach(client);
    client.state = std::move(state);
    if (!was_tiled && tiled)
        attach(client, tile_index);
    client.suppress_next_configure_request = false;
    effects_.allowed_actions.insert(id);
    changed(id);
    request_geometry(client.id);
    invalidate(client.monitor);
}

void State::geometry(xcb_window_t id, Geometry rectangle)
{
    auto& c = edit(id);
    if (c.kind() != Client::Kind::Floating || floating_geometry(c) == rectangle)
        return;
    floating_geometry(c) = rectangle;
    request_geometry(id);
    effects_.state_changed = true;
}
void State::tiled_geometry(xcb_window_t id, Geometry rectangle)
{
    edit(id).tiled_geometry = rectangle;
    request_geometry(id);
}
void State::iconic(xcb_window_t id, bool enabled)
{
    auto& c = edit(id);
    if (c.iconic == enabled)
        return;
    c.iconic = enabled;
    if (enabled && c.kind() == Client::Kind::Tiled)
        workspace_policy::fixup_workspace_focus(
            monitors_[c.monitor].workspaces[c.workspace],
            id,
            [this](auto w) { return require(w).iconic; }
        );
    effects_.states.insert(id);
    effects_.iconic.insert(id);
    invalidate(c.monitor, !enabled && c.fullscreen ? id : XCB_NONE);
    changed(id);
}
void State::sticky(xcb_window_t id, bool enabled)
{
    auto& c = edit(id);
    if (c.sticky == enabled)
        return;
    c.sticky = enabled;
    if (enabled)
        c.desktop_pinned = false;
    effects_.states.insert(id);
    effects_.desktops.insert(id);
    invalidate(c.monitor);
    changed(id);
}
void State::borderless(xcb_window_t id, bool enabled)
{
    auto& c = edit(id);
    if (c.borderless == enabled)
        return;
    c.borderless = enabled;
    request_geometry(id);
    effects_.allowed_actions.insert(id);
    changed(id);
}
void State::classification(xcb_window_t id, bool update_mode)
{
    auto& c = edit(id);
    auto defaults = classify_window_type(c.ewmh_type, c.transient_for != XCB_NONE);
    bool normal = c.kind() == Client::Kind::Tiled || c.kind() == Client::Kind::Floating;
    bool default_normal =
        defaults.kind == WindowClassification::Kind::Tiled || defaults.kind == WindowClassification::Kind::Floating;
    if (!normal || !default_normal)
        return;
    if (update_mode && !c.scratchpad)
    {
        bool floating = c.preferences.floating.value_or(defaults.kind == WindowClassification::Kind::Floating);
        if (floating && c.kind() == Client::Kind::Tiled)
        {
            auto rectangle = prior_floating_geometry(c).value_or(c.tiled_geometry);
            change_kind(id, FloatingState{ rectangle });
            touch(id);
        }
        else if (!floating && c.kind() == Client::Kind::Floating)
        {
            auto prior = floating_geometry(c);
            auto saved = saved_tiled_pos(c);
            std::optional<size_t> index;
            if (saved && saved->monitor == c.monitor && saved->workspace == c.workspace)
                index = saved->index;
            change_kind(id, TiledState{ prior }, index);
        }
    }
    bool taskbar = c.preferences.skip_taskbar.value_or(defaults.skip_taskbar || c.transient_for != XCB_NONE);
    bool pager = c.preferences.skip_pager.value_or(defaults.skip_pager || c.transient_for != XCB_NONE);
    auto layer = c.fullscreen ? LayerHint::Normal
        : c.modal             ? LayerHint::Above
                              : c.preferences.layer.value_or(defaults.above ? LayerHint::Above : LayerHint::Normal);
    if (c.skip_taskbar == taskbar && c.skip_pager == pager && c.layer_hint == layer)
        return;
    effects_.stacking |= c.layer_hint != layer;
    c.skip_taskbar = taskbar;
    c.skip_pager = pager;
    c.layer_hint = layer;
    effects_.states.insert(id);
    changed(id);
}
void State::floating_preference(xcb_window_t id, bool enabled)
{
    edit(id).preferences.floating = enabled;
    changed(id);
}
void State::layer(xcb_window_t id, LayerHint hint)
{
    edit(id).preferences.layer = hint;
    classification(id);
}
void State::maximize(xcb_window_t id, bool horizontal, bool vertical)
{
    auto& c = edit(id);
    if (c.fullscreen)
        horizontal = vertical = false;
    if (c.maximized_horz == horizontal && c.maximized_vert == vertical)
        return;
    c.maximized_horz = horizontal;
    c.maximized_vert = vertical;
    if (c.kind() == Client::Kind::Floating)
        request_geometry(id);
    effects_.states.insert(id);
    changed(id);
}
void State::modal(xcb_window_t id, bool enabled)
{
    auto& c = edit(id);
    if (c.modal == enabled)
        return;
    c.modal = enabled;
    classification(id);
    effects_.states.insert(id);
    changed(id);
}
void State::fullscreen(xcb_window_t id, bool enabled)
{
    auto& c = edit(id);
    if (c.kind() != Client::Kind::Tiled && c.kind() != Client::Kind::Floating)
        return;
    if (!enabled && !c.fullscreen)
        return;
    if (enabled)
        maximize(id, false, false);
    c.fullscreen = enabled;
    classification(id);
    invalidate(c.monitor, enabled ? id : XCB_NONE);
    effects_.states.insert(id);
    effects_.repair_focus = true;
    changed(id);
}
void State::skip_taskbar(xcb_window_t id, bool enabled)
{
    edit(id).preferences.skip_taskbar = enabled;
    classification(id);
}
void State::skip_pager(xcb_window_t id, bool enabled)
{
    edit(id).preferences.skip_pager = enabled;
    classification(id);
}
void State::urgency(xcb_window_t id, UrgencySource source, bool enabled)
{
    auto& c = edit(id);
    if (enabled ? c.urgency.add(source) : c.urgency.remove(source))
    {
        effects_.urgency.insert(id);
        changed(id);
    }
}
void State::clear_urgency(xcb_window_t id)
{
    if (edit(id).urgency.clear())
    {
        effects_.urgency.insert(id);
        changed(id);
    }
}
void State::scratchpad(xcb_window_t id, std::optional<ScratchpadMembership> membership)
{
    auto& c = edit(id);
    if (auto const* named = scratchpad_named(c))
        for (auto& slot : named_scratchpads_)
            if (slot.name == named->name && slot.window() == id)
                slot.mark_empty();
    c.scratchpad = std::move(membership);
    if (auto const* named = scratchpad_named(c))
    {
        std::erase(scratchpad_pool_, id);
        for (auto& slot : named_scratchpads_)
            if (slot.name == named->name)
                slot.mark_claimed(id);
    }
    else if (c.scratchpad)
    {
        if (std::ranges::find(scratchpad_pool_, id) == scratchpad_pool_.end())
            scratchpad_pool_.push_back(id);
    }
    else
        std::erase(scratchpad_pool_, id);
    effects_.state_changed = true;
}
void State::configure_scratchpads(std::span<std::string const> names)
{
    assert(!publishing_);
    std::vector<NamedScratchpadState> slots;
    for (auto const& name : names)
    {
        auto it = std::ranges::find(named_scratchpads_, name, &NamedScratchpadState::name);
        slots.push_back(it == named_scratchpads_.end() ? NamedScratchpadState{ name } : *it);
    }
    named_scratchpads_ = std::move(slots);
    for (auto& [id, c] : clients_)
        if (auto const* named = scratchpad_named(c); named && std::ranges::find(names, named->name) == names.end())
        {
            scratchpad(id, std::nullopt);
            iconic(id, false);
        }
}
void State::scratchpad_pending(std::string_view name, bool pending)
{
    assert(!publishing_);
    for (auto& slot : named_scratchpads_)
        if (slot.name == name && slot.window() == XCB_NONE)
        {
            if (pending)
                slot.mark_launch_pending();
            else
                slot.mark_empty();
        }
}
void State::restore_pool(std::span<xcb_window_t const> ids)
{
    assert(!publishing_);
    scratchpad_pool_.clear();
    for (auto id : ids)
        if (auto const* c = find(id);
            c && !scratchpad_named(*c) && (c->kind() == Client::Kind::Tiled || c->kind() == Client::Kind::Floating))
        {
            if (!c->scratchpad)
                scratchpad(id, VisibleScratchpadPoolMembership{ });
            else if (std::ranges::find(scratchpad_pool_, id) == scratchpad_pool_.end())
                scratchpad_pool_.push_back(id);
        }
}
void State::pin_desktop(xcb_window_t id, bool pinned) { edit(id).desktop_pinned = pinned; }
void State::configure_suppression(xcb_window_t id, bool enabled) { edit(id).suppress_next_configure_request = enabled; }
void State::title(xcb_window_t id, std::string value)
{
    edit(id).name = std::move(value);
    effects_.state_changed = true;
}
void State::window_class(xcb_window_t id, std::string instance, std::string name)
{
    auto& c = edit(id);
    c.wm_class_name = std::move(instance);
    c.wm_class = std::move(name);
    effects_.state_changed = true;
}
void State::window_type(xcb_window_t id, WindowType type)
{
    edit(id).ewmh_type = type;
    classification(id, true);
}
void State::transient(xcb_window_t id, xcb_window_t parent)
{
    auto& client = edit(id);
    client.transient_for = parent;
    classification(id, true);
    // Transients of the fullscreen owner are exempt from suppression.
    invalidate(client.monitor);
}
void State::focus_hints(xcb_window_t id, bool input, bool take_focus)
{
    auto& c = edit(id);
    c.accepts_input = input;
    c.supports_take_focus = take_focus;
    effects_.repair_focus = true;
}
void State::user_time(xcb_window_t id, uint32_t time, xcb_window_t window)
{
    auto& c = edit(id);
    c.user_time = time;
    c.user_time_window = window;
}
void State::fullscreen_monitors(xcb_window_t id, std::optional<FullscreenMonitors> value)
{
    auto& c = edit(id);
    c.fullscreen_monitors = value;
    if (c.fullscreen)
        request_geometry(id);
}
void State::remember_focus(size_t monitor, size_t workspace, xcb_window_t id)
{
    assert(!publishing_);
    workspace_policy::set_workspace_focus(monitors_[monitor].workspaces[workspace], id);
}
bool State::switch_workspace(size_t monitor, size_t workspace)
{
    assert(!publishing_);
    if (monitor >= monitors_.size())
        return false;
    auto& m = monitors_[monitor];
    auto result = workspace_policy::validate_workspace_switch(m, workspace);
    if (!result)
        return false;
    m.previous_workspace = m.current_workspace;
    m.current_workspace = workspace;
    auto [it, inserted] = effects_.workspace_events.try_emplace(monitor, result->old_workspace, workspace);
    it->second.second = workspace;
    invalidate(monitor);
    effects_.current_desktop = true;
    for (auto const& [id, c] : clients_)
        if (c.urgency.active() && id != active_window_)
            effects_.urgency.insert(id);
    return true;
}
void State::layout(size_t monitor, LayoutStrategy strategy)
{
    assert(!publishing_);
    monitors_[monitor].current().layout_strategy = strategy;
    effects_.layouts.insert(monitor);
}
void State::ratio(size_t monitor, SplitAddress address, double value)
{
    assert(!publishing_);
    monitors_[monitor].current().split_ratios[address] = value;
    effects_.layouts.insert(monitor);
}
void State::erase_ratio(size_t monitor, SplitAddress address)
{
    assert(!publishing_);
    monitors_[monitor].current().split_ratios.erase(address);
    effects_.layouts.insert(monitor);
}
void State::reset_ratios(size_t monitor)
{
    assert(!publishing_);
    monitors_[monitor].current().split_ratios.clear();
    effects_.layouts.insert(monitor);
}
void State::swap_tiles(size_t monitor, size_t a, size_t b)
{
    assert(!publishing_);
    auto& w = monitors_[monitor].current().windows;
    std::swap(w[a], w[b]);
    effects_.layouts.insert(monitor);
}
void State::resolve_owner(size_t monitor, xcb_window_t owner)
{
    assert(!publishing_);
    monitors_[monitor].fullscreen_owner = owner;
}
void State::workarea(size_t monitor, Strut strut)
{
    assert(!publishing_);
    if (monitors_[monitor].strut == strut)
        return;
    monitors_[monitor].strut = strut;
    invalidate(monitor);
}
void State::replace_monitors(std::vector<Monitor> monitors)
{
    assert(!publishing_);
    auto previous = std::move(monitors_);
    displaced_.clear();
    for (auto const& [id, c] : clients_)
        if (c.monitor < previous.size()
            && !std::ranges::any_of(monitors, [&](auto const& m) { return m.name == previous[c.monitor].name; }))
            displaced_.insert(id);
    auto destinations = hotplug_policy::preserve_workspaces(previous, monitors);
    focused_monitor_ = focused_monitor_ < destinations.size() ? destinations[focused_monitor_] : 0;
    monitors_ = std::move(monitors);
    for (auto& [id, c] : clients_)
    {
        c.monitor = c.monitor < destinations.size() ? destinations[c.monitor] : 0;
        c.workspace = std::min(c.workspace, monitors_[c.monitor].workspaces.size() - 1);
        c.fullscreen_monitors.reset();
        effects_.desktops.insert(id);
    }
    for (size_t i = 0; i < monitors_.size(); ++i) invalidate(i);
}
void State::fit_floating()
{
    assert(!publishing_);
    for (auto& [id, c] : clients_)
        if (c.kind() == Client::Kind::Floating)
        {
            bool survives = !displaced_.contains(id);
            auto g = floating_geometry(c);
            auto area = monitors_[c.monitor].working_area();
            geometry(
                id,
                survives ? floating::clamp_to_area(area, g)
                         : floating::place_floating(area, g.width, g.height, std::nullopt)
            );
        }
}
void State::restore_workspaces(size_t monitor, size_t current, size_t previous)
{
    assert(!publishing_);
    monitors_[monitor].current_workspace = current;
    monitors_[monitor].previous_workspace = previous;
    invalidate(monitor);
}
void State::restore_layout(size_t monitor, size_t workspace, LayoutStrategy strategy, SplitRatioMap ratios)
{
    assert(!publishing_);
    auto& ws = monitors_[monitor].workspaces[workspace];
    ws.layout_strategy = strategy;
    ws.split_ratios = std::move(ratios);
    effects_.layouts.insert(monitor);
}
void State::restore_tile_order(std::span<xcb_window_t const> order)
{
    assert(!publishing_);
    std::unordered_map<xcb_window_t, size_t> rank;
    for (size_t i = 0; i < order.size(); ++i) rank[order[i]] = i;
    for (auto& m : monitors_)
        for (auto& ws : m.workspaces)
            std::ranges::stable_sort(
                ws.windows,
                [&](auto a, auto b)
                { return (rank.contains(a) ? rank[a] : SIZE_MAX) < (rank.contains(b) ? rank[b] : SIZE_MAX); }
            );
}
void State::restore_client(xcb_window_t id, restart::ClientRecord const& record)
{
    auto& c = edit(id);
    if (c.kind() == Client::Kind::Tiled)
        detach(c);
    c.borderless = record.borderless;
    c.desktop_pinned = record.desktop_pinned;
    if (record.kind == Client::Kind::Tiled)
        c.state = TiledState{ record.prior_floating };
    else if (record.kind == Client::Kind::Floating)
        c.state = FloatingState{ record.floating };
    if (record.hidden_pool_kind == 1)
        c.scratchpad = HiddenTiledScratchpadPoolMembership{ record.prior_floating };
    else if (record.hidden_pool_kind == 2)
        c.scratchpad = HiddenFloatingScratchpadPoolMembership{ record.floating };
    if (record.urgency)
        c.urgency.sources = *record.urgency;
    if (record.preferences)
        c.preferences = *record.preferences;
    else if (record.kind)
        c.preferences.floating = *record.kind == Client::Kind::Floating;
    if (c.kind() == Client::Kind::Tiled)
        attach(c);
    classification(id);
    invalidate(c.monitor);
    effects_.states.insert(id);
    effects_.urgency.insert(id);
    changed(id);
}
void State::touch(xcb_window_t id)
{
    edit(id).mru_order = next_mru_order_++;
    effects_.state_changed = true;
}
}
