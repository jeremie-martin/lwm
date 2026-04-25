#pragma once

#include "lwm/core/command.hpp"
#include <algorithm>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <variant>
#include <vector>
#include <xcb/randr.h>
#include <xcb/xcb.h>

namespace lwm {

/// EWMH window type classification
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

/// Off-screen X coordinate for hidden windows (DWM-style visibility management)
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
};

struct Strut
{
    uint32_t left = 0;
    uint32_t right = 0;
    uint32_t top = 0;
    uint32_t bottom = 0;
};

/**
 * @brief Fullscreen monitor configuration for _NET_WM_FULLSCREEN_MONITORS.
 *
 * Specifies which monitors a fullscreen window should span.
 */
struct FullscreenMonitors
{
    uint32_t top = 0;
    uint32_t bottom = 0;
    uint32_t left = 0;
    uint32_t right = 0;
};

enum class WindowLayer
{
    Normal,
    Overlay
};

/// EWMH layer preference (`_NET_WM_STATE_ABOVE` / `_BELOW`).
/// Tri-state by construction — making "both above and below" unrepresentable.
enum class LayerHint
{
    Normal,
    Above,
    Below,
};

struct NamedScratchpadMembership
{
    std::string name;
};

struct VisibleScratchpadPoolMembership {};

struct HiddenTiledScratchpadPoolMembership
{
    std::optional<Geometry> prior_floating;
};

struct HiddenFloatingScratchpadPoolMembership
{
    Geometry restore_geometry;
};

using ScratchpadMembership = std::variant<
    NamedScratchpadMembership,
    VisibleScratchpadPoolMembership,
    HiddenTiledScratchpadPoolMembership,
    HiddenFloatingScratchpadPoolMembership>;

struct AppPreferences
{
    bool skip_taskbar = false;
    bool skip_pager = false;
    bool above = false;
};

enum class UrgencySource : uint8_t
{
    WmInitiated = 1U << 0,
    App = 1U << 1,
};

struct Urgency
{
    uint8_t sources = 0;

    bool active() const
    {
        return sources != 0;
    }

    bool has(UrgencySource source) const
    {
        return (sources & static_cast<uint8_t>(source)) != 0;
    }

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

/// Saved position in ws.windows before the last float conversion.
/// Only valid when monitor/workspace match the conversion target; used to
/// restore layout order when a window returns to the same workspace as tiled.
struct SavedTilePos
{
    size_t index = 0;
    size_t monitor = 0;
    size_t workspace = 0;
};

struct TiledState
{
    std::optional<Geometry> prior_floating;
};

struct FloatingState
{
    Geometry geometry;
    std::optional<SavedTilePos> saved_tiled_pos;
};

using TilingState = std::variant<TiledState, FloatingState>;

/**
 * @brief Unified client record representing any managed window.
 *
 * This struct is the authoritative source of truth for all per-window state.
 * It replaces the scattered unordered_set and unordered_map structures that
 * were previously used (fullscreen caches, iconic windows, etc.).
 *
 * Design rationale:
 * - Single authoritative source: all state for a window is in one place
 * - O(1) lookup for any window property via the clients_ map
 * - Eliminates state synchronization bugs between multiple data structures
 * - Simplifies invariant reasoning and debugging
 * - Unifies tiled and floating window handling for state management
 *
 * State flags managed here:
 * - fullscreen, iconic, sticky, layer_hint (Above/Below as a tri-state)
 * - maximized_horz, maximized_vert, modal
 * - skip_taskbar
 *
 * Restore geometries:
 * - fullscreen_restore: geometry before entering fullscreen
 * - maximize_restore: geometry before maximizing
 *
 * @see WindowManager::clients_ for the central registry
 */
struct Client
{
    xcb_window_t id = XCB_NONE;

