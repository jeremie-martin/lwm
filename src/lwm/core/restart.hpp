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

inline constexpr uint32_t format = 6;

struct ClientRecord
{
    xcb_window_t window = XCB_NONE;
    size_t monitor = 0;
    size_t workspace = 0;
    Client::Kind kind = Client::Kind::Tiled;
    Geometry geometry;                ///< Floating normal rectangle, or the tile's layout target
    std::optional<Geometry> floating; ///< A tile's remembered floating rectangle
    ClientPreferences preferences;
    uint8_t urgency = 0;
    bool borderless = false;
    bool desktop_pinned = false;
    std::optional<TileSlot> tile_slot; ///< Floating client's return position on its original output

    bool operator==(ClientRecord const&) const = default;
};

struct WorkspaceRecord
{
    LayoutStrategy strategy = LayoutStrategy::MasterStack;
    SplitRatioMap ratios;
    std::vector<xcb_window_t> tiles;
    xcb_window_t focused = XCB_NONE;

    bool operator==(WorkspaceRecord const&) const = default;
};

struct MonitorRecord
{
    size_t current = 0;
    size_t previous = 0;
    std::vector<WorkspaceRecord> workspaces;

    bool operator==(MonitorRecord const&) const = default;
};

struct NamedScratchpadRecord
{
    std::string name;
    xcb_window_t window = XCB_NONE;

    bool operator==(NamedScratchpadRecord const&) const = default;
};

struct Snapshot
{
    size_t focused_monitor = 0;
    xcb_window_t active = XCB_NONE;
    bool showing_desktop = false;
    std::vector<MonitorRecord> monitors;
    std::vector<ClientRecord> clients; ///< Oldest to newest focus recency
    std::vector<NamedScratchpadRecord> named_scratchpads;
    std::vector<xcb_window_t> pool;
    std::vector<xcb_window_t> fullscreen_claims; ///< Oldest to newest, including hidden and iconic clients

    ClientRecord const* find(xcb_window_t window) const;
    bool operator==(Snapshot const&) const = default;
};

std::vector<uint32_t> encode(Snapshot const& snapshot);
// Rejects any other format and any record that is incomplete or out of range.
std::optional<Snapshot> decode(std::span<uint32_t const> words);

} // namespace lwm::restart
