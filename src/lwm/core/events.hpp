#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <type_traits>
#include <utility>
#include <rfl/Flatten.hpp>
#include <rfl/Rename.hpp>

namespace lwm {

// Event records are the wire payloads. Type names are their subscription names.
// Placement is immutable: a window has both coordinates or neither.
struct Placement
{
    std::optional<size_t> const monitor, workspace;
    Placement() : monitor(std::nullopt), workspace(std::nullopt) { }
    Placement(size_t monitor, size_t workspace) : monitor(monitor), workspace(workspace) { }
};

namespace event {
struct workspace_switch
{
    size_t monitor, from, to;
};
struct focus_change
{
    uint32_t window;
    rfl::Rename<"class", std::string> wm_class;
    std::string title;
};
struct window_map
{
    uint32_t window;
    rfl::Rename<"class", std::string> wm_class;
    std::string_view kind;
    rfl::Flatten<Placement> placement;
};
struct window_unmap
{
    uint32_t window;
    std::string_view kind;
    rfl::Flatten<Placement> placement;
};
using LayoutValue = std::variant<std::string, double>;
struct layout_change
{
    std::string_view action;
    std::optional<LayoutValue> value;
    std::optional<double> delta;
};
struct key_action
{
    std::string_view action;
};
struct config_reload
{
    bool success;
    std::string_view source;
    std::optional<std::string> error;
};
struct state_change { };
} // namespace event

using Event = std::variant<
    event::workspace_switch,
    event::focus_change,
    event::window_map,
    event::window_unmap,
    event::layout_change,
    event::key_action,
    event::config_reload,
    event::state_change>;

// Filters are derived from the same variant as delivery; their bits are internal.
static_assert(std::variant_size_v<Event> <= 32);
inline constexpr uint32_t all_events = 0xFFFFFFFF;
template <typename T> inline constexpr uint32_t event_mask = []<size_t... I>(std::index_sequence<I...>)
{
    static_assert((std::is_same_v<T, std::variant_alternative_t<I, Event>> || ...));
    return ((std::is_same_v<T, std::variant_alternative_t<I, Event>> ? uint32_t{ 1 } << I : 0) | ...);
}(std::make_index_sequence<std::variant_size_v<Event>>{});
std::span<std::string_view const> event_names();
uint32_t parse_event_filter(std::string_view filter);
std::string event_json(Event const& event, std::string_view instance, uint64_t sequence);

} // namespace lwm
