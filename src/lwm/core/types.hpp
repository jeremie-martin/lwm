#pragma once

#include "lwm/core/command.hpp"
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

struct VisibleScratchpadPoolMembership
{ };

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

// Unset fields follow classification defaults. Explicit requests (rules, user,
// or application) replace the same preference; they are not competing layers.
struct ClientPreferences
{
    std::optional<bool> floating;
    std::optional<bool> skip_taskbar;
    std::optional<bool> skip_pager;
    std::optional<LayerHint> layer;
};

enum class UrgencySource : uint8_t
{
    WmInitiated = 1U << 0,
    App = 1U << 1,
};

struct Urgency
{
    uint8_t sources = 0;

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

struct DockState
{ };
struct DesktopState
{ };

using ClientState = std::variant<TiledState, FloatingState, DockState, DesktopState>;

struct ClientPresentation
{
    bool hidden = false;
    std::optional<Geometry> applied_geometry;
    uint32_t applied_border = 0;
    bool ignore_next_wm_hints_urgency_echo = false;
    uint32_t sync_counter = 0;
    uint64_t sync_value = 0;
};

/// Managed-window record owned by WindowManager::clients_. The state variant
/// holds kind-specific data and is the authority for kind().
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
    Kind kind() const
    {
        if (std::holds_alternative<TiledState>(state))
            return Kind::Tiled;
        if (std::holds_alternative<FloatingState>(state))
            return Kind::Floating;
        if (std::holds_alternative<DockState>(state))
            return Kind::Dock;
        return Kind::Desktop;
    }

    size_t monitor = 0;
    size_t workspace = 0;

    std::string name;
    std::string wm_class;
    std::string wm_class_name;

    bool fullscreen = false;                  ///< _NET_WM_STATE_FULLSCREEN
    LayerHint layer_hint = LayerHint::Normal; ///< _NET_WM_STATE_ABOVE / _BELOW (tri-state)
    bool iconic = false;                      ///< _NET_WM_STATE_HIDDEN (minimized)
    bool sticky = false;                      ///< _NET_WM_STATE_STICKY
    bool maximized_horz = false;              ///< _NET_WM_STATE_MAXIMIZED_HORZ
    bool maximized_vert = false;              ///< _NET_WM_STATE_MAXIMIZED_VERT

    bool modal = false;                             ///< _NET_WM_STATE_MODAL
    bool skip_taskbar = false;                      ///< _NET_WM_STATE_SKIP_TASKBAR
    bool skip_pager = false;                        ///< _NET_WM_STATE_SKIP_PAGER
    ClientPreferences preferences;
    Urgency urgency;                                ///< _NET_WM_STATE_DEMANDS_ATTENTION provenance
    bool borderless = false;                        ///< WM-managed zero-border window
    WindowType ewmh_type = WindowType::Normal;      ///< Cached EWMH window type
    bool accepts_input = true;                      ///< Cached WM_HINTS input field (ICCCM default: true)
    bool supports_take_focus = false;               ///< Cached: WM_PROTOCOLS contains WM_TAKE_FOCUS
    bool desktop_pinned = false;                    ///< Client supplied a concrete _NET_WM_DESKTOP assignment

    using SavedTilePos = lwm::SavedTilePos;

    ClientState state = TiledState{};
    ClientPresentation presentation;
    Geometry tiled_geometry; ///< Latest layout target; initially the client rectangle
    xcb_window_t transient_for = XCB_NONE;
    bool suppress_next_configure_request =
        false; ///< Preserve WM-chosen startup placement against one client resize/move request

    std::optional<FullscreenMonitors> fullscreen_monitors;  ///< Multi-monitor fullscreen


    uint32_t user_time = 0;                   ///< Last user interaction time
    xcb_window_t user_time_window = XCB_NONE; ///< _NET_WM_USER_TIME_WINDOW

    uint64_t order = 0;     ///< Mapping order for _NET_CLIENT_LIST
    uint64_t mru_order = 0; ///< Focus recency for tiled/floating clients (higher = newer)

    std::optional<ScratchpadMembership> scratchpad; ///< Named or generic scratchpad membership
};

/// Stable lowercase name for a client kind. Single source of truth for IPC
/// JSON, the `_LWM_WINDOW_CLASS` property, and any other kind-to-string use.
inline char const* client_kind_str(Client::Kind kind)
{
    switch (kind)
    {
        case Client::Kind::Tiled:
            return "tiled";
        case Client::Kind::Floating:
            return "floating";
        case Client::Kind::Dock:
            return "dock";
        case Client::Kind::Desktop:
            return "desktop";
    }
    return "unknown";
}

