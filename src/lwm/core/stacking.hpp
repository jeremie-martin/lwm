#pragma once

#include "lwm/core/state.hpp"
#include <span>

namespace lwm::stacking {

// One bottom-to-top order for every client and fixture, derived from state and
// a shared fullscreen visibility projection.
std::vector<xcb_window_t> compute_order(State const& state, State::FullscreenVisibility const& fullscreen);

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
