#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

namespace lwm {

enum EventType : uint32_t
{
    Event_WindowMap = 1 << 0,
    Event_WindowUnmap = 1 << 1,
    Event_FocusChange = 1 << 2,
    Event_WorkspaceSwitch = 1 << 3,
    Event_LayoutChange = 1 << 4,
    Event_ConfigReload = 1 << 5,
    Event_KeyAction = 1 << 6,
    Event_StateChange = 1 << 7,
    Event_All = 0xFFFFFFFF,
};

struct NamedEvent
{
    std::string_view name;
    EventType type;
};
inline constexpr NamedEvent event_specs[] = {
    {       "window_map",       Event_WindowMap },
    {     "window_unmap",     Event_WindowUnmap },
    {     "focus_change",     Event_FocusChange },
    { "workspace_switch", Event_WorkspaceSwitch },
    {    "layout_change",    Event_LayoutChange },
    {    "config_reload",    Event_ConfigReload },
    {       "key_action",       Event_KeyAction },
    {     "state_change",     Event_StateChange },
};

/// Parse a comma-separated filter string (e.g. "focus_change,window_map") into bitmask.
/// Returns Event_All if filter is empty; unknown names are ignored.
uint32_t parse_event_filter(std::string_view filter);

/// Escape a string for JSON output (handles quotes, backslashes, control chars).
std::string json_escape(std::string_view input);

struct Placement
{
    size_t monitor;
    size_t workspace;
};

// Subscription events are typed facts; JSON is produced in one place.
namespace event {
struct WorkspaceSwitch
{
    size_t monitor, from, to;
};
struct FocusChange
{
    uint32_t window;
    std::string wm_class, title;
};
struct WindowMap
{
    uint32_t window;
    std::string wm_class;
    std::string_view kind;
    std::optional<Placement> placement; ///< Absent for fixtures and direct-mapped popups
};
struct WindowUnmap
{
    uint32_t window;
    std::string_view kind;
    std::optional<Placement> placement; ///< Absent for fixtures
};
struct LayoutChange
{
    std::string_view action;
    std::optional<std::string> value; ///< Pre-rendered JSON value, when the action has one
    std::optional<double> delta;
};
struct KeyAction
{
    std::string_view action;
};
struct ConfigReload
{
    bool success;
    std::string_view source;
    std::string error;
};
} // namespace event

// Variant order is the documented delivery order within one operation.
using Event = std::variant<
    event::WorkspaceSwitch,
    event::FocusChange,
    event::WindowMap,
    event::WindowUnmap,
    event::LayoutChange,
    event::KeyAction,
    event::ConfigReload>;

EventType event_type(Event const& event);
// Workspace changes, then focus, then map/unmap, then action and reload outcomes.
int event_priority(Event const& event);
std::string event_json(Event const& event);

} // namespace lwm
