#include "state.hpp"
#include "floating.hpp"
#include "log.hpp"
#include "window_rules.hpp"

namespace lwm {

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

    if (auto const* floating = floating_mode(require(window)))
    {
        auto rectangle = rule.geometry.value_or(floating->geometry);
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
    if (rule.borderless)
        assign(window, &Client::borderless, *rule.borderless);
    if (rule.fullscreen)
        fullscreen(window, *rule.fullscreen);
}

// Metadata reconciles changed rule actions; losing a match leaves prior actions.
void State::reconcile_metadata(xcb_window_t id, Config const& config)
{
    bool changed = match_rule(id, config.rules);
    if (!claim_pending_scratchpad(id, config.scratchpads) && changed)
        apply_initial_rule(id);
}

bool State::match_rule(xcb_window_t id, std::span<WindowRuleConfig const> rules)
{
    auto const* rule = match_window_rules(rules, require(id));
    return assign(id, &Client::rule, rule ? std::optional{ *rule } : std::nullopt);
}

void State::apply_initial_rule(xcb_window_t id)
{
    if (auto const& rule = require(id).rule)
        apply_rule(id, *rule);
}

// Reload deliberately reapplies unchanged actions and does not claim pending launches.
void State::reapply_rules(std::span<WindowRuleConfig const> rules)
{
    for (auto const* client : clients_by_order())
    {
        match_rule(client->id, rules);
        apply_initial_rule(client->id);
    }
}

void State::title(xcb_window_t id, std::string value, Config const& config)
{
    if (assign(id, &Client::name, std::move(value)))
        reconcile_metadata(id, config);
}

void State::window_class(xcb_window_t id, std::string instance, std::string name, Config const& config)
{
    bool changed = assign(id, &Client::wm_class_name, std::move(instance));
    changed |= assign(id, &Client::wm_class, std::move(name));
    if (changed)
        reconcile_metadata(id, config);
}

void State::window_type(xcb_window_t id, WindowType type, Config const& config)
{
    if (assign(id, &Client::ewmh_type, type))
        apply_default_mode(id);
    reconcile_metadata(id, config);
}

// Derive managed-parent geometry after relocation; only an active preview comes from the adapter.
void State::transient(
    xcb_window_t id, xcb_window_t parent, Config const& config, std::optional<Geometry> parent_preview
)
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
                        parent_preview ? *parent_preview : presentation_geometry(*target)
                    )
                );
    }
    reconcile_metadata(id, config);
}

// A rule naming a scratchpad takes precedence over matchers; claimed names cannot match.
ScratchpadConfig const* State::match_scratchpad(Client const& client, std::span<ScratchpadConfig const> configs) const
{
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

// True asks the adapter to launch; pending begins only after spawn succeeds.
bool State::toggle_scratchpad(ScratchpadConfig const& config)
{
    auto const* slot = named_scratchpad(config.name);
    if (!slot)
        return false;
    auto window = slot->claimed_window();
    if (window == XCB_NONE)
        return !slot->pending_launch();
    auto const& client = require(window);
    if (client.monitor != focused_monitor_ || !visible(client))
        show_named_scratchpad(window, config);
    else if (window == active_window_)
        iconic(window, true);
    else
        focus(window);
    return false;
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

bool State::claim_pending_scratchpad(xcb_window_t id, std::span<ScratchpadConfig const> configs)
{
    if (scratchpad_claim(id) || pooled(id))
        return false;
    auto const* config = match_scratchpad(require(id), configs);
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
