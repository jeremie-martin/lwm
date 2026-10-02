#pragma once

#include <algorithm>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>
#include <xcb/randr.h>
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

/// ICCCM WM_STATE values
constexpr uint32_t WM_STATE_WITHDRAWN = 0;
constexpr uint32_t WM_STATE_NORMAL = 1;
constexpr uint32_t WM_STATE_ICONIC = 3;

/// ICCCM WM_HINTS urgency flag (not exposed by xcb_icccm as a named constant)
constexpr uint32_t XUrgencyHint = 256; // 1L << 8

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

    bool add(UrgencySource source)
    {
        uint8_t const bit = static_cast<uint8_t>(source);
        bool const changed = (sources & bit) == 0;
        sources |= bit;
        return changed;
    }

    bool remove(UrgencySource source)
    {
        uint8_t const bit = static_cast<uint8_t>(source);
        bool const changed = (sources & bit) != 0;
        sources &= static_cast<uint8_t>(~bit);
        return changed;
    }

    bool clear()
    {
        bool const changed = sources != 0;
        sources = 0;
        return changed;
    }
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

    bool operator==(ClientIntent const&) const = default;
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

    uint64_t order = 0;            ///< Registration order (_NET_CLIENT_LIST)

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
/// workspaces, layout, and focus.
struct Fixture
{
    enum class Role
    {
        Dock,
        Desktop
    };

    xcb_window_t id = XCB_NONE;
    Role role = Role::Dock;
    uint64_t order = 0;
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

// The workspace graph and output identity survive exec. RandR handles and dock
// reservations belong to the discovered monitor, not to the saved graph.
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
    xcb_randr_output_t output = XCB_NONE;
    Strut strut = {};

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

} // namespace lwm
