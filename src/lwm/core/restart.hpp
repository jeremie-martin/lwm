#pragma once

#include "types.hpp"
#include <optional>
#include <span>
#include <string>
#include <vector>

// Exec handoff snapshot. The encoding is private to one LWM version: a
// different format word means the snapshot is ignored and windows are adopted
// as on a fresh start.
namespace lwm::restart {

inline constexpr uint32_t format = 11; // Bump when the snapshot schema changes.

struct ClientRecord
{
    xcb_window_t window = XCB_NONE;
    size_t monitor = 0;
    size_t workspace = 0;
    ClientMode mode = TiledMode{ };
    ClientPreferences preferences;
    uint8_t urgency = 0;
    bool borderless = false;
    bool desktop_pinned = false;
    std::optional<FullscreenMonitors> fullscreen_monitors;

    uint64_t mru_order = 0; ///< Zero means never focused

    bool operator==(ClientRecord const&) const = default;
};

struct WorkspaceRecord
{
    LayoutStrategy strategy = LayoutStrategy::MasterStack;
    SplitRatioMap ratios;
    std::vector<xcb_window_t> tiles;
    xcb_window_t preferred_tile = XCB_NONE;

    bool operator==(WorkspaceRecord const&) const = default;
};

struct MonitorRecord
{
    std::string name;
    Geometry geometry;
    size_t current = 0;
    size_t previous = 0;
    std::vector<WorkspaceRecord> workspaces;

    bool operator==(MonitorRecord const&) const = default;
};

struct NamedScratchpadRecord
{
    std::string name;
    std::optional<xcb_window_t> window; ///< Absent means a launch is pending; present means claimed

    bool operator==(NamedScratchpadRecord const&) const = default;
};

struct Snapshot
{
    size_t focused_monitor = 0;
    xcb_window_t active = XCB_NONE;
    bool showing_desktop = false;
    std::vector<MonitorRecord> monitors;
    std::vector<ClientRecord> clients; ///< Registration order
    std::vector<xcb_window_t> registration_order; ///< Clients and fixtures, oldest registration first
    std::vector<NamedScratchpadRecord> named_scratchpads;
    std::vector<xcb_window_t> pool;
    std::vector<xcb_window_t> fullscreen_claims; ///< Oldest to newest, including hidden and iconic clients

    ClientRecord const* find(xcb_window_t window) const;
    // Resolve the saved graph onto discovered outputs before any client adoption.
    // Uses the same workspace transfer and floating fit policy as live hotplug.
    void rebind(std::span<Monitor const> discovered);
    bool operator==(Snapshot const&) const = default;
};

MonitorRecord capture_monitor(Monitor const& monitor);

std::vector<uint32_t> encode(Snapshot const& snapshot);
// Rejects any other format and any record that is incomplete or out of range.
std::optional<Snapshot> decode(std::span<uint32_t const> words);

} // namespace lwm::restart
