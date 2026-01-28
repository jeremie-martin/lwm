#pragma once

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>
#include <xcb/randr.h>
#include <xcb/xcb.h>

namespace lwm {

// ─────────────────────────────────────────────────────────────────────────────
// Constants
// ─────────────────────────────────────────────────────────────────────────────

/// Off-screen X coordinate for hidden windows (DWM-style visibility management)
constexpr int16_t OFF_SCREEN_X = -20000;

// ─────────────────────────────────────────────────────────────────────────────
// Basic geometry types (must be defined before Client)
// ─────────────────────────────────────────────────────────────────────────────

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

// ─────────────────────────────────────────────────────────────────────────────
// Unified Client record
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief Unified client record representing any managed window.
 *
 * This struct is the authoritative source of truth for all per-window state.
 * It replaces the scattered unordered_set and unordered_map structures that
 * were previously used (fullscreen_windows_, iconic_windows_, etc.).
 *
 * Design rationale:
 * - Single authoritative source: all state for a window is in one place
 * - O(1) lookup for any window property via the clients_ map
 * - Eliminates state synchronization bugs between multiple data structures
 * - Simplifies invariant reasoning and debugging
 * - Unifies tiled and floating window handling for state management
 *
 * State flags managed here:
 * - fullscreen, iconic, sticky, above, below
 * - maximized_horz, maximized_vert, shaded, modal
 * - skip_taskbar, skip_pager
 *
 * Restore geometries:
 * - fullscreen_restore: geometry before entering fullscreen
 * - maximize_restore: geometry before maximizing
 *
 * @see WindowManager::clients_ for the central registry
 * @see is_client_fullscreen(), is_client_iconic(), etc. for accessors
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

    // ─────────────────────────────────────────────────────────────────────────
    // Location (meaningful for Tiled/Floating kinds)
    // ─────────────────────────────────────────────────────────────────────────
    size_t monitor = 0;
    size_t workspace = 0;

    // ─────────────────────────────────────────────────────────────────────────
    // Identification (from WM_NAME, WM_CLASS)
    // ─────────────────────────────────────────────────────────────────────────
    std::string name;
    std::string wm_class;
    std::string wm_class_name;

    // ─────────────────────────────────────────────────────────────────────────
    // Window state flags (replaces scattered unordered_set<xcb_window_t>)
    //
    // These flags are kept in sync with _NET_WM_STATE atoms on the window.
    // ─────────────────────────────────────────────────────────────────────────
    bool hidden = false;            ///< True when window is moved off-screen by WM
    bool fullscreen = false;        ///< _NET_WM_STATE_FULLSCREEN
    bool above = false;             ///< _NET_WM_STATE_ABOVE
    bool below = false;             ///< _NET_WM_STATE_BELOW
    bool iconic = false;            ///< _NET_WM_STATE_HIDDEN (minimized)
    bool sticky = false;            ///< _NET_WM_STATE_STICKY
    bool maximized_horz = false;    ///< _NET_WM_STATE_MAXIMIZED_HORZ
    bool maximized_vert = false;    ///< _NET_WM_STATE_MAXIMIZED_VERT
    bool shaded = false;            ///< _NET_WM_STATE_SHADED
    bool modal = false;             ///< _NET_WM_STATE_MODAL
    bool skip_taskbar = false;      ///< _NET_WM_STATE_SKIP_TASKBAR
    bool skip_pager = false;        ///< _NET_WM_STATE_SKIP_PAGER
    bool demands_attention = false; ///< _NET_WM_STATE_DEMANDS_ATTENTION

    // ─────────────────────────────────────────────────────────────────────────
    // Floating-specific data (only used when kind == Floating)
    // ─────────────────────────────────────────────────────────────────────────
    Geometry floating_geometry;
    xcb_window_t transient_for = XCB_NONE;

    // ─────────────────────────────────────────────────────────────────────────
    // Geometry restore points
    // ─────────────────────────────────────────────────────────────────────────
    std::optional<Geometry> fullscreen_restore;            ///< Geometry before fullscreen
    std::optional<Geometry> maximize_restore;              ///< Geometry before maximize
    std::optional<FullscreenMonitors> fullscreen_monitors; ///< Multi-monitor fullscreen

    // ─────────────────────────────────────────────────────────────────────────
    // Sync protocol state (_NET_WM_SYNC_REQUEST)
    // ─────────────────────────────────────────────────────────────────────────
    uint32_t sync_counter = 0; ///< XSync counter ID (0 if none)
    uint64_t sync_value = 0;   ///< Expected counter value

    // ─────────────────────────────────────────────────────────────────────────
    // Focus stealing prevention (_NET_WM_USER_TIME)
    // ─────────────────────────────────────────────────────────────────────────
    uint32_t user_time = 0;                   ///< Last user interaction time
    xcb_window_t user_time_window = XCB_NONE; ///< _NET_WM_USER_TIME_WINDOW

    // ─────────────────────────────────────────────────────────────────────────
    // Management tracking
    // ─────────────────────────────────────────────────────────────────────────
    uint64_t order = 0; ///< Mapping order for _NET_CLIENT_LIST
};

struct Workspace
{
    std::vector<xcb_window_t> windows;
    xcb_window_t focused_window = XCB_NONE;

    auto find_window(xcb_window_t id) { return std::ranges::find(windows, id); }

    auto find_window(xcb_window_t id) const { return std::ranges::find(windows, id); }
};

// ─────────────────────────────────────────────────────────────────────────────
// Monitor and workspace types
// ─────────────────────────────────────────────────────────────────────────────

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

// ─────────────────────────────────────────────────────────────────────────────
// Keybinding types
// ─────────────────────────────────────────────────────────────────────────────

struct KeyBinding
{
    uint16_t modifier;
    xcb_keysym_t keysym;

    auto operator<=>(KeyBinding const&) const = default;
};

struct Action
{
    std::string type;
    std::string command;
    int workspace = -1;
};

} // namespace lwm
