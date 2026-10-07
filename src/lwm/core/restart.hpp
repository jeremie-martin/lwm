#pragma once

#include "types.hpp"
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Exec handoff snapshot. The domain values define the current schema;
// incompatible formats are rejected rather than migrated.
namespace lwm::restart {

inline constexpr uint32_t format = 17; // Bump when the snapshot schema changes.

struct Snapshot
{
    uint32_t format = restart::format;
    size_t focused_monitor = 0;
    xcb_window_t active = XCB_NONE;
    std::vector<Monitor> monitors;
    std::vector<ClientIntent> clients; ///< Registration order
    std::vector<FixtureIntent> fixtures; ///< Established roles and registration ranks
    std::vector<NamedScratchpad> named_scratchpads;
    std::vector<xcb_window_t> pool;

    ClientIntent const* find(xcb_window_t window) const;
    FixtureIntent const* find_fixture(xcb_window_t window) const;
    bool operator==(Snapshot const&) const = default;
};

// JSON text; X11 output names are byte strings and are preserved even if not UTF-8.
std::string encode(Snapshot const& snapshot);
// Rejects any other format and any record that is incomplete or out of range.
std::optional<Snapshot> decode(std::string_view text);

} // namespace lwm::restart
