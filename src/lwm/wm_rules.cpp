#include "lwm/core/floating.hpp"
#include "lwm/core/policy.hpp"
#include "wm.hpp"
#include <algorithm>

namespace lwm {

void WindowManager::apply_rule_target_location(xcb_window_t window, WindowRuleResult const& rule_result)
{
    if (!rule_result.target_monitor.has_value() && !rule_result.target_workspace.has_value())
        return;

    auto* movable = get_client(window);
    if (!movable)
        return;

    size_t target_monitor = rule_result.target_monitor.value_or(movable->monitor);
    size_t target_workspace = rule_result.target_workspace.value_or(movable->workspace);
    if (target_monitor >= monitors_.size())
        return;

    target_workspace = std::min(target_workspace, monitors_[target_monitor].workspaces.size() - 1);
    if (movable->monitor == target_monitor && movable->workspace == target_workspace)
        return;
    if (!state_.relocate(movable->id, target_monitor, target_workspace, RelocationGeometry::CenterOnMonitorChange))
        return;
    if (movable->kind() == Client::Kind::Tiled && window == active_window_)
        state_.remember_focus(target_monitor, target_workspace, window);
}

void WindowManager::apply_rule_floating_placement(xcb_window_t window, WindowRuleResult const& rule_result)
{
    auto* client = get_client(window);
    if (!client || client->kind() != Client::Kind::Floating)
        return;

    auto geom = rule_result.geometry.value_or(floating_geometry(*client));

    if (rule_result.center)
    {
        Geometry area = monitors_[client->monitor].working_area();
        geom.x = area.x + static_cast<int16_t>((area.width - geom.width) / 2);
        geom.y = area.y + static_cast<int16_t>((area.height - geom.height) / 2);
    }

    state_.geometry(window, geom);
    state_.configure_suppression(window, rule_result.geometry.has_value() || rule_result.center);
}

void WindowManager::apply_rule_result_to_window(xcb_window_t window, WindowRuleResult const& rule_result)
{
    auto* client = get_client(window);
    if (!client)
        return;
    if (rule_result.floating.has_value())
        state_.floating(window, *rule_result.floating);

    apply_rule_target_location(window, rule_result);
    apply_rule_floating_placement(window, rule_result);

    if (rule_result.skip_taskbar)
        state_.skip_taskbar(client->id, *rule_result.skip_taskbar);
    if (rule_result.skip_pager)
        state_.skip_pager(client->id, *rule_result.skip_pager);
    if (rule_result.sticky)
        state_.sticky(client->id, *rule_result.sticky);
    if (rule_result.layer_hint)
        state_.layer(client->id, *rule_result.layer_hint);
    if (rule_result.borderless)
        state_.borderless(client->id, *rule_result.borderless);
    if (rule_result.fullscreen)
        set_fullscreen(*client, *rule_result.fullscreen);
}

void WindowManager::reapply_rules_to_existing_windows()
{
    std::vector<std::pair<uint64_t, xcb_window_t>> ordered;
    ordered.reserve(clients_.size());

    for (auto const& [window, client] : clients_)
    {
        if (client.kind() == Client::Kind::Tiled || client.kind() == Client::Kind::Floating)
            ordered.push_back({ client.order, window });
    }

    std::sort(ordered.begin(), ordered.end(), [](auto const& a, auto const& b) { return a.first < b.first; });

    for (auto const& [order, window] : ordered)
    {
        (void)order;

        auto const* client = get_client(window);
        if (!client)
            continue;

        auto match_info = window_match_info(*client);

        auto rule_result = match_window_rules(config_.rules, match_info, monitors_, config_.workspaces.names);
        if (rule_result.matched)
        {
            apply_rule_result_to_window(window, rule_result);
            request_geometry(require_client(window));
        }
    }
}

ClassificationResult WindowManager::classify_managed_window(xcb_window_t window, bool refresh_transient)
{
    auto const* cached = get_client(window);
    xcb_window_t transient =
        cached && !refresh_transient ? cached->transient_for : transient_for_window(window).value_or(XCB_NONE);
    bool has_transient = transient != XCB_NONE;

    // One set of property values drives classification, rules, and initial
    // registration. Runtime callers refresh the relevant cached property first.
    WindowMatchInfo match_info;
    if (auto const* existing = get_client(window))
    {
        match_info = window_match_info(*existing);
    }
    else
    {
        auto [instance, name] = get_wm_class(window);
        match_info.wm_class = std::move(name);
        match_info.wm_class_name = std::move(instance);
        match_info.title = get_window_name(window);
        match_info.ewmh_type = ewmh_.get_window_type_enum(window);
    }
    match_info.is_transient = has_transient;
    auto classification = classify_window_type(match_info.ewmh_type, has_transient);
    auto rule_result = match_window_rules(config_.rules, match_info, monitors_, config_.workspaces.names);

    if (rule_result.matched && classification.kind != WindowClassification::Kind::Dock
        && classification.kind != WindowClassification::Kind::Desktop
        && classification.kind != WindowClassification::Kind::Popup)
    {
        if (rule_result.floating.has_value())
        {
            classification.kind =
                *rule_result.floating ? WindowClassification::Kind::Floating : WindowClassification::Kind::Tiled;
        }
    }

    return { classification, std::move(rule_result), transient, std::move(match_info) };
}

void WindowManager::relocate_to_transient_parent(xcb_window_t window, xcb_window_t previous_transient_for)
{
    auto* client = get_client(window);
    if (!client || previous_transient_for == client->transient_for || client->transient_for == XCB_NONE)
        return;

    auto* parent = get_client(client->transient_for);
    if (!parent)
        return;
    if (parent->monitor >= monitors_.size() || parent->workspace >= monitors_[parent->monitor].workspaces.size())
        return;

    if (!state_.relocate(client->id, parent->monitor, parent->workspace))
        return;
    if (client->kind() == Client::Kind::Floating)
    {
        Geometry geometry = floating_geometry(*client);
        auto parent_geometry = placement_parent_geometry(client->transient_for);
        state_.geometry(
            window,
            floating::place_floating(
                monitors_[parent->monitor].working_area(),
                std::max<uint16_t>(1, geometry.width),
                std::max<uint16_t>(1, geometry.height),
                parent_geometry
            )
        );
    }
}

bool WindowManager::claim_pending_scratchpad(
    xcb_window_t window,
    WindowMatchInfo const& properties,
    WindowRuleResult const& rules
)
{
    auto const& client = require_client(window);
    if (client.scratchpad)
        return false;
    auto name = match_scratchpad_for_window(properties, rules);
    if (!name)
        return false;
    auto* state = find_named_scratchpad(*name);
    if (!state || state->window() != XCB_NONE || !state->pending_launch())
        return false;
    finalize_scratchpad_claim(window, *state, *name);
    return true;
}

void WindowManager::reevaluate_metadata(xcb_window_t window, WindowRuleResult const& previous)
{
    auto const& client = require_client(window);
    if (client.kind() != Client::Kind::Tiled && client.kind() != Client::Kind::Floating)
        return;
    auto properties = window_match_info(client);
    auto current = match_window_rules(config_.rules, properties, monitors_, config_.workspaces.names);
    if (claim_pending_scratchpad(window, properties, current))
        return;
    // Metadata alone must not reapply placement or undo a user's state changes.
    if (current.matched && current != previous)
        apply_rule_result_to_window(window, current);
}

} // namespace lwm
