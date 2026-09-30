// Named scratchpads launch as floating windows; generic pool entries retain
// their kind. Recall rehosts either on the focused monitor/current workspace.

#include "lwm/core/floating.hpp"
#include "lwm/core/log.hpp"
#include "lwm/core/window_rules.hpp"
#include "wm.hpp"

namespace lwm {

// ---------------------------------------------------------------------------
// Initialization and config reload
// ---------------------------------------------------------------------------

std::optional<Geometry> WindowManager::detach_tiled_to_floating(Client const& client)
{
    std::optional<Geometry> prior_floating = prior_floating_geometry(client);
    Geometry geometry = client.tiled_geometry;
    state_.change_kind(client.id, FloatingState{ geometry });
    return prior_floating;
}

void WindowManager::init_scratchpad_state()
{
    std::vector<std::string> names;
    for (auto const& sp : config_.scratchpads) names.push_back(sp.name);
    state_.configure_scratchpads(names);
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

WindowManager::NamedScratchpadState const* WindowManager::find_named_scratchpad(std::string_view name)
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
        LWM_LOG_WARN("Unknown scratchpad: {}", name);
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
            LWM_LOG_WARN("Scratchpad '{}' has no command configured", name);
            return;
        }
        LWM_LOG_DEBUG("Scratchpad '{}': launching", name);
        if (launch_program(config->spawn))
            state_.scratchpad_pending(name);
        return;
    }

    auto* client = get_client(claimed);
    if (!client)
    {
        return;
    }

    if (!client->iconic && is_visible(*client))
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
    if (client->kind() != Client::Kind::Tiled && client->kind() != Client::Kind::Floating)
        return;
    if (drag_active())
        return;

    LWM_LOG_DEBUG("Stashing window {:#x} to scratchpad pool", window);

    if (client->kind() == Client::Kind::Tiled)
    {
        auto prior_floating = detach_tiled_to_floating(*client);
        state_.scratchpad(window, HiddenTiledScratchpadPoolMembership{ prior_floating });
    }
    else
    {
        state_.scratchpad(window, HiddenFloatingScratchpadPoolMembership{ floating_geometry(*client) });
    }


    iconify_window(window);
}

xcb_window_t WindowManager::find_visible_pool_window() const
{
    // scratchpad_pool_ entries are guaranteed managed: release_scratchpad_window runs
    // before unmanage erases from clients_.
    for (auto it = scratchpad_pool_.rbegin(); it != scratchpad_pool_.rend(); ++it)
    {
        auto const& client = require_client(*it);
        if (client.scratchpad && std::holds_alternative<VisibleScratchpadPoolMembership>(*client.scratchpad)
            && !client.iconic && is_visible(client))
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

    LWM_LOG_DEBUG("Hiding scratchpad window {:#x}", window);

    if (scratchpad_named(*client))
    {
        // Named scratchpads are always restored using their configured floating placement.
    }
    else if (client->kind() == Client::Kind::Tiled)
    {
        // Hidden tiled scratchpads are kept as Floating to satisfy the invariant
        // that Tiled clients live in their workspace's tiled list.
        auto prior_floating = detach_tiled_to_floating(*client);
        state_.scratchpad(window, HiddenTiledScratchpadPoolMembership{ prior_floating });
    }
    else if (client->kind() == Client::Kind::Floating)
    {
        state_.scratchpad(window, HiddenFloatingScratchpadPoolMembership{ floating_geometry(*client) });
    }

    iconify_window(window);
}

void WindowManager::show_named_scratchpad_window(xcb_window_t window, ScratchpadConfig const& config)
{
    auto* client = get_client(window);
    if (!client)
        return;

    LWM_LOG_DEBUG("Showing named scratchpad '{}' window {:#x}", config.name, window);

    size_t target_monitor = focused_monitor_;
    size_t target_workspace = monitors_[target_monitor].current_workspace;

    bool was_tiled = client->kind() == Client::Kind::Tiled;
    if (was_tiled)
    {
        detach_tiled_to_floating(*client);
        state_.touch(client->id);
    }

    Geometry wa = monitors_[target_monitor].working_area();
    uint16_t w = static_cast<uint16_t>(static_cast<double>(wa.width) * config.width);
    uint16_t h = static_cast<uint16_t>(static_cast<double>(wa.height) * config.height);
    int16_t x = static_cast<int16_t>(wa.x + (wa.width - w) / 2);
    int16_t y = static_cast<int16_t>(wa.y + (wa.height - h) / 2);
    state_.geometry(window, { x, y, w, h });

    state_.relocate(client->id, target_monitor, target_workspace);

    // A late title/class match may already be visible on the target workspace.
    request_geometry(*client);
    deiconify_window(window, true);
}

void WindowManager::show_pool_scratchpad_window(xcb_window_t window)
{
    auto* client = get_client(window);
    if (!client)
        return;

    LWM_LOG_DEBUG("Showing pool scratchpad window {:#x}", window);

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

    state_.scratchpad(window, VisibleScratchpadPoolMembership{ });

    if (restore_tiled)
    {
        state_.relocate(client->id, target_monitor, target_workspace);
        state_.change_kind(client->id, TiledState{ restore_prior_floating });
    }
    else
    {
        if (client->kind() == Client::Kind::Tiled)
        {
            state_.change_kind(client->id, FloatingState{ restore_geometry.value_or(client->tiled_geometry) });
            state_.touch(client->id);
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
            state_.geometry(window, restored_geometry);
        }
        state_.relocate(client->id, target_monitor, target_workspace);
    }

    deiconify_window(window, true);
}

// ---------------------------------------------------------------------------
// Scratchpad window claiming (called during map)
// ---------------------------------------------------------------------------

void WindowManager::finalize_scratchpad_claim(
    xcb_window_t window,
    NamedScratchpadState const& state,
    std::string_view name
)
{
    bool was_pending = state.pending_launch();

    auto* client = get_client(window);
    if (!client)
        return;
    state_.scratchpad(window, NamedScratchpadMembership{ std::string(name) });

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

std::optional<std::string>
WindowManager::match_scratchpad_for_window(WindowMatchInfo const& properties, WindowRuleResult const& rule_result)
{
    if (rule_result.scratchpad.has_value())
    {
        auto* state = find_named_scratchpad(*rule_result.scratchpad);
        if (state && state->window() == XCB_NONE)
            return rule_result.scratchpad;
    }

    for (auto const& matcher : config_.scratchpads)
    {
        auto* state = find_named_scratchpad(matcher.name);
        if (!state || state->window() != XCB_NONE)
            continue;

        if (matcher.match.empty()
            || !matcher.match.matches(properties.wm_class, properties.wm_class_name, properties.title))
            continue;

        return matcher.name;
    }

    return std::nullopt;
}

} // namespace lwm
