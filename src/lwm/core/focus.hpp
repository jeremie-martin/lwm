#pragma once

#include "lwm/core/state.hpp"
#include <optional>
#include <span>

namespace lwm::focus {

// Automatic focus on one monitor: remembered tiled focus, bounded focus
// history, reverse tiled order, sticky tiles, then floating clients by recency.
xcb_window_t fallback(State const& state, size_t monitor);

// All clients by descending recency. Eligibility is checked per step, so the
// order can outlive changes in visibility or input hints.
std::vector<xcb_window_t> recent_order(State const& state);
xcb_window_t
cycle_target(std::span<xcb_window_t const> order, State const& state, size_t monitor, xcb_window_t current, bool forward);

std::optional<size_t> monitor_index_at_point(std::span<Monitor const> monitors, int32_t x, int32_t y);

} // namespace lwm::focus