    /**
     * @brief Classification of the window type.
     *
     * - Tiled: Participates in workspace tiling layout
     * - Floating: Positioned independently, does not affect tiling
     * - Dock: Panel/bar that reserves screen edges (strut)
     * - Desktop: Background/desktop window (_NET_WM_WINDOW_TYPE_DESKTOP)
     */
    enum class Kind
    {
        Tiled,
        Floating,
        Dock,
        Desktop
    };
    Kind kind = Kind::Tiled;

    size_t monitor = 0;
    size_t workspace = 0;

    std::string name;
    std::string wm_class;
    std::string wm_class_name;

    bool hidden = false;            ///< True when window is moved off-screen by WM
    bool fullscreen = false;        ///< _NET_WM_STATE_FULLSCREEN
    LayerHint layer_hint = LayerHint::Normal; ///< _NET_WM_STATE_ABOVE / _BELOW (tri-state)
    bool iconic = false;            ///< _NET_WM_STATE_HIDDEN (minimized)
    bool sticky = false;            ///< _NET_WM_STATE_STICKY
    bool maximized_horz = false;    ///< _NET_WM_STATE_MAXIMIZED_HORZ
    bool maximized_vert = false;    ///< _NET_WM_STATE_MAXIMIZED_VERT

    bool modal = false;             ///< _NET_WM_STATE_MODAL
    bool skip_taskbar = false;      ///< _NET_WM_STATE_SKIP_TASKBAR
    bool skip_pager = false;        ///< _NET_WM_STATE_SKIP_PAGER
    AppPreferences app_prefs;       ///< App-declared EWMH preferences before WM policy
    Urgency urgency;                ///< _NET_WM_STATE_DEMANDS_ATTENTION provenance
    bool ignore_next_wm_hints_urgency_echo = false; ///< Skip the PropertyNotify from our own WM_HINTS write
    bool borderless = false;        ///< WM-managed zero-border window
    WindowLayer layer = WindowLayer::Normal;

    WindowType ewmh_type = WindowType::Normal; ///< Cached EWMH window type
    bool accepts_input = true;       ///< Cached WM_HINTS input field (ICCCM default: true)
    bool supports_take_focus = false; ///< Cached: WM_PROTOCOLS contains WM_TAKE_FOCUS

    using SavedTilePos = lwm::SavedTilePos;

    TilingState tiling_state = TiledState {};
    Geometry tiled_geometry;             ///< Last applied tiled layout geometry (avoids X round-trip)
    xcb_window_t transient_for = XCB_NONE;
    bool suppress_next_configure_request = false; ///< Preserve WM-chosen startup placement against one client resize/move request

    std::optional<Geometry> fullscreen_restore;            ///< Geometry before fullscreen
    std::optional<Geometry> maximize_restore;              ///< Geometry before maximize
    std::optional<FullscreenMonitors> fullscreen_monitors; ///< Multi-monitor fullscreen

    uint32_t sync_counter = 0; ///< XSync counter ID (0 if none)
    uint64_t sync_value = 0;   ///< Expected counter value

    uint32_t user_time = 0;                   ///< Last user interaction time
    xcb_window_t user_time_window = XCB_NONE; ///< _NET_WM_USER_TIME_WINDOW

    uint64_t order = 0;     ///< Mapping order for _NET_CLIENT_LIST
    uint64_t mru_order = 0;  ///< MRU ordering for floating windows (higher = more recent)

