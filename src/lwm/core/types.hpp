#pragma once

#include <algorithm>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>
#include <xcb/xcb.h>

namespace lwm {

enum class WindowType
{
    Desktop,
    Dock,
    Toolbar,
    Menu,
    Utility,
    Splash,
    Dialog,
    DropdownMenu,
    PopupMenu,
    Tooltip,
    Notification,
    Combo,
    Dnd,
    Normal
};

/// X coordinate used when hiding a window off-screen.
constexpr int16_t OFF_SCREEN_X = -20000;

/// _NET_WM_DESKTOP value of a window shown on every desktop.
constexpr uint32_t STICKY_DESKTOP = 0xFFFFFFFF;

/// X timestamps are 32-bit millisecond counters; ordering treats subtraction
/// as a signed delta so it survives wraparound.
inline bool timestamp_is_before(uint32_t timestamp, uint32_t reference)
{
    return timestamp != reference && timestamp - reference >= 0x80000000U;
}

struct Geometry
{
    int16_t x = 0;
    int16_t y = 0;
    uint16_t width = 0;
    uint16_t height = 0;

    bool operator==(Geometry const&) const = default;
};

// Saturate arithmetic at the X11 wire boundary instead of wrapping coordinates
// or turning a large positive size into zero.
constexpr int16_t geometry_coordinate(int64_t value)
{
    return static_cast<int16_t>(std::clamp<int64_t>(value, -32768, 32767));
}
constexpr uint16_t geometry_extent(int64_t value)
{
    return static_cast<uint16_t>(std::clamp<int64_t>(value, 1, 65535));
}

// The model places frames: outer rectangles with the border drawn inside. X
// positions a window by its outer corner and sizes it without the border.
constexpr Geometry outset(Geometry window, int64_t border)
{
    return { window.x, window.y, geometry_extent(window.width + 2 * border), geometry_extent(window.height + 2 * border) };
}
constexpr Geometry inset(Geometry frame, int64_t border) { return outset(frame, -border); }

// Containment keeps computed coordinates wide, so distant points never wrap inside.
constexpr bool contains(Geometry area, int32_t x, int32_t y)
{
    return x >= area.x && x < area.x + area.width && y >= area.y && y < area.y + area.height;
}
constexpr std::pair<int32_t, int32_t> center(Geometry rectangle)
{
    return { rectangle.x + rectangle.width / 2, rectangle.y + rectangle.height / 2 };
}
// A rectangle belongs to the area containing its center.
constexpr bool contains_center(Geometry area, Geometry rectangle)
{
    auto [x, y] = center(rectangle);
    return contains(area, x, y);
}

// Root-relative dock reservation (_NET_WM_STRUT_PARTIAL or legacy _NET_WM_STRUT).
struct EdgeReservation
{
    uint32_t depth = 0;
    uint32_t start = 0;
    uint32_t end = UINT32_MAX; // Inclusive root-coordinate range; legacy struts span the edge.
    bool operator==(EdgeReservation const&) const = default;
};
struct DockStrut
{
    EdgeReservation left, right, top, bottom;
    bool operator==(DockStrut const&) const = default;
};

/// One monitor's reserved edges, projected from every dock reservation.
struct Strut
{
    uint32_t left = 0;
    uint32_t right = 0;
    uint32_t top = 0;
    uint32_t bottom = 0;
    bool operator==(Strut const&) const = default;
};

/// Edge monitor indices for _NET_WM_FULLSCREEN_MONITORS geometry.
struct FullscreenMonitors
{
    uint32_t top = 0;
    uint32_t bottom = 0;
    uint32_t left = 0;
    uint32_t right = 0;

    bool operator==(FullscreenMonitors const&) const = default;
};

/// Mutually exclusive _NET_WM_STATE_ABOVE / _BELOW preference.
enum class LayerHint
{
    Normal,
    Above,
    Below,
};

// Unset fields follow classification defaults. Explicit requests (rules, user,
// or application) replace the same preference; they are not competing layers.
struct ClientPreferences
{
    std::optional<bool> floating;
    std::optional<bool> skip_taskbar;
    std::optional<bool> skip_pager;
    std::optional<LayerHint> layer;

    bool operator==(ClientPreferences const&) const = default;
};

enum class UrgencySource : uint8_t
{
    WmInitiated = 1U << 0,
    App = 1U << 1,
};

struct Urgency
{
    uint8_t sources = 0;
    bool operator==(Urgency const&) const = default;

    bool active() const { return sources != 0; }

