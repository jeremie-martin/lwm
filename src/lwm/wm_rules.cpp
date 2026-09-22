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
    if (movable->kind == Client::Kind::Floating)
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
    if (!client || client->kind != Client::Kind::Floating)
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
        if (client.kind == Client::Kind::Tiled || client.kind == Client::Kind::Floating)
            ordered.push_back({ client.order, window });
    }

    std::sort(ordered.begin(), ordered.end(), [](auto const& a, auto const& b) { return a.first < b.first; });

    for (auto const& [order, window] : ordered)
    {
        (void)order;

        auto const* client = get_client(window);
        if (!client)
            continue;

        WindowMatchInfo match_info{
            .wm_class = client->wm_class,
            .wm_class_name = client->wm_class_name,
            .title = client->name,
            .ewmh_type = client->ewmh_type,
            .is_transient = client->transient_for != XCB_NONE,
        };

        auto rule_result = window_rules_.match(match_info, monitors_, config_.workspaces.names);
        if (rule_result.matched)
        {
            apply_rule_result_to_window(window, rule_result);
            apply_visible_floating_geometry(require_client(window));
        }
    }
}

ClassificationResult WindowManager::classify_managed_window(xcb_window_t window)
{
    bool has_transient = transient_for_window(window).has_value();
    auto classification = ewmh_.classify_window(window, has_transient);

    // Use cached client data when available to avoid X round-trips.
    // On initial manage the client doesn't exist yet, so we fall back to X reads.
    auto const* existing = get_client(window);
    std::string instance_name, class_name, title;
    WindowType ewmh_type;
    if (existing)
    {
        class_name = existing->wm_class;
        instance_name = existing->wm_class_name;
        title = existing->name;
        ewmh_type = existing->ewmh_type;
    }
    else
    {
        auto wm_class = get_wm_class(window);
        instance_name = wm_class.first;
        class_name = wm_class.second;
        title = get_window_name(window);
        ewmh_type = ewmh_.get_window_type_enum(window);
    }
    WindowMatchInfo match_info{ .wm_class = class_name,
                                .wm_class_name = instance_name,
                                .title = title,
                                .ewmh_type = ewmh_type,
                                .is_transient = has_transient };
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

    return { classification, rule_result };
}

bool WindowManager::sync_kind(xcb_window_t window, WindowClassification::Kind desired_kind)
{
    auto* client = get_client(window);
    if (!client)
        return false;

    if (desired_kind == WindowClassification::Kind::Floating && client->kind == Client::Kind::Tiled)
        convert_window_to_floating(window);
    else if (desired_kind == WindowClassification::Kind::Tiled && client->kind == Client::Kind::Floating)
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

    if (client->kind == Client::Kind::Tiled)
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

    auto state_flags = ewmh_.get_window_state_flags(window);

    // Reload patches current state; mapping and property changes recompute defaults.
    auto desired = classification_policy::compute_desired_state(
        {
            .classification_skip_taskbar = classification ? classification->skip_taskbar : client->skip_taskbar,
            .classification_skip_pager = classification ? classification->skip_pager : client->skip_pager,
            .classification_above = classification ? classification->above : client->layer_hint == LayerHint::Above,
            .app_skip_taskbar = classification && client->app_prefs.skip_taskbar,
            .app_skip_pager = classification && client->app_prefs.skip_pager,
            .ewmh_sticky = state_flags.sticky,
            .ewmh_modal = state_flags.modal,
            .app_above = classification && !client->fullscreen && client->app_prefs.above,
            .app_below = classification ? !client->fullscreen && client->app_prefs.below
                                        : client->layer_hint == LayerHint::Below,
            .rule_skip_taskbar = rule_result.skip_taskbar,
            .rule_skip_pager = rule_result.skip_pager,
            .rule_sticky = rule_result.sticky,
            .rule_layer_hint = rule_result.layer_hint,
            .rule_borderless = rule_result.borderless.value_or(classification ? false : client->borderless),
            .has_transient = classification && has_transient,
            .is_sticky_desktop = is_sticky_desktop(window),
        }
    );

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
    auto const& classification = result.classification;
    auto const& rule_result = result.rule_result;

    auto* client = get_client(window);
    if (!client)
        return;

    Client::Kind previous_kind = client->kind;
    size_t previous_monitor = client->monitor;
    size_t previous_workspace = client->workspace;
    xcb_window_t previous_transient_for = client->transient_for;

    auto transient = transient_for_window(window);
    client->transient_for = transient.value_or(XCB_NONE);

    // If classification isn't tiled/floating, only transient restacking matters
    WindowClassification::Kind desired_kind = classification.kind;
    if (desired_kind != WindowClassification::Kind::Tiled && desired_kind != WindowClassification::Kind::Floating)
    {
        if (previous_transient_for != client->transient_for)
            stacking_dirty_ = true;
        return;
    }

    // Classification determines kind before transient placement and rule overrides.
    if (!sync_kind(window, desired_kind))
        return;
    relocate_to_transient_parent(window, previous_transient_for);
    apply_rule_result_to_window(window, rule_result, &classification);

    if (previous_transient_for != client->transient_for)
        stacking_dirty_ = true;

    // Reconcile affected monitors before applying geometry and focus.
    size_t current_monitor = client->monitor;
    bool monitor_changed = previous_monitor != current_monitor;
    bool workspace_changed = previous_workspace != client->workspace;
    bool kind_changed = previous_kind != client->kind;

    // Sync visibility on all affected monitors.
    sync_visibility_for_monitor(previous_monitor);
    if (monitor_changed)
        sync_visibility_for_monitor(current_monitor);

    // Rearrange/restack previous monitor if window moved away
    if (monitor_changed || workspace_changed || kind_changed)
    {
        if (previous_kind == Client::Kind::Tiled)
            rearrange_monitor(monitors_[previous_monitor]);
        else if (monitor_changed)
            apply_stacking();
    }

    // Rearrange/apply geometry on current monitor
    if (client->kind == Client::Kind::Tiled)
    {
        if (kind_changed || monitor_changed || workspace_changed)
            rearrange_monitor(monitors_[current_monitor]);
    }
    else
    {
        apply_visible_floating_geometry(*client);
        apply_stacking();
    }

    if (window == active_window_ && (!is_focus_eligible(*client) || !is_physically_visible(*client)))
        repair_focus_after_visibility_change(previous_monitor, false);

    flush_and_drain_crossing();
}

void WindowManager::reevaluate_managed_window(xcb_window_t window)
{
    auto* client = get_client(window);
    if (!client)
        return;

    if (client->kind != Client::Kind::Tiled && client->kind != Client::Kind::Floating)
        return;

    auto result = classify_managed_window(window);

    if (!client->scratchpad.has_value())
    {
        auto scratchpad_match = match_scratchpad_for_window(window, result.rule_result);
        if (scratchpad_match)
        {
            auto* state = find_named_scratchpad(*scratchpad_match);
            if (state && state->window() == XCB_NONE && state->pending_launch())
            {
                finalize_scratchpad_claim(window, *state, *scratchpad_match);
                return;
            }
        }
    }

    sync_managed_window_classification(window, result);
}

} // namespace lwm
