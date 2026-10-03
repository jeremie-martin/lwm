// Named slots and the generic pool are the only scratchpad membership records.
// Explicit hiding uses iconic state; ordinary visibility remains derived.

#include "state.hpp"
#include "floating.hpp"
#include "log.hpp"
#include <algorithm>

namespace lwm {

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
void State::reconcile_scratchpads()
{
    auto const& configs = config_.scratchpads;
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

void State::stash(xcb_window_t id)
{
    auto const& client = require(id);
    if (scratchpad_claim(id) || pooled(id) || client.fullscreen || client.iconic || drag_)
        return;
    LWM_LOG_DEBUG("Stashing window {:#x} to scratchpad pool", id);
    pool_scratchpad(id);
    iconic(id, true);
}

// Pool order owns selection, independently of workspace visibility or minimization.
// Recall/focus the target first; cycling an active target advances the rotation.
void State::cycle_scratchpad_pool()
{
    if (scratchpad_pool_.empty())
        return;
    auto window = scratchpad_pool_.back();
    auto const& client = require(window);
    if (client.monitor != focused_monitor_ || !visible(client))
        show_pooled_scratchpad(window);
    else if (window != active_window_)
        focus(window);
    else
    {
        iconic(window, true);
        advance_scratchpad_pool();
        if (scratchpad_pool_.size() > 1)
            show_pooled_scratchpad(scratchpad_pool_.back());
    }
}

// Pooled clients keep their mode and floating offset within the workarea.
void State::show_pooled_scratchpad(xcb_window_t id)
{
    LWM_LOG_DEBUG("Showing pool scratchpad window {:#x}", id);
    relocate(id, focused_monitor_, monitors_[focused_monitor_].current_workspace, RelocationGeometry::Translate);
    restore(id, true);
}

void State::scratchpad_pending(std::string_view name, bool pending)
{
    auto it = std::ranges::find(named_scratchpads_, name, &NamedScratchpad::name);
    if (it == named_scratchpads_.end() || it->claimed_window() != XCB_NONE)
        return;
    mutated();
    it->window = pending ? std::nullopt : std::optional<xcb_window_t>{ XCB_NONE };
}


// A rule naming a scratchpad takes precedence over matchers; claimed names cannot match.
ScratchpadConfig const* State::match_scratchpad(Client const& client) const
{
    auto const& configs = config_.scratchpads;
    auto matches = [&](ScratchpadConfig const& config)
    {
        auto const* slot = named_scratchpad(config.name);
        return slot && slot->claimed_window() == XCB_NONE;
    };
    if (client.rule && client.rule->scratchpad)
        for (auto const& config : configs)
            if (config.name == *client.rule->scratchpad && matches(config))
                return &config;
    for (auto const& config : configs)
        if (matches(config) && config.match.matches(client.wm_class, client.wm_class_name, client.name))
            return &config;
    return nullptr;
}

// A returned configuration asks the adapter to launch; pending begins only after spawn succeeds.
std::expected<ScratchpadConfig const*, std::string> State::toggle_scratchpad(std::string_view name)
{
    auto const* slot = named_scratchpad(name);
    if (!slot)
        return std::unexpected("unknown scratchpad: " + std::string(name));
    auto const& config = *std::ranges::find(config_.scratchpads, name, &ScratchpadConfig::name);
    auto window = slot->claimed_window();
    if (window == XCB_NONE)
        return slot->pending_launch() ? nullptr : &config;
    auto const& client = require(window);
    if (client.monitor != focused_monitor_ || !visible(client))
        show_named_scratchpad(window, config);
    else if (window == active_window_)
        iconic(window, true);
    else
        focus(window);
    return nullptr;
}

// Pending launches show their window; an unrequested live match starts hidden.
void State::claim_scratchpad(xcb_window_t id, ScratchpadConfig const& config)
{
    auto const* slot = named_scratchpad(config.name);
    if (!slot || slot->claimed_window() != XCB_NONE || !find(id))
        return;
    bool requested = slot->pending_launch();
    mutated();
    release_scratchpad(id);
    std::ranges::find(named_scratchpads_, config.name, &NamedScratchpad::name)->window = id;
    if (requested)
        show_named_scratchpad(id, config);
    else
        iconic(id, true);
}

bool State::claim_pending_scratchpad(xcb_window_t id)
{
    if (scratchpad_claim(id) || pooled(id))
        return false;
    auto const* config = match_scratchpad(require(id));
    if (!config || !named_scratchpad(config->name)->pending_launch())
        return false;
    claim_scratchpad(id, *config);
    return true;
}

void State::show_named_scratchpad(xcb_window_t window, ScratchpadConfig const& config)
{
    LWM_LOG_DEBUG("Showing named scratchpad '{}' window {:#x}", config.name, window);
    size_t monitor = focused_monitor();
    size_t workspace = monitors()[monitor].current_workspace;
    floating(window, true);
    Geometry area = monitors()[monitor].working_area();
    auto width = geometry_extent(static_cast<int64_t>(area.width * config.width));
    auto height = geometry_extent(static_cast<int64_t>(area.height * config.height));
    geometry(window, floating::place_floating(area, width, height, std::nullopt));
    relocate(window, monitor, workspace);
    restore(window, true);
}

} // namespace lwm
