#include "restart.hpp"
#include "invariants.hpp"
#include <rfl/AddTagsToVariants.hpp>
#include <rfl/NoExtraFields.hpp>
#include <rfl/NoOptionals.hpp>
#include <rfl/json/read.hpp>
#include <rfl/json/write.hpp>
#include <utility>

namespace lwm::restart {
namespace {

using Wire = rfl::Processors<rfl::NoExtraFields, rfl::NoOptionals, rfl::AddTagsToVariants>;

} // namespace

ClientIntent const* Snapshot::find(xcb_window_t window) const
{
    auto record = std::ranges::find(clients, window, &ClientIntent::id);
    return record == clients.end() ? nullptr : &*record;
}

FixtureIntent const* Snapshot::find_fixture(xcb_window_t window) const
{
    auto fixture = std::ranges::find(fixtures, window, &FixtureIntent::id);
    return fixture == fixtures.end() ? nullptr : &*fixture;
}

std::string encode(Snapshot const& snapshot)
{
    return rfl::json::write<Wire>(snapshot, YYJSON_WRITE_ALLOW_INVALID_UNICODE);
}

std::optional<Snapshot> decode(std::string_view text)
{
    // The predecessor wrote this from the same schema; format mismatches are rejected.
    auto snapshot = rfl::json::read<Snapshot, rfl::NoExtraFields, rfl::NoOptionals, rfl::AddTagsToVariants>(
        text, YYJSON_READ_ALLOW_INVALID_UNICODE
    );
    // Observation, released scratchpads and final fullscreen ordering can each
    // rank the X11 window-ID space. Bound incoming ranks before any mutation.
    constexpr uint64_t rank_limit = UINT64_MAX - 3 * uint64_t{ UINT32_MAX };
    if (!snapshot || snapshot->format != format || invariants::validate(*snapshot, rank_limit).has_value())
        return std::nullopt;
    return std::move(*snapshot);
}

} // namespace lwm::restart