    bool has(UrgencySource source) const { return (sources & static_cast<uint8_t>(source)) != 0; }
    void set(UrgencySource source, bool enabled)
    {
        auto bit = static_cast<uint8_t>(source);
        sources = enabled ? sources | bit : sources & static_cast<uint8_t>(~bit);
    }
};

/// How LWM manages an observed window. Popups are mapped but never registered.
enum class WindowRole
{
    Client,
    Dock,
    Desktop,
    Popup
};

/// _NET_WM_STATE values LWM understands, independent of their atoms.
enum class WindowState : uint8_t
{
    Fullscreen,
    Above,
    Below,
    Sticky,
    Modal,
    SkipTaskbar,
    SkipPager,
    MaximizedHorz,
    MaximizedVert,
    Hidden,
    DemandsAttention,
    Focused,
    Count
};

struct WindowStates
{
    uint16_t bits = 0;

    bool has(WindowState state) const { return (bits >> static_cast<unsigned>(state)) & 1U; }
    void set(WindowState state, bool enabled = true)
    {
        auto bit = static_cast<uint16_t>(1U << static_cast<unsigned>(state));
        bits = enabled ? bits | bit : bits & static_cast<uint16_t>(~bit);
    }
    bool operator==(WindowStates const&) const = default;
};

/// How a _NET_WM_STATE request changes the named states.
enum class StateChange : uint8_t
{
    Remove,
    Add,
    Toggle
};

/// Requested floating geometry fields; absent fields keep the normal rectangle's values.
struct GeometryRequest
{
    std::optional<int16_t> x, y;
    std::optional<uint16_t> width, height;
};

/// WM_NORMAL_HINTS placement fields. User positions always count; program
/// positions count only for windows that are not transient.
struct SizeHints
{
    std::optional<std::pair<int16_t, int16_t>> user_position;
    std::optional<std::pair<int16_t, int16_t>> program_position;
    std::optional<uint16_t> width;
    std::optional<uint16_t> height;
    bool operator==(SizeHints const&) const = default;
};

/// Application properties observed when LWM first sees a window. The model
/// derives role, placement and initial state from these values alone.
struct WindowObservation
{
    xcb_window_t id = XCB_NONE;
    std::string name;
    std::string wm_class;
    std::string wm_class_name;
    WindowType type = WindowType::Normal;
    xcb_window_t transient_for = XCB_NONE;
    std::optional<Geometry> unmanaged_parent; ///< Server rectangle of a parent LWM does not manage
    std::optional<uint32_t> desktop;          ///< _NET_WM_DESKTOP, including STICKY_DESKTOP
    WindowStates states;                      ///< _NET_WM_STATE
    bool accepts_input = true;                ///< WM_HINTS input (ICCCM default: true)
    bool initially_iconic = false;            ///< WM_HINTS initial_state
    bool urgent = false;                      ///< WM_HINTS urgency
    bool supports_take_focus = false;
    uint32_t user_time = 0;
    xcb_window_t user_time_window = XCB_NONE;
    std::optional<FullscreenMonitors> fullscreen_monitors;
    std::optional<Geometry> geometry; ///< Current server rectangle
    SizeHints size_hints;
    DockStrut strut;
};

/// Typed actions of a window rule. Unset fields leave the window unchanged.
struct RuleActions
{
    std::optional<bool> floating;
    std::optional<size_t> workspace;
    std::optional<std::variant<size_t, std::string>> monitor; ///< Index, or output name resolved when applied
    std::optional<bool> fullscreen;
    std::optional<LayerHint> layer;
    std::optional<bool> sticky;
    std::optional<bool> skip_taskbar;
    std::optional<bool> skip_pager;
    std::optional<bool> borderless;
    std::optional<Geometry> geometry;
    bool center = false;
    std::optional<std::string> scratchpad;

    bool operator==(RuleActions const&) const = default;
};

/// Position in a workspace's tiled order before a tile became floating. It is
/// honored only when the window returns to tiling on the same workspace.
struct TileSlot
{
    size_t index = 0;
    std::string output; ///< Stable across monitor enumeration and exec restart
    size_t workspace = 0;