    std::optional<ScratchpadMembership> scratchpad; ///< Named or generic scratchpad membership
};

inline bool is_tiled(Client const& client)
{
    return client.kind == Client::Kind::Tiled;
}

inline bool is_floating(Client const& client)
{
    return client.kind == Client::Kind::Floating;
}

inline bool is_dock(Client const& client)
{
    return client.kind == Client::Kind::Dock;
}

inline bool is_desktop(Client const& client)
{
    return client.kind == Client::Kind::Desktop;
}

inline bool is_user_window(Client const& client)
{
    return is_tiled(client) || is_floating(client);
}

inline TiledState* tiled_state(Client& client)
{
    return std::get_if<TiledState>(&client.tiling_state);
}

inline TiledState const* tiled_state(Client const& client)
{
    return std::get_if<TiledState>(&client.tiling_state);
}

inline FloatingState* floating_state(Client& client)
{
    return std::get_if<FloatingState>(&client.tiling_state);
}

inline FloatingState const* floating_state(Client const& client)
{
    return std::get_if<FloatingState>(&client.tiling_state);
}

inline Geometry& floating_geometry(Client& client)
{
    return std::get<FloatingState>(client.tiling_state).geometry;
}

inline Geometry const& floating_geometry(Client const& client)
{
    return std::get<FloatingState>(client.tiling_state).geometry;
}

inline std::optional<Geometry>& prior_floating_geometry(Client& client)
{
    return std::get<TiledState>(client.tiling_state).prior_floating;
}

inline std::optional<Geometry> const& prior_floating_geometry(Client const& client)
{
    return std::get<TiledState>(client.tiling_state).prior_floating;
}

inline std::optional<SavedTilePos>& saved_tiled_pos(Client& client)
{
    return std::get<FloatingState>(client.tiling_state).saved_tiled_pos;
}

inline std::optional<SavedTilePos> const& saved_tiled_pos(Client const& client)
{
    return std::get<FloatingState>(client.tiling_state).saved_tiled_pos;
}

inline void set_tiled_state(Client& client, std::optional<Geometry> prior_floating = std::nullopt)
{
    client.kind = Client::Kind::Tiled;
    client.tiling_state = TiledState { prior_floating };
}

inline void set_floating_state(
    Client& client,
    Geometry geometry,
    std::optional<SavedTilePos> saved_position = std::nullopt)
{
    client.kind = Client::Kind::Floating;
    client.tiling_state = FloatingState { geometry, saved_position };
}

inline NamedScratchpadMembership const* scratchpad_named(Client const& client)
{
    if (!client.scratchpad)
        return nullptr;
    return std::get_if<NamedScratchpadMembership>(&*client.scratchpad);
}

inline HiddenFloatingScratchpadPoolMembership const* hidden_floating_pool_scratchpad(Client const& client)
{
    if (!client.scratchpad)
        return nullptr;
    return std::get_if<HiddenFloatingScratchpadPoolMembership>(&*client.scratchpad);
}

inline HiddenTiledScratchpadPoolMembership const* hidden_tiled_pool_scratchpad(Client const& client)
{
    if (!client.scratchpad)
        return nullptr;
    return std::get_if<HiddenTiledScratchpadPoolMembership>(&*client.scratchpad);
}

inline bool is_hidden_tiled_pool_scratchpad(Client const& client)
{
    return hidden_tiled_pool_scratchpad(client) != nullptr;
}

inline bool is_hidden_pool_scratchpad(Client const& client)
{
    return is_hidden_tiled_pool_scratchpad(client) || hidden_floating_pool_scratchpad(client) != nullptr;
}

/// Identifies a split node by its structural position in the layout tree.
/// path encodes the sequence of left(0)/right(1) choices from the root:
/// bit i = child direction taken at depth i.
struct SplitAddress
{
    uint8_t depth;   // 0 = root
    uint32_t path;   // bit field: bit i = direction at depth i (0=left, 1=right)

    auto operator<=>(SplitAddress const&) const = default;
};

struct SerializedSplitAddress
{
    uint32_t depth;
    uint32_t path;
};

constexpr SerializedSplitAddress serialize_split_address(SplitAddress const& address)
{
    return { static_cast<uint32_t>(address.depth), address.path };
}

constexpr std::optional<SplitAddress> deserialize_split_address(uint32_t depth, uint32_t path)
{
    if (depth > std::numeric_limits<uint8_t>::max())
        return std::nullopt;
    return SplitAddress { static_cast<uint8_t>(depth), path };
}

using SplitRatioMap = std::map<SplitAddress, double>;

/// Layout strategy for a workspace's tiling algorithm.
enum class LayoutStrategy
{
    MasterStack
};

struct Workspace
{
    std::vector<xcb_window_t> windows;
    xcb_window_t focused_window = XCB_NONE;
    std::vector<xcb_window_t> focus_history; ///< MRU stack; back = most recent

