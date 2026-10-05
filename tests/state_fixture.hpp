#pragma once

// Builds domain states through State's own operations, so unit tests start
// from states the WM can actually reach.

#include "lwm/core/state.hpp"
#include <algorithm>
#include <stdexcept>
#include <string>

namespace lwm::test {

inline Monitor monitor(std::string name, int16_t x = 0, size_t workspaces = 3)
{
    Monitor monitor;
    monitor.name = std::move(name);
    monitor.geometry.x = x;
    monitor.geometry.width = 1000;
    monitor.geometry.height = 800;
    monitor.workspaces.assign(workspaces, Workspace{ });
    return monitor;
}

// Installs a configuration edited from the current one through State's own reload path.
template <typename Edit> void configure(State& state, Edit edit)
{
    auto config = state.config();
    edit(config);
    if (!state.configure(std::move(config)))
        throw std::logic_error("test configuration rejected");
}

inline Topology::Output output(std::string name, int16_t x = 0)
{
    return { std::move(name), { x, 0, 1000, 800 } };
}

// The screen spans every output from the root origin, as RandR reports it.
inline void outputs(State& state, std::vector<Topology::Output> outputs)
{
    Geometry screen;
    for (auto const& output : outputs)
    {
        screen.width = std::max<uint16_t>(screen.width, geometry_extent(output.geometry.x + output.geometry.width));
        screen.height = std::max<uint16_t>(screen.height, geometry_extent(output.geometry.y + output.geometry.height));
    }
    state.replace_topology({ std::move(outputs), screen });
}

inline State state(size_t monitors = 1, size_t workspaces = 3)
{
    State state;
    configure(state, [&](Config& config) { config.workspaces.count = workspaces; });
    std::vector<Topology::Output> discovered;
    for (size_t i = 0; i < monitors; ++i) discovered.push_back(output("M" + std::to_string(i), static_cast<int16_t>(i * 1000)));
    outputs(state, std::move(discovered));
    return state;
}

inline void focus(State& state, xcb_window_t id)
{
    state.focus(id);
    state.settle();
}

// Application observations for restart tests carry no private WM intent.
inline WindowObservation observe(Client const& client, std::optional<uint32_t> desktop = std::nullopt)
{
    WindowObservation window{
        .id = client.id,
        .name = client.name,
        .wm_class = client.wm_class,
        .wm_class_name = client.wm_class_name,
        .type = client.ewmh_type,
        .transient_for = client.transient_for,
        .desktop = desktop,
        .accepts_input = client.accepts_input,
        .urgent = client.urgency.has(UrgencySource::App),
        .supports_take_focus = client.supports_take_focus,
        .user_time = client.user_time,
        .user_time_window = client.user_time_window,
        .fullscreen_monitors = client.fullscreen_monitors,
        .size_hints = client.size_hints,
    };
    window.states.set(WindowState::Fullscreen, client.fullscreen);
    window.states.set(WindowState::Hidden, client.iconic);
    window.states.set(WindowState::Sticky, client.sticky);
    window.states.set(WindowState::MaximizedHorz, client.maximized_horz);
    window.states.set(WindowState::MaximizedVert, client.maximized_vert);
    window.states.set(WindowState::Modal, client.modal);
    return window;
}

struct ClientSpec
{
    size_t monitor = 0;
    size_t workspace = 0;
    bool floating = false;
    Geometry geometry{ 10, 10, 200, 100 };
};

inline Client const& add(State& state, xcb_window_t id, ClientSpec spec = { })
{
    Client client;
    client.id = id;
    client.monitor = spec.monitor;
    client.workspace = spec.workspace;
    if (spec.floating)
        client.mode = FloatingMode{ spec.geometry };
    else
        client.mode = TiledMode{ };
    state.insert(std::move(client));
    return state.require(id);
}

inline Client const& add_floating(State& state, xcb_window_t id, size_t monitor = 0, size_t workspace = 0)
{
    return add(state, id, { .monitor = monitor, .workspace = workspace, .floating = true });
}

} // namespace lwm::test
