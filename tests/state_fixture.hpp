#pragma once

// Builds domain states through State's own operations, so unit tests start
// from states the WM can actually reach.

#include "lwm/core/state.hpp"
#include <string>

namespace lwm::test {

inline Monitor monitor(std::string name, int16_t x = 0, size_t workspaces = 3)
{
    Monitor monitor;
    monitor.name = std::move(name);
    monitor.x = x;
    monitor.width = 1000;
    monitor.height = 800;
    monitor.workspaces.assign(workspaces, Workspace{ });
    return monitor;
}

inline State state(size_t monitors = 1, size_t workspaces = 3)
{
    State state;
    std::vector<Monitor> outputs;
    for (size_t i = 0; i < monitors; ++i)
        outputs.push_back(monitor("M" + std::to_string(i), static_cast<int16_t>(i * 1000), workspaces));
    state.replace_monitors(std::move(outputs));
    return state;
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
        client.mode = TiledMode{ std::nullopt, spec.geometry };
    state.insert(std::move(client));
    return state.require(id);
}

inline Client const& add_floating(State& state, xcb_window_t id, size_t monitor = 0, size_t workspace = 0)
{
    return add(state, id, { .monitor = monitor, .workspace = workspace, .floating = true });
}

} // namespace lwm::test
