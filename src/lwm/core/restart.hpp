#pragma once

#include "types.hpp"
#include <optional>
#include <span>
#include <string>
#include <vector>

// Exec handoff snapshot. The domain values define the current schema;
// incompatible formats are rejected rather than migrated.
namespace lwm::restart {

inline constexpr uint32_t format = 12; // Bump when the snapshot schema changes.

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
    std::vector<MonitorState> monitors;
    std::vector<ClientIntent> clients; ///< Registration order
    std::vector<xcb_window_t> registration_order; ///< Clients and fixtures, oldest registration first
    std::vector<NamedScratchpadRecord> named_scratchpads;
    std::vector<xcb_window_t> pool;
    std::vector<xcb_window_t> fullscreen_claims; ///< Oldest to newest, including hidden and iconic clients

    ClientIntent const* find(xcb_window_t window) const;
    bool operator==(Snapshot const&) const = default;
};

std::vector<uint32_t> encode(Snapshot const& snapshot);
// Rejects any other format and any record that is incomplete or out of range.
std::optional<Snapshot> decode(std::span<uint32_t const> words);

} // namespace lwm::restart
