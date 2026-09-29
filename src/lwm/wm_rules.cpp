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
    if (movable->kind() == Client::Kind::Floating)
    {
        move_floating_client_to_workspace(*movable, target_monitor, target_workspace, true);
        return;
    }

    if (movable->monitor == target_monitor && movable->workspace == target_workspace)
        return;

    if (!move_tiled_client_to_workspace(*movable, target_monitor, target_workspace))
        return;
    if (window == active_window_)
        workspace_policy::set_workspace_focus(monitors_[target_monitor].workspaces[target_workspace], window);
}

void WindowManager::apply_rule_floating_placement(xcb_window_t window, WindowRuleResult const& rule_result)
{
    auto* client = get_client(window);
    if (!client || client->kind() != Client::Kind::Floating)
        return;

    if (rule_result.geometry.has_value())
        floating_geometry(*client) = *rule_result.geometry;

    if (rule_result.center)
    {
        Geometry area = monitors_[client->monitor].working_area();
        auto& geom = floating_geometry(*client);
        geom.x = area.x + static_cast<int16_t>((area.width - geom.width) / 2);
        geom.y = area.y + static_cast<int16_t>((area.height - geom.height) / 2);
    }

    client->suppress_next_configure_request = rule_result.geometry.has_value() || rule_result.center;
}

void WindowManager::apply_rule_result_to_window(
    xcb_window_t window,
    WindowRuleResult const& rule_result,
    WindowClassification const* classification
)
{
    auto* client = get_client(window);
    if (!client)
        return;
    if (rule_result.fullscreen == false)
        set_fullscreen(*client, false);

    if (rule_result.floating.has_value())
    {
        if (*rule_result.floating)
            convert_window_to_floating(window);
        else
            convert_window_to_tiled(window);
    }

    apply_rule_target_location(window, rule_result);
    apply_rule_floating_placement(window, rule_result);

    apply_classification_state(window, classification, rule_result, client->transient_for != XCB_NONE);
    if (rule_result.fullscreen == true)
        set_fullscreen(*client, true);
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

        auto rule_result = window_rules_.match(match_info, monitors_, config_.workspaces.names);
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
    auto rule_result = window_rules_.match(match_info, monitors_, config_.workspaces.names);

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

bool WindowManager::sync_kind(xcb_window_t window, WindowClassification::Kind desired_kind)
{
    auto* client = get_client(window);
    if (!client)
        return false;

    if (desired_kind == WindowClassification::Kind::Floating && client->kind() == Client::Kind::Tiled)
        convert_window_to_floating(window);
    else if (desired_kind == WindowClassification::Kind::Tiled && client->kind() == Client::Kind::Floating)
        convert_window_to_tiled(window);

    return get_client(window) != nullptr;
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

    if (client->kind() == Client::Kind::Tiled)
    {
        if (!move_tiled_client_to_workspace(*client, parent->monitor, parent->workspace))
            return;
    }
    else
    {
        if (!move_floating_client_to_workspace(*client, parent->monitor, parent->workspace, false))
            return;

        Geometry geometry = current_window_geometry(window);
        Geometry parent_geometry = current_window_geometry(client->transient_for);
        floating_geometry(*client) = floating::place_floating(
            monitors_[parent->monitor].working_area(),
            std::max<uint16_t>(1, geometry.width),
            std::max<uint16_t>(1, geometry.height),
            parent_geometry
        );
    }
}

void WindowManager::apply_classification_state(
    xcb_window_t window,
    WindowClassification const* classification,
    WindowRuleResult const& rule_result,
    bool has_transient
)
{
    auto* client = get_client(window);
    if (!client)
        return;

    // Reload patches current state; mapping and property changes recompute defaults.
    auto desired = classification_policy::compute_desired_state({
        .classification_skip_taskbar = classification ? classification->skip_taskbar : client->skip_taskbar,
        .classification_skip_pager = classification ? classification->skip_pager : client->skip_pager,
        .classification_above = classification ? classification->above : client->layer_hint == LayerHint::Above,
        .app_skip_taskbar = classification && client->app_prefs.skip_taskbar,
        .app_skip_pager = classification && client->app_prefs.skip_pager,
        .ewmh_sticky = client->sticky,
        .ewmh_modal = client->modal,
        .app_above = classification && !client->fullscreen && client->app_prefs.above,
        .app_below =
            classification ? !client->fullscreen && client->app_prefs.below : client->layer_hint == LayerHint::Below,
        .rule_skip_taskbar = rule_result.skip_taskbar,
        .rule_skip_pager = rule_result.skip_pager,
        .rule_sticky = rule_result.sticky,
        .rule_layer_hint = rule_result.layer_hint,
        .rule_borderless = rule_result.borderless.value_or(classification ? false : client->borderless),
        .has_transient = classification && has_transient,
        .is_sticky_desktop = client->sticky,
    });

    if (client->skip_taskbar != desired.skip_taskbar)
        set_client_skip_taskbar(*client, desired.skip_taskbar);
    if (client->skip_pager != desired.skip_pager)
        set_client_skip_pager(*client, desired.skip_pager);
    if (client->sticky != desired.sticky)
        set_window_sticky(*client, desired.sticky);
    if (client->modal != desired.modal)
        set_window_modal(*client, desired.modal);
    if (client->layer_hint != desired.layer_hint)
        set_window_layer_hint(*client, desired.layer_hint);
    if (client->borderless != desired.borderless)
        set_window_borderless(*client, desired.borderless);

    update_allowed_actions(*client);
}

void WindowManager::sync_managed_window_classification(xcb_window_t window, ClassificationResult const& result)
{
    auto& client = require_client(window);
    auto previous_monitor = client.monitor;
    auto previous_transient = client.transient_for;
    client.transient_for = result.transient_for;
    auto kind = result.classification.kind;
    if (kind != WindowClassification::Kind::Tiled && kind != WindowClassification::Kind::Floating)
    {
        stacking_dirty_ |= previous_transient != client.transient_for;
        return;
    }
    sync_kind(window, kind);
    relocate_to_transient_parent(window, previous_transient);
    apply_rule_result_to_window(window, result.rule_result, &result.classification);
    invalidate_monitor(previous_monitor);
    invalidate_monitor(client.monitor);
}

void WindowManager::reevaluate_managed_window(xcb_window_t window, bool refresh_transient)
{
    auto* client = get_client(window);
    if (!client)
        return;

    if (client->kind() != Client::Kind::Tiled && client->kind() != Client::Kind::Floating)
        return;

    auto result = classify_managed_window(window, refresh_transient);

    if (claim_pending_scratchpad(window, result.properties, result.rule_result))
        return;

    sync_managed_window_classification(window, result);
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
    auto current = window_rules_.match(properties, monitors_, config_.workspaces.names);
    if (claim_pending_scratchpad(window, properties, current))
        return;
    // Metadata alone must not reapply placement or undo a user's state changes.
    if (current != previous)
        reevaluate_managed_window(window);
}

} // namespace lwm
