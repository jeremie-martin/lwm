#pragma once

#include "lwm/core/types.hpp"
#include <span>
#include <unordered_map>

namespace lwm::stacking {

// Monitor fullscreen owners must already be resolved for the current state.
std::vector<xcb_window_t> compute_order(
    std::unordered_map<xcb_window_t, Client> const& clients,
    std::span<Monitor const> monitors,
    bool showing_desktop,
    xcb_window_t active
);

struct StackMove
{
    xcb_window_t window;
    xcb_window_t sibling;
    uint32_t mode;
};

// Compute the minimum sibling moves needed to reconcile surviving managed clients.
std::vector<StackMove>
plan_moves(std::span<xcb_window_t const> server_order, std::span<xcb_window_t const> desired_order);

} // namespace lwm::stacking