    bool operator==(TileSlot const&) const = default;
};

struct TiledMode
{
    std::optional<Geometry> floating; ///< Floating rectangle to restore when floated again
    bool operator==(TiledMode const&) const = default;
};

struct FloatingMode
{
    Geometry geometry; ///< Normal rectangle; maximize and fullscreen only project it
    std::optional<TileSlot> tile_slot;
    bool operator==(FloatingMode const&) const = default;
};

using ClientMode = std::variant<TiledMode, FloatingMode>;

// Private intent survives exec; application properties are observed afresh.
// This is also the handoff value, so restoration cannot omit individual fields.
struct ClientIntent
{
    xcb_window_t id = XCB_NONE;
    size_t monitor = 0;
    size_t workspace = 0;
    ClientMode mode = TiledMode{ };
    ClientPreferences preferences;
    Urgency urgency;
    bool borderless = false;
    bool desktop_pinned = false;
    std::optional<FullscreenMonitors> fullscreen_monitors;
    uint64_t mru_order = 0; ///< Completed focus recency; zero means never focused
    uint64_t order = 0; ///< Registration rank shared by clients and fixtures

    bool operator==(ClientIntent const&) const = default;
};

// The same value owns a named slot in memory and in the handoff. Null means a
// pending launch; NONE means an empty slot; another ID means a claimed client.
struct NamedScratchpad
{
    std::string name;
    std::optional<xcb_window_t> window = XCB_NONE;

    xcb_window_t claimed_window() const { return window.value_or(XCB_NONE); }
    bool pending_launch() const { return !window; }
    bool operator==(NamedScratchpad const&) const = default;
};

/// A managed normal window. Placement is always a valid monitor/workspace pair;
/// docks and desktop windows are Fixtures and never Clients.
struct Client : ClientIntent
{
    enum class Kind
    {
        Tiled,
        Floating
    };

    Kind kind() const { return std::holds_alternative<TiledMode>(mode) ? Kind::Tiled : Kind::Floating; }

    std::string name;
    std::string wm_class;
    std::string wm_class_name;
    WindowType ewmh_type = WindowType::Normal;
    xcb_window_t transient_for = XCB_NONE;

    bool fullscreen = false;     ///< _NET_WM_STATE_FULLSCREEN
    bool iconic = false;         ///< _NET_WM_STATE_HIDDEN (minimized)
    bool sticky = false;         ///< _NET_WM_STATE_STICKY
    bool maximized_horz = false; ///< Retained for any mode; only floating presentation honors it
    bool maximized_vert = false;
    bool modal = false;          ///< _NET_WM_STATE_MODAL

    bool accepts_input = true;        ///< WM_HINTS input field (ICCCM default: true)
    bool supports_take_focus = false; ///< WM_PROTOCOLS contains WM_TAKE_FOCUS
    uint32_t user_time = 0;
    xcb_window_t user_time_window = XCB_NONE;
    SizeHints size_hints;

    /// Actions of the rule matched at the last manage, metadata change, or
    /// reload. Metadata changes apply a rule only when this result changes.
    std::optional<RuleActions> rule;
};

/// Stable lowercase name for a client kind shared by IPC JSON and `_LWM_WINDOW_CLASS`.
inline char const* client_kind_str(Client::Kind kind)
{
    return kind == Client::Kind::Tiled ? "tiled" : "floating";
}

inline TiledMode* tiled_mode(Client& client) { return std::get_if<TiledMode>(&client.mode); }
inline TiledMode const* tiled_mode(Client const& client) { return std::get_if<TiledMode>(&client.mode); }
inline FloatingMode* floating_mode(Client& client) { return std::get_if<FloatingMode>(&client.mode); }
inline FloatingMode const* floating_mode(Client const& client) { return std::get_if<FloatingMode>(&client.mode); }

// Tiled clients retain maximize flags as a preference; only floating presentation honors them.
inline bool presents_maximized(Client const& client)
{
    return client.kind() == Client::Kind::Floating && (client.maximized_horz || client.maximized_vert);
}

/// Docks and desktop windows: registered, listed and stacked, but outside
/// workspaces, layout, and focus. Role and registration rank survive exec.
struct FixtureIntent
{
    enum class Role
    {
        Dock,
        Desktop
    };