inline TiledState* tiled_state(Client& client) { return std::get_if<TiledState>(&client.state); }

inline TiledState const* tiled_state(Client const& client) { return std::get_if<TiledState>(&client.state); }

inline FloatingState* floating_state(Client& client) { return std::get_if<FloatingState>(&client.state); }

inline FloatingState const* floating_state(Client const& client) { return std::get_if<FloatingState>(&client.state); }

inline Geometry& floating_geometry(Client& client) { return std::get<FloatingState>(client.state).geometry; }

inline Geometry const& floating_geometry(Client const& client)
{
    return std::get<FloatingState>(client.state).geometry;
}

inline std::optional<Geometry>& prior_floating_geometry(Client& client)
{
    return std::get<TiledState>(client.state).prior_floating;
}

inline std::optional<Geometry> const& prior_floating_geometry(Client const& client)
{
    return std::get<TiledState>(client.state).prior_floating;
}

inline std::optional<SavedTilePos>& saved_tiled_pos(Client& client)
{
    return std::get<FloatingState>(client.state).saved_tiled_pos;
}

inline std::optional<SavedTilePos> const& saved_tiled_pos(Client const& client)
{
    return std::get<FloatingState>(client.state).saved_tiled_pos;
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

/// Split 0 divides master and stack; split i > 0 divides stack slot i
/// from the remaining slots. Identity is stable when the window count changes.
struct SplitAddress
{
    uint32_t index = 0;
    auto operator<=>(SplitAddress const&) const = default;
};

// Preserve the version-3 restart representation for the old right-leaning tree.
struct SerializedSplitAddress
{
    uint32_t depth;
    uint32_t path;
};

constexpr SerializedSplitAddress serialize_split_address(SplitAddress address)
{
    return { address.index, address.index < 32 ? (uint32_t{ 1 } << address.index) - 1 : UINT32_MAX };
}

constexpr std::optional<SplitAddress> deserialize_split_address(uint32_t depth, uint32_t path)
{
    SplitAddress address{ depth };
    if (serialize_split_address(address).path != path)
        return std::nullopt;
    return address;
}

using SplitRatioMap = std::map<SplitAddress, double>;

/// Layout strategy for a workspace's tiling algorithm.
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
    xcb_window_t focused_window = XCB_NONE;
    std::vector<xcb_window_t> focus_history; ///< MRU stack; back = most recent

    LayoutStrategy layout_strategy = LayoutStrategy::MasterStack;
    SplitRatioMap split_ratios; ///< Per-workspace split ratios, keyed by split index

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
        uint64_t horizontal = static_cast<uint64_t>(strut.left) + strut.right;
        uint64_t vertical = static_cast<uint64_t>(strut.top) + strut.bottom;
        // Oversized struts consume the extent without shifting the origin.
        return { geometry_coordinate(static_cast<int64_t>(x) + (horizontal >= width ? 0 : strut.left)),
                 geometry_coordinate(static_cast<int64_t>(y) + (vertical >= height ? 0 : strut.top)),
                 geometry_extent(static_cast<int64_t>(width) - std::min<uint64_t>(width, horizontal)),
                 geometry_extent(static_cast<int64_t>(height) - std::min<uint64_t>(height, vertical)) };
    }
};

struct KeyBinding
{
    uint16_t modifier;
    xcb_keysym_t keysym;

    auto operator<=>(KeyBinding const&) const = default;
};

struct KillAction
{ };
struct ReloadConfigAction
{ };
struct RestartAction
{ };
struct ToggleWorkspaceAction
{ };
struct ToggleFullscreenAction
{ };
struct ToggleFloatAction
{ };
struct FocusNextAction
{ };
struct FocusPrevAction
{ };
struct RatioGrowAction
{ };
struct RatioShrinkAction
{ };
struct SwapNextAction
{ };
struct SwapPrevAction
{ };
struct ScratchpadStashAction
{ };
struct ScratchpadCycleAction
{ };

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
    SwapNextAction,
    SwapPrevAction,
    ScratchpadStashAction,
    ScratchpadCycleAction,
    SpawnAction,
    SwitchWorkspaceAction,
    MoveToWorkspaceAction,
    FocusMonitorAction,
    MoveToMonitorAction,
    ToggleScratchpadAction>;

} // namespace lwm
