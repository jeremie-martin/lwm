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

void WindowManager::init_scratchpad_state()
{
    named_scratchpads_.clear();
    for (auto const& sp : config_.scratchpads)
    {
        named_scratchpads_.push_back({ sp.name, XCB_NONE, false });
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
    if (state->window == XCB_NONE)
    {
        if (state->pending_launch)
            return; // already waiting
        if (config->command.empty())
        {
            LOG_WARN("Scratchpad '{}' has no command configured", name);
            return;
        }
        LOG_INFO("Scratchpad '{}': launching '{}'", name, config->command);
        launch_program(keybinds_.resolve_command(config->command, config_));
        state->pending_launch = true;
        return;
    }

    auto* client = get_client(state->window);
    if (!client)
    {
        state->window = XCB_NONE;
        return;
    }

    if (!client->iconic && !client->hidden && is_physically_visible(state->window))
    {
        if (active_window_ == state->window)
            hide_scratchpad_window(state->window);
        else
            focus_any_window(state->window);
        return;
    }

    show_named_scratchpad_window(state->window, *config);
}

// ---------------------------------------------------------------------------
// Generic pool: stash and cycle
// ---------------------------------------------------------------------------

void WindowManager::stash_to_scratchpad(xcb_window_t window)
{
    auto* client = get_client(window);
    if (!client)
        return;
    if (client->in_scratchpad || client->scratchpad_name.has_value())
        return;
    if (client->fullscreen || client->iconic)
        return;
    if (client->kind != Client::Kind::Tiled && client->kind != Client::Kind::Floating)
        return;
    if (drag_state_.active)
        return;

    LOG_INFO("Stashing window {:#x} to scratchpad pool", window);

    client->scratchpad_restore_kind = client->kind;
    if (client->kind == Client::Kind::Floating)
        client->scratchpad_restore_geometry = client->floating_geometry;

    if (client->kind == Client::Kind::Tiled)
    {
        remove_tiled_from_workspace(window, client->monitor, client->workspace);
        client->kind = Client::Kind::Floating;
    }

    client->in_scratchpad = true;
    client->iconic = true;
    set_iconic_state(window, true);

    std::erase(scratchpad_pool_, window);
    scratchpad_pool_.push_back(window);

    if (client->monitor < monitors_.size())
    {
        sync_visibility_for_monitor(client->monitor);
        rearrange_monitor(monitors_[client->monitor]);
    }

    if (active_window_ == window && focused_monitor_ < monitors_.size())
        focus_or_fallback(monitors_[focused_monitor_]);

    flush_and_drain_crossing();
}

xcb_window_t WindowManager::find_visible_pool_window() const
{
    for (auto it = scratchpad_pool_.rbegin(); it != scratchpad_pool_.rend(); ++it)
    {
        auto const* client = get_client(*it);
        if (client && !client->iconic && !client->hidden && !client->scratchpad_name.has_value())
        {
            if (is_physically_visible(*it))
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
                if (*it != visible)
                {
                    auto* client = get_client(*it);
                    if (client && client->in_scratchpad && client->iconic)
                    {
                        show_pool_scratchpad_window(*it);
                        return;
                    }
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
        auto* client = get_client(*it);
        if (client && client->in_scratchpad && client->iconic)
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

    if (client->kind == Client::Kind::Tiled && !client->scratchpad_name.has_value())
    {
        client->scratchpad_restore_kind = Client::Kind::Tiled;
        remove_tiled_from_workspace(window, client->monitor, client->workspace);
        client->kind = Client::Kind::Floating; // Satisfy invariants while hidden
    }
    else if (client->kind == Client::Kind::Floating && !client->scratchpad_name.has_value())
    {
        client->scratchpad_restore_kind = Client::Kind::Floating;
        client->scratchpad_restore_geometry = client->floating_geometry;
    }

    client->in_scratchpad = true;
    client->iconic = true;
    set_iconic_state(window, true);

    if (client->monitor < monitors_.size())
    {
        sync_visibility_for_monitor(client->monitor);
        rearrange_monitor(monitors_[client->monitor]);
    }

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
        remove_tiled_from_workspace(window, client->monitor, client->workspace);
        client->kind = Client::Kind::Floating;
        client->mru_order = next_mru_order_++;
    }

    Geometry wa = monitors_[target_monitor].working_area();
    uint16_t w = static_cast<uint16_t>(static_cast<double>(wa.width) * config.width);
    uint16_t h = static_cast<uint16_t>(static_cast<double>(wa.height) * config.height);
    int16_t x = static_cast<int16_t>(wa.x + (wa.width - w) / 2);
    int16_t y = static_cast<int16_t>(wa.y + (wa.height - h) / 2);
    client->floating_geometry = { x, y, w, h };

    assign_window_workspace(window, target_monitor, target_workspace);

    client->iconic = false;
    client->in_scratchpad = false;
    set_iconic_state(window, false);

    sync_visibility_for_monitor(target_monitor);
    apply_floating_geometry(window);
    if (was_tiled)
        rearrange_monitor(monitors_[target_monitor]);
    restack_monitor_layers(target_monitor);

    if (old_monitor != target_monitor && old_monitor < monitors_.size())
    {
        sync_visibility_for_monitor(old_monitor);
        rearrange_monitor(monitors_[old_monitor]);
    }

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

    Client::Kind restore_kind = client->scratchpad_restore_kind.value_or(Client::Kind::Floating);

    assign_window_workspace(window, target_monitor, target_workspace);

    client->iconic = false;
    client->in_scratchpad = false;
    set_iconic_state(window, false);

    if (restore_kind == Client::Kind::Tiled)
    {
        client->kind = Client::Kind::Tiled;
        add_tiled_to_workspace(window, target_monitor, target_workspace);
    }
    else
    {
        if (client->kind == Client::Kind::Tiled)
        {
            client->kind = Client::Kind::Floating;
            client->mru_order = next_mru_order_++;
        }
        if (client->scratchpad_restore_geometry.has_value())
        {
            Geometry restored_geometry = *client->scratchpad_restore_geometry;
            if (old_monitor != target_monitor)
            {
                Geometry target_area = monitors_[target_monitor].working_area();
                if (old_monitor < monitors_.size())
                {
                    restored_geometry = floating::translate_to_area(
                        restored_geometry,
                        monitors_[old_monitor].working_area(),
                        target_area
                    );
                }
                else
                {
                    restored_geometry = floating::clamp_to_area(target_area, restored_geometry);
                }
            }
            client->floating_geometry = restored_geometry;
        }
    }

    client->scratchpad_restore_kind.reset();
    client->scratchpad_restore_geometry.reset();

    sync_visibility_for_monitor(target_monitor);
    if (client->kind == Client::Kind::Tiled)
        rearrange_monitor(monitors_[target_monitor]);
    else
    {
        apply_floating_geometry(window);
        restack_monitor_layers(target_monitor);
    }

    focus_any_window(window);
    flush_and_drain_crossing();
}

// ---------------------------------------------------------------------------
// Scratchpad window claiming (called during map)
// ---------------------------------------------------------------------------

void WindowManager::finalize_scratchpad_claim(
    xcb_window_t window, NamedScratchpadState& state, std::string_view name)
{
    state.window = window;
    bool was_pending = state.pending_launch;
    state.pending_launch = false;

    auto* client = get_client(window);
    if (!client)
        return;
    client->in_scratchpad = true;

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
        if (state && state->window == XCB_NONE)
            return rule_result.scratchpad;
    }

    auto [wm_instance, wm_class] = get_wm_class(window);
    std::string title = get_window_name(window);

    for (auto const& matcher : scratchpad_matchers_)
    {
        auto* state = find_named_scratchpad(matcher.name);
        if (!state || state->window != XCB_NONE)
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

    if (client->scratchpad_name.has_value())
    {
        auto* state = find_named_scratchpad(*client->scratchpad_name);
        if (state && state->window == window)
        {
            LOG_INFO("Named scratchpad '{}' window {:#x} removed", state->name, window);
            state->window = XCB_NONE;
        }
    }

    std::erase(scratchpad_pool_, window);
}

} // namespace lwm