    LayoutStrategy layout_strategy = LayoutStrategy::MasterStack;
    SplitRatioMap split_ratios; ///< Per-workspace split ratios, keyed by structural address

    auto find_window(xcb_window_t id) { return std::ranges::find(windows, id); }

    auto find_window(xcb_window_t id) const { return std::ranges::find(windows, id); }
};

struct Monitor
{
    xcb_randr_output_t output = XCB_NONE;
    std::string name;
    int16_t x = 0;
    int16_t y = 0;
    uint16_t width = 0;
    uint16_t height = 0;
    std::vector<Workspace> workspaces;
    size_t current_workspace = 0;
    size_t previous_workspace = 0;
    Strut strut = {};
    xcb_window_t fullscreen_owner = XCB_NONE; ///< Window owning fullscreen on this monitor (at most one)

    Workspace& current() { return workspaces[current_workspace]; }
    Workspace const& current() const { return workspaces[current_workspace]; }

    Geometry geometry() const { return { x, y, width, height }; }

    Geometry working_area() const
    {
        int32_t w = static_cast<int32_t>(width);
        int32_t h = static_cast<int32_t>(height);
        int32_t left = static_cast<int32_t>(strut.left);
        int32_t right = static_cast<int32_t>(strut.right);
        int32_t top = static_cast<int32_t>(strut.top);
        int32_t bottom = static_cast<int32_t>(strut.bottom);

        // Clamp total struts to monitor dimensions
        int32_t h_strut = std::min(w, left + right);
        int32_t v_strut = std::min(h, top + bottom);

        // Calculate working area dimensions (minimum 1)
        int32_t area_w = std::max<int32_t>(1, w - h_strut);
        int32_t area_h = std::max<int32_t>(1, h - v_strut);

        // Clamp left/top offsets: if struts exceed dimension, use no offset
        int32_t offset_x = (left + right >= w) ? 0 : left;
        int32_t offset_y = (top + bottom >= h) ? 0 : top;

        int32_t area_x = static_cast<int32_t>(x) + offset_x;
        int32_t area_y = static_cast<int32_t>(y) + offset_y;

        return { static_cast<int16_t>(area_x),
                 static_cast<int16_t>(area_y),
                 static_cast<uint16_t>(area_w),
                 static_cast<uint16_t>(area_h) };
    }
};

struct KeyBinding
{
    uint16_t modifier;
    xcb_keysym_t keysym;

    auto operator<=>(KeyBinding const&) const = default;
};

struct KillAction {};
struct ReloadConfigAction {};
struct RestartAction {};
struct ToggleWorkspaceAction {};
struct ToggleFullscreenAction {};
struct ToggleFloatAction {};
struct FocusNextAction {};
struct FocusPrevAction {};
struct RatioGrowAction {};
struct RatioShrinkAction {};
struct ScratchpadStashAction {};
struct ScratchpadCycleAction {};

struct SpawnAction
{
    CommandConfig command;
};

struct SwitchWorkspaceAction
{
    size_t workspace = 0;
};

struct MoveToWorkspaceAction
{
    size_t workspace = 0;
};

struct FocusMonitorAction
{
    int direction = 0;
};

struct MoveToMonitorAction
{
    int direction = 0;
};

struct ToggleScratchpadAction
{
    std::string name;
};

using Action = std::variant<
    KillAction,
    ReloadConfigAction,
    RestartAction,
    ToggleWorkspaceAction,
    ToggleFullscreenAction,
    ToggleFloatAction,
    FocusNextAction,
    FocusPrevAction,
    RatioGrowAction,
    RatioShrinkAction,
    ScratchpadStashAction,
    ScratchpadCycleAction,
    SpawnAction,
    SwitchWorkspaceAction,
    MoveToWorkspaceAction,
    FocusMonitorAction,
    MoveToMonitorAction,
    ToggleScratchpadAction>;

} // namespace lwm