    xcb_window_t id = XCB_NONE;
    Role role = Role::Dock;
    uint64_t order = 0;
    bool operator==(FixtureIntent const&) const = default;
};

struct Fixture : FixtureIntent
{
    DockStrut strut; ///< Observed reservation; only docks reserve workarea
};

inline char const* fixture_role_str(Fixture::Role role) { return role == Fixture::Role::Dock ? "dock" : "desktop"; }

/// Split 0 divides master and stack; split i > 0 divides stack slot i
/// from the remaining slots. Identity is stable when the window count changes.
struct SplitAddress
{
    uint32_t index = 0;
    auto operator<=>(SplitAddress const&) const = default;
};

using SplitRatioMap = std::map<SplitAddress, double>;

enum class LayoutStrategy
{
    MasterStack,
    Monocle ///< All tiled windows occupy the full content rect; stacking determines which is on top.
};

/// Stable name for a layout strategy. Single source of truth for IPC JSON,
/// config parsing, and command replies.
inline char const* layout_strategy_str(LayoutStrategy strategy)
{
    switch (strategy)
    {
        case LayoutStrategy::MasterStack:
            return "master-stack";
        case LayoutStrategy::Monocle:
            return "monocle";
    }
    return "unknown";
}

inline std::optional<LayoutStrategy> parse_layout_strategy(std::string_view name)
{
    if (name == "master-stack")
        return LayoutStrategy::MasterStack;
    if (name == "monocle")
        return LayoutStrategy::Monocle;
    return std::nullopt;
}

struct Workspace
{
    std::vector<xcb_window_t> windows;
    xcb_window_t preferred_tile = XCB_NONE; ///< Destination preference; actual tiled focus clears it

    LayoutStrategy layout_strategy = LayoutStrategy::MasterStack;
    SplitRatioMap split_ratios; ///< Per-workspace split ratios, keyed by split index

    auto find_window(xcb_window_t id) { return std::ranges::find(windows, id); }

    auto find_window(xcb_window_t id) const { return std::ranges::find(windows, id); }
    bool operator==(Workspace const&) const = default;
};

// Discovered outputs and the root extent that right/bottom reservations refer to.
struct Topology
{
    struct Output
    {
        std::string name;
        Geometry geometry;
    };
    std::vector<Output> outputs;
    Geometry screen;
};

// The workspace graph and output identity survive exec. Workarea reservations
// are derived from the current docks, not saved.
struct MonitorState
{
    std::string name;
    Geometry geometry;
    std::vector<Workspace> workspaces;
    size_t current_workspace = 0;
    size_t previous_workspace = 0;

    Workspace& current() { return workspaces[current_workspace]; }
    Workspace const& current() const { return workspaces[current_workspace]; }

    bool operator==(MonitorState const&) const = default;
};

struct Monitor : MonitorState
{
    Strut strut = {}; ///< Derived from dock reservations

    Geometry working_area() const
    {
        auto const& [x, y, width, height] = geometry;
        uint64_t horizontal = static_cast<uint64_t>(strut.left) + strut.right;
        uint64_t vertical = static_cast<uint64_t>(strut.top) + strut.bottom;
        // Oversized struts consume the extent without shifting the origin.
        return { geometry_coordinate(static_cast<int64_t>(x) + (horizontal >= width ? 0 : strut.left)),
                 geometry_coordinate(static_cast<int64_t>(y) + (vertical >= height ? 0 : strut.top)),
                 geometry_extent(static_cast<int64_t>(width) - std::min<uint64_t>(width, horizontal)),
                 geometry_extent(static_cast<int64_t>(height) - std::min<uint64_t>(height, vertical)) };
    }
};

// The smallest rectangle containing every rectangle in a range, if any.
template <std::ranges::forward_range Rectangles> std::optional<Geometry> bounds(Rectangles&& rectangles)
{
    if (std::ranges::empty(rectangles))
        return std::nullopt;
    int32_t left = INT32_MAX, top = INT32_MAX, right = INT32_MIN, bottom = INT32_MIN;
    for (Geometry const& rectangle : rectangles)
    {
        left = std::min<int32_t>(left, rectangle.x);
        top = std::min<int32_t>(top, rectangle.y);
        right = std::max<int32_t>(right, rectangle.x + rectangle.width);
        bottom = std::max<int32_t>(bottom, rectangle.y + rectangle.height);
    }
    return Geometry{ geometry_coordinate(left), geometry_coordinate(top), geometry_extent(right - left), geometry_extent(bottom - top) };
}

// The first monitor containing a point, or a rectangle's center.
inline std::optional<size_t> monitor_at(std::span<Monitor const> monitors, int32_t x, int32_t y)
{
    auto it = std::ranges::find_if(monitors, [&](auto const& monitor) { return contains(monitor.geometry, x, y); });
    return it == monitors.end() ? std::nullopt : std::optional{ static_cast<size_t>(it - monitors.begin()) };
}
inline std::optional<size_t> monitor_at(std::span<Monitor const> monitors, Geometry rectangle)
{
    auto [x, y] = center(rectangle);
    return monitor_at(monitors, x, y);
}

} // namespace lwm
