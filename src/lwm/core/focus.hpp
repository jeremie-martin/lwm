#pragma once

#include "lwm/core/types.hpp"
#include <optional>
#include <span>
#include <unordered_map>

namespace lwm::focus {

using Clients = std::unordered_map<xcb_window_t, Client>;

// One selection uses one resolved fullscreen owner, including pending transitions.
struct Context
{
    size_t monitor;
    size_t workspace;
    xcb_window_t fullscreen_owner;
    bool showing_desktop;
};

bool accepts_focus(Client const& client);
bool eligible(Client const& client, Context const& context);
xcb_window_t fallback(Clients const& clients, Monitor const& monitor, Context const& context);
std::vector<xcb_window_t> recent_order(Clients const& clients);
xcb_window_t cycle_target(
    std::span<xcb_window_t const> order,
    Clients const& clients,
    Context const& context,
    xcb_window_t current,
    bool forward
);

enum class PointerTransition
{
    None,
    MonitorChangedClearFocus,
};

struct PointerFocusResult
{
    PointerTransition transition = PointerTransition::None;
    size_t new_monitor = 0;

    bool monitor_changed() const { return transition != PointerTransition::None; }
    bool clears_focus() const { return transition == PointerTransition::MonitorChangedClearFocus; }
};

std::optional<size_t> monitor_index_at_point(std::span<Monitor const> monitors, int32_t x, int32_t y);

PointerFocusResult pointer_move(std::span<Monitor const> monitors, size_t active_monitor, int16_t x, int16_t y);

} // namespace lwm::focus
