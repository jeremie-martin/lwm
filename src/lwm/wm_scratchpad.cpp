/**
 * @file wm_scratchpad.cpp
 * @brief Scratchpad operations for WindowManager
 *
 * Implements named scratchpads (pre-configured, always floating, auto-launch)
 * and a generic scratchpad pool (ad-hoc stash/recall with original kind preserved).
 * Both are global: windows follow the user across workspaces and monitors.
 */

#include "lwm/core/floating.hpp"
#include "lwm/core/log.hpp"
#include "lwm/core/window_rules.hpp"
#include "wm.hpp"

namespace lwm {

// ---------------------------------------------------------------------------
// Initialization and config reload
// ---------------------------------------------------------------------------

std::optional<Geometry> WindowManager::detach_tiled_to_floating(Client& client)
{
    std::optional<Geometry> prior_floating = prior_floating_geometry(client);
    Geometry geometry = client.tiled_geometry;
    remove_tiled_from_workspace(client, client.monitor, client.workspace);
    set_floating_state(client, geometry);
    return prior_floating;
}

void WindowManager::init_scratchpad_state()
{
    named_scratchpads_.clear();
    for (auto const& sp : config_.scratchpads)
    {
        named_scratchpads_.push_back({ sp.name });
    }
    rebuild_scratchpad_matchers();
}

void WindowManager::rebuild_scratchpad_matchers()
{
    scratchpad_matchers_.clear();
    for (auto const& sp : config_.scratchpads)
    {
        CompiledScratchpadMatcher matcher;
        matcher.name = sp.name;
        matcher.class_regex = WindowRules::compile_pattern(sp.class_pattern);
        matcher.instance_regex = WindowRules::compile_pattern(sp.instance_pattern);
        matcher.title_regex = WindowRules::compile_pattern(sp.title_pattern);
        scratchpad_matchers_.push_back(std::move(matcher));
    }
}

ScratchpadConfig const* WindowManager::find_scratchpad_config(std::string_view name) const
{
    for (auto const& sp : config_.scratchpads)
    {
        if (sp.name == name)
            return &sp;
    }
    return nullptr;
}

WindowManager::NamedScratchpadState* WindowManager::find_named_scratchpad(std::string_view name)
{
    for (auto& sp : named_scratchpads_)
    {
        if (sp.name == name)
            return &sp;
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// Named scratchpad toggle
// ---------------------------------------------------------------------------

void WindowManager::toggle_named_scratchpad(std::string_view name)
{
    auto* state = find_named_scratchpad(name);
    if (!state)
    {
        LOG_WARN("Unknown scratchpad: {}", name);
        return;
    }

    auto const* config = find_scratchpad_config(name);
    if (!config)
        return;

    // No window claimed — auto-launch
    xcb_window_t claimed = state->window();
    if (claimed == XCB_NONE)
    {
        if (state->pending_launch())
            return; // already waiting
        if (config->spawn.empty())
        {
            LOG_WARN("Scratchpad '{}' has no command configured", name);
            return;
        }
        LOG_INFO("Scratchpad '{}': launching '{}'", name, config->spawn.describe());
        launch_program(config->spawn);
        state->mark_launch_pending();
        return;
    }

    auto* client = get_client(claimed);
    if (!client)
    {
        state->mark_empty();
        return;
    }

    if (!client->iconic && !client->hidden && is_physically_visible(*client))
    {
        if (active_window_ == claimed)
            hide_scratchpad_window(claimed);
        else
            focus_any_window(claimed);
        return;
    }

    show_named_scratchpad_window(claimed, *config);
}

// ---------------------------------------------------------------------------
// Generic pool: stash and cycle
// ---------------------------------------------------------------------------

void WindowManager::stash_to_scratchpad(xcb_window_t window)
{
    auto* client = get_client(window);
    if (!client)
        return;
    if (client->scratchpad.has_value())
        return;
    if (client->fullscreen || client->iconic)
        return;
    if (client->kind != Client::Kind::Tiled && client->kind != Client::Kind::Floating)
        return;
    if (drag_active())
        return;

    LOG_INFO("Stashing window {:#x} to scratchpad pool", window);

    if (client->kind == Client::Kind::Tiled)
    {
        auto prior_floating = detach_tiled_to_floating(*client);
        client->scratchpad = HiddenTiledScratchpadPoolMembership { prior_floating };
    }
    else
    {
        client->scratchpad = HiddenFloatingScratchpadPoolMembership { floating_geometry(*client) };
    }

    client->iconic = true;
    set_iconic_state(window, true);

    std::erase(scratchpad_pool_, window);
    scratchpad_pool_.push_back(window);

    finalize_visibility_on_monitor(client->monitor);

    if (active_window_ == window && focused_monitor_ < monitors_.size())
        focus_or_fallback(monitors_[focused_monitor_]);

    flush_and_drain_crossing();
}

xcb_window_t WindowManager::find_visible_pool_window() const
{
    // scratchpad_pool_ entries are guaranteed managed: release_scratchpad_window runs
    // before unmanage erases from clients_.
    for (auto it = scratchpad_pool_.rbegin(); it != scratchpad_pool_.rend(); ++it)
    {
        auto const& client = require_client(*it);
        if (client.scratchpad
            && std::holds_alternative<VisibleScratchpadPoolMembership>(*client.scratchpad)
            && !client.iconic && !client.hidden
            && is_physically_visible(client))
        {
            return *it;
        }
    }
    return XCB_NONE;
}

void WindowManager::cycle_scratchpad_pool()
{
    if (scratchpad_pool_.empty())
        return;

    xcb_window_t visible = find_visible_pool_window();

    if (visible != XCB_NONE)
    {
        if (active_window_ == visible)
        {
            hide_scratchpad_window(visible);

            for (auto it = scratchpad_pool_.rbegin(); it != scratchpad_pool_.rend(); ++it)
            {
                if (*it == visible)
                    continue;
                auto const& client = require_client(*it);
                if (is_hidden_pool_scratchpad(client) && client.iconic)
                {
                    show_pool_scratchpad_window(*it);
                    return;
                }
            }
        }
        else
        {
            focus_any_window(visible);
        }
        return;
    }

    for (auto it = scratchpad_pool_.rbegin(); it != scratchpad_pool_.rend(); ++it)
    {
        auto const& client = require_client(*it);
        if (is_hidden_pool_scratchpad(client) && client.iconic)
        {
            show_pool_scratchpad_window(*it);
            return;
        }
    }
}

// ---------------------------------------------------------------------------
// Show / hide helpers
// ---------------------------------------------------------------------------

void WindowManager::hide_scratchpad_window(xcb_window_t window)
{
    auto* client = get_client(window);
    if (!client)
        return;

    LOG_DEBUG("Hiding scratchpad window {:#x}", window);

    if (scratchpad_named(*client))
    {
        // Named scratchpads are always restored using their configured floating placement.
    }
    else if (client->kind == Client::Kind::Tiled)
    {
        // Hidden tiled scratchpads are kept as Floating to satisfy the invariant
        // that Tiled clients live in their workspace's tiled list.
        auto prior_floating = detach_tiled_to_floating(*client);
        client->scratchpad = HiddenTiledScratchpadPoolMembership { prior_floating };
    }
    else if (client->kind == Client::Kind::Floating)
    {
        client->scratchpad = HiddenFloatingScratchpadPoolMembership { floating_geometry(*client) };
    }

    client->iconic = true;
    set_iconic_state(window, true);

    finalize_visibility_on_monitor(client->monitor);

    if (active_window_ == window && focused_monitor_ < monitors_.size())
        focus_or_fallback(monitors_[focused_monitor_]);

    flush_and_drain_crossing();
}

void WindowManager::show_named_scratchpad_window(xcb_window_t window, ScratchpadConfig const& config)
{
    auto* client = get_client(window);
    if (!client)
        return;

    LOG_DEBUG("Showing named scratchpad '{}' window {:#x}", config.name, window);

    size_t old_monitor = client->monitor;
    size_t target_monitor = focused_monitor_;
    size_t target_workspace = monitors_[target_monitor].current_workspace;

    bool was_tiled = client->kind == Client::Kind::Tiled;
    if (was_tiled)
    {
        detach_tiled_to_floating(*client);
        client->mru_order = next_mru_order_++;
    }

    Geometry wa = monitors_[target_monitor].working_area();
    uint16_t w = static_cast<uint16_t>(static_cast<double>(wa.width) * config.width);
    uint16_t h = static_cast<uint16_t>(static_cast<double>(wa.height) * config.height);
    int16_t x = static_cast<int16_t>(wa.x + (wa.width - w) / 2);
    int16_t y = static_cast<int16_t>(wa.y + (wa.height - h) / 2);
    floating_geometry(*client) = { x, y, w, h };

    assign_window_workspace(*client, target_monitor, target_workspace);

    client->iconic = false;
    set_iconic_state(window, false);

    apply_floating_geometry(*client);
    finalize_visibility_on_monitor(target_monitor);

    if (old_monitor != target_monitor)
        finalize_visibility_on_monitor(old_monitor);

    focus_any_window(window);
    flush_and_drain_crossing();
}

void WindowManager::show_pool_scratchpad_window(xcb_window_t window)
{
    auto* client = get_client(window);
    if (!client)
        return;

    LOG_DEBUG("Showing pool scratchpad window {:#x}", window);

    size_t old_monitor = client->monitor;
    size_t target_monitor = focused_monitor_;
    size_t target_workspace = monitors_[target_monitor].current_workspace;

    auto const* hidden_tiled = hidden_tiled_pool_scratchpad(*client);
    bool restore_tiled = hidden_tiled != nullptr;
    std::optional<Geometry> restore_prior_floating;
    if (hidden_tiled)
        restore_prior_floating = hidden_tiled->prior_floating;
    std::optional<Geometry> restore_geometry;
    if (auto const* hidden_floating = hidden_floating_pool_scratchpad(*client))
    {
        restore_geometry = hidden_floating->restore_geometry;
    }

    assign_window_workspace(*client, target_monitor, target_workspace);

    client->iconic = false;
    client->scratchpad = VisibleScratchpadPoolMembership {};
    set_iconic_state(window, false);

    if (restore_tiled)
    {
        set_tiled_state(*client, restore_prior_floating);
        add_tiled_to_workspace(*client, target_monitor, target_workspace);
    }
    else
    {
        if (client->kind == Client::Kind::Tiled)
        {
            set_floating_state(*client, restore_geometry ? *restore_geometry : client->tiled_geometry);
            client->mru_order = next_mru_order_++;
        }
        if (restore_geometry.has_value())
        {
            Geometry restored_geometry = *restore_geometry;
            if (old_monitor != target_monitor)
            {
                Geometry target_area = monitors_[target_monitor].working_area();
                restored_geometry = floating::translate_to_area(
                    restored_geometry,
                    monitors_[old_monitor].working_area(),
                    target_area
                );
            }
            floating_geometry(*client) = restored_geometry;
        }
    }

    if (client->kind != Client::Kind::Tiled)
        apply_floating_geometry(*client);
    finalize_visibility_on_monitor(target_monitor);

    focus_any_window(window);
    flush_and_drain_crossing();
}

// ---------------------------------------------------------------------------
// Scratchpad window claiming (called during map)
// ---------------------------------------------------------------------------

void WindowManager::finalize_scratchpad_claim(
    xcb_window_t window, NamedScratchpadState& state, std::string_view name)
{
    bool was_pending = state.pending_launch();
    state.mark_claimed(window);

    auto* client = get_client(window);
    if (!client)
        return;
    client->scratchpad = NamedScratchpadMembership { std::string(name) };

    if (was_pending)
    {
        auto const* config = find_scratchpad_config(name);
        if (config)
            show_named_scratchpad_window(window, *config);
    }
    else
    {
        hide_scratchpad_window(window);
    }
}

std::optional<std::string> WindowManager::match_scratchpad_for_window(
    xcb_window_t window, WindowRuleResult const& rule_result)
{
    if (rule_result.scratchpad.has_value())
    {
        auto* state = find_named_scratchpad(*rule_result.scratchpad);
        if (state && state->window() == XCB_NONE)
            return rule_result.scratchpad;
    }

    auto [wm_instance, wm_class] = get_wm_class(window);
    std::string title = get_window_name(window);

    for (auto const& matcher : scratchpad_matchers_)
    {
        auto* state = find_named_scratchpad(matcher.name);
        if (!state || state->window() != XCB_NONE)
            continue;

        if (matcher.class_regex.has_value() && !std::regex_match(wm_class, *matcher.class_regex))
            continue;
        if (matcher.instance_regex.has_value() && !std::regex_match(wm_instance, *matcher.instance_regex))
            continue;
        if (matcher.title_regex.has_value() && !std::regex_match(title, *matcher.title_regex))
            continue;
        if (!matcher.class_regex.has_value() && !matcher.instance_regex.has_value() && !matcher.title_regex.has_value())
            continue;

        return matcher.name;
    }

    return std::nullopt;
}

// ---------------------------------------------------------------------------
// Cleanup on window removal
// ---------------------------------------------------------------------------

void WindowManager::release_scratchpad_window(xcb_window_t window)
{
    auto const* client = get_client(window);
    if (!client)
        return;

    if (auto const* named = scratchpad_named(*client))
    {
        auto* state = find_named_scratchpad(named->name);
        if (state && state->window() == window)
        {
            LOG_INFO("Named scratchpad '{}' window {:#x} removed", state->name, window);
            state->mark_empty();
        }
    }

    std::erase(scratchpad_pool_, window);
}

} // namespace lwm
