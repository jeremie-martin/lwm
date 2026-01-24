# LWM Window Manager - Complete Logic Specification

## Table of Contents
1. [Off-Screen Visibility Architecture](#off-screen-visibility-architecture)
2. [Architecture Overview](#architecture-overview)
3. [Data Structures](#data-structures)
4. [Window State Machine](#window-state-machine)
5. [Event Loop](#event-loop)
6. [Focus System](#focus-system)
7. [Workspace Management](#workspace-management)
8. [Layout Algorithm](#layout-algorithm)
9. [Floating Windows](#floating-windows)
10. [Drag Operations](#drag-operations)
11. [EWMH Protocol](#ewmh-protocol)
12. [ICCCM Compliance](#icccm-compliance)
13. [Configuration](#configuration)
14. [State Transitions](#state-transitions)
15. [Special Behaviors and Edge Cases](#special-behaviors-and-edge-cases)
16. [Invariants](#invariants)

---

## Key Terminology

**Off-screen**: The visibility model used by LWM. Windows are always mapped (XCB_MAP_STATE_VIEWABLE). Visibility is controlled by positioning - windows are either at their visible location or at OFF_SCREEN_X (-20000).

**Hidden** (client.hidden flag): Boolean flag that implements off-screen visibility. When true, the window is positioned at OFF_SCREEN_X. When false, the window is at its on-screen position.

**On-screen**: The opposite of off-screen. A window that is not positioned at OFF_SCREEN_X.

**Use consistent**: off-screen/on-screen for visibility state, hidden for the flag name.

---

## Architecture Overview

### Core Components

```
WindowManager (wm.cpp)
├── clients_ (unordered_map<window, Client>)  ← Authoritative state
├── monitors_ (vector<Monitor>)
├── floating_windows_ (vector<FloatingWindow>)
├── dock_windows_ (vector<window>)
├── desktop_windows_ (vector<window>)
└── Ewmh, KeybindManager, StatusBar, Layout
```

### Entry Points

**main()** → WindowManager construction → run() → Main event loop

**Initialization Sequence:**
1. setup_signal_handlers()
2. init_mousebinds()
3. create_wm_window()
4. setup_root() (SubstructureRedirect)
5. grab_buttons()
6. claim_wm_ownership() (WM_S0 selection)
7. intern_atoms()
8. window_rules_.load_rules()
9. layout_.set_sync_request_callback()
10. detect_monitors()
11. setup_ewmh()
12. setup_monitor_bars()
13. scan_existing_windows()
14. run_autostart()
15. keybinds_.grab_keys()
16. update_ewmh_client_list()
17. update_all_bars()

---

## Data Structures

### Client (src/lwm/core/types.hpp)

Authoritative source of truth for all window state.

```cpp
struct Client {
    xcb_window_t id = XCB_NONE;
    enum class Kind { Tiled, Floating, Dock, Desktop };
    Kind kind = Kind::Tiled;
    size_t monitor = 0;
    size_t workspace = 0;
    std::string name;
    std::string wm_class;
    std::string wm_class_name;

    // State flags (synced with _NET_WM_STATE)
    bool hidden = false;              // True when window is moved off-screen by WM
    bool fullscreen = false;
    bool above = false;
    bool below = false;
    bool iconic = false;
    bool sticky = false;
    bool maximized_horz = false;
    bool maximized_vert = false;
    bool shaded = false;
    bool modal = false;
    bool skip_taskbar = false;
    bool skip_pager = false;
    bool demands_attention = false;

    // Floating-specific
    Geometry floating_geometry;
    xcb_window_t transient_for = XCB_NONE;

    // Restore points
    std::optional<Geometry> fullscreen_restore;
    std::optional<Geometry> maximize_restore;
    std::optional<FullscreenMonitors> fullscreen_monitors;

    // Sync protocol
    uint32_t sync_counter = 0;
    uint64_t sync_value = 0;

    // Focus stealing prevention
    uint32_t user_time = 0;
    xcb_window_t user_time_window = XCB_NONE;

    // Management tracking
    uint64_t order = 0;              // Unique monotonically increasing value
};
```

### Workspace

```cpp
struct Workspace {
    std::vector<xcb_window_t> windows;  // Tiling order
    xcb_window_t focused_window = XCB_NONE;
};
```

### Monitor

```cpp
struct Monitor {
    xcb_randr_output_t output;
    std::string name;
    int16_t x, y;
    uint16_t width, height;
    std::vector<Workspace> workspaces;
    size_t current_workspace = 0;
    size_t previous_workspace = 0;
    xcb_window_t bar_window = XCB_NONE;
    Strut strut;
};
```

### FloatingWindow

```cpp
struct FloatingWindow {
    xcb_window_t id = XCB_NONE;
    Geometry geometry;  // Runtime geometry only
};
```

### DragState

```cpp
struct DragState {
    bool active = false;
    bool resizing = false;
    bool tiled = false;
    xcb_window_t window = XCB_NONE;
    int16_t start_root_x, start_root_y;
    int16_t last_root_x, last_root_y;
    Geometry start_geometry;
};
```

### BarState (Internal Status Bar)

```cpp
struct BarState {
    std::vector<bool> workspace_has_windows;
    std::string focused_title;
};
```

### Geometry Types

```cpp
struct Geometry {
    int16_t x = 0;
    int16_t y = 0;
    uint16_t width = 0;
    uint16_t height = 0;
};

struct Strut {
    uint32_t left, right, top, bottom;
};

struct FullscreenMonitors {
    uint32_t top, bottom, left, right;
};
```

### Bounds Checking and Validation

The WM performs extensive bounds checking to prevent invalid state:

**Workspace Index Bounds**:
- Workspace indices are clamped to valid range when switching
- `target_ws = std::min(target_ws, monitors_[target_mon].workspaces.size() - 1)`
- Prevents out-of-bounds access to workspaces array

**Monitor Index Validation**:
- Monitor lookups check `monitor_idx < monitors_.size()` before access
- Early returns for invalid monitor indices in various functions
- `update_floating_visibility()` checks monitor validity before updating

**Client State Validation**:
- Focus operations check `client.monitor < monitors_.size()` before proceeding
- Workspace operations validate `client.workspace < monitors_[client.monitor].workspaces.size()`
- Window removal checks for existence in `clients_` before manipulation

**EWMH Desktop Bounds**:
- `_NET_WM_DESKTOP` queries check bounds before returning
- `workspace_idx >= monitor.workspaces.size()` returns early without error

---

## Window State Machine

### Window Classification (EWMH)

```
MapRequest → classify_window()
├─ If already managed → deiconify_window() (remap request)
├─ If override-redirect → Ignore (menus, dropdowns)
├─ _NET_WM_WINDOW_TYPE_DESKTOP → Desktop
├─ _NET_WM_WINDOW_TYPE_DOCK → Dock
├─ _NET_WM_WINDOW_TYPE_NOTIFICATION/TOOLTIP/... → Popup (mapped directly, NOT MANAGED)
├─ Transient window → Floating
└─ Normal window → apply rules → Tiled/Floating
```

**Note**: Popup windows (menus, tooltips, notifications) are mapped directly but NOT managed. They bypass the state machine entirely.

**Initial Iconic State Detection**:
1. Check _NET_WM_STATE_HIDDEN property
2. If not set, check WM_HINTS.initial_state == IconicState
3. Precedence: _NET_WM_STATE_HIDDEN > WM_HINTS.initial_state

 ### Complete State Machine

```
                      ┌─────────────────────────────────────────┐
                      │           UNMANAGED                       │
                      └─────────────────┬───────────────────────┘
                                        │ MapRequest
                                        ▼
                      ┌─────────────────────────────────────────┐
                      │           MANAGED                         │
                      │  kind: Tiled/Floating/Dock/Desktop       │
                      │  WM_STATE: Normal/Iconic                 │
                      └─────────────────┬───────────────────────┘
                                        │
                      ┌─────────────────┼───────────────────────┐
                      │                 │                       │
            ┌─────────▼─────────┐  ┌──▼─────────┐
            │     VISIBLE       │  │   ICONIC    │
            │  (mapped,         │  │ (minimized) │
            │   hidden=false,    │  │ (mapped but  │
            │   iconic=false,   │  │   hidden=true,│
            │   on-screen)      │  │   iconic=true,│
            │                   │  │   off-screen) │
            │  State modifiers:  │  │              │
            │  • fullscreen     │  │              │
            │  • above/below    │  │              │
            │  • sticky         │  │              │
            │  • modal          │  │              │
            │  • maximized_*    │  │              │
            │  • shaded         │  │              │
            │  • skip_*         │  │              │
            │  • demands_attent. │  │              │
            └─────────┬─────────┘  └──┬─────────┘
                      │                │
                      └─────────────────┴────────────────────┘
                                        │
                      ┌─────────────────┼─────────────────────┐
                      │                 │                     │
                UnmapNotify    DestroyNotify    Unmanage (error)
                      │                 │                     │
                      └─────────────────┴─────────────────────┘
                                        ▼
                      ┌─────────────────────────────────────────┐
                      │           WITHDRAWN                      │
                      │     (WM_STATE = WithdrawnState)        │
                      └─────────────────────────────────────────┘

┌───────────────────────────────────────────────────────────────┐
│ Global Focus State                                             │
│                                                                │
│ NO_FOCUS: active_window_ = XCB_NONE (not a window state)      │
│                                                                │
│ Triggered when:                                                │
│   - Last window unmanaged and no replacement exists            │
│   - Pointer enters root window (empty space)                   │
│   - Entering showing_desktop mode                              │
│   - focus_or_fallback() finds no eligible candidates           │
│                                                                │
│ Note: Windows themselves have VISIBLE or ICONIC states;       │
│       NO_FOCUS is a global WindowManager condition.             │
└───────────────────────────────────────────────────────────────┘

┌───────────────────────────────────────────────────────────────┐
│ Interaction Modes                                              │
│                                                                │
│ NORMAL: Default interaction mode                               │
│ DRAG: Drag state active (drag_state_.active = true)           │
│   - Ignores EnterNotify events (focus-follows-mouse disabled)  │
│   - MotionNotify delegates to update_drag()                    │
│   - ButtonRelease calls end_drag()                            │
│   - Silently rejects: fullscreen windows, showing_desktop mode  │
│                                                                │
│ SHOWING_DESKTOP: Desktop mode enabled (showing_desktop_ = true)│
│   - Hides all non-sticky windows (off-screen)                  │
│   - Clears focus                                               │
│   - rearrange_monitor() returns early (no layout)              │
│   - Cannot start tiled drag operations                         │
└───────────────────────────────────────────────────────────────┘
 ```

**State Model Notes:**
- Top-level window states: UNMANAGED, MANAGED (with VISIBLE/ICONIC substates), WITHDRAWN (ICCCM WM_STATE)
- `hidden` is a physical state flag (positioned off-screen at OFF_SCREEN_X)
- `iconic` is a logical state flag (minimized by user, WM_STATE Iconic)
- **iconic=true always implies hidden=true** (minimized windows are off-screen)
- **hidden=true does NOT imply iconic=true** (workspace switches also hide windows)
- State modifiers apply to VISIBLE windows: fullscreen, above, below, sticky, maximized, shaded, modal
- `fullscreen` can combine with VISIBLE state; fullscreen windows are excluded from tiling
- Modal state is tightly coupled: `modal=true` ⇒ `above=true` (when modal is enabled, above is also enabled; when modal is disabled, above is also cleared)

**Visibility Model**: LWM uses off-screen visibility, not unmap/map cycles. Windows are always mapped (XCB_MAP_STATE_VIEWABLE). Visibility is controlled by:
- `hidden` flag: true = moved off-screen to OFF_SCREEN_X (-20000)
- `iconic` flag: true = minimized (iconify_window sets iconic=true AND calls hide_window() which sets hidden=true)
- Workspace assignment: windows on non-visible workspaces are hidden
- `showing_desktop_`: when true, hides all non-sticky windows (sticky windows remain visible)

**Off-Screen Constant**:
```cpp
constexpr int16_t OFF_SCREEN_X = -20000;
```

**Key Constants**:
```cpp
constexpr auto SYNC_WAIT_TIMEOUT = std::chrono::milliseconds(50);  // Sync request timeout
constexpr auto PING_TIMEOUT = std::chrono::seconds(5);            // Ping response window
constexpr auto KILL_TIMEOUT = std::chrono::seconds(5);             // Force-kill timeout

// WM_STATE values
constexpr uint32_t WM_STATE_WITHDRAWN = 0;
constexpr uint32_t WM_STATE_NORMAL = 1;
constexpr uint32_t WM_STATE_ICONIC = 3;
```

**hide_window(window)**: Sets client.hidden=true, moves window to OFF_SCREEN_X, preserves y coordinate. For sticky windows: early return without setting hidden flag or moving off-screen.

**show_window(window)**: Sets client.hidden=false only. Does NOT send configure events or restore position. Caller must call rearrange_monitor() (tiled) or apply_floating_geometry() (floating) to restore actual geometry.

### State Transitions

**→ MAPPED**
- Trigger: MapRequest (manage_window/manage_floating_window)
- Actions: Create Client, add to workspace, configure geometry, map window (xcb_map_window)

**↔ ICONIC**
- Trigger: iconify_window() / deiconify_window()
- Actions:
  - Set/clear iconic flag
  - WM_STATE Iconic/Normal
  - _NET_WM_STATE_HIDDEN
  - hide_window() / show_window() (off-screen, NOT unmap)
- Iconic windows remain mapped but are off-screen at OFF_SCREEN_X

**↔ FULLSCREEN**
- Trigger: set_fullscreen()
- Actions: Save/restore geometry, set/clear fullscreen flag, apply fullscreen geometry
- When enabling: Clears above/below (incompatible), clears maximized flags (superseded), preserves maximize_restore geometry for restoration
- When disabling: Restores geometry from fullscreen_restore

**↔ ABOVE/BELOW**
- Trigger: set_window_above() / set_window_below()
- Actions: Mutually exclusive, set flag, restack window

**↔ MAXIMIZED**
- Trigger: set_window_maximized(horiz, vert)
- Actions: Save/restore geometry, set flag
- For floating windows: Applies maximized geometry to working area
- For tiled windows: Sets flags and saves maximize_restore, but layout controls geometry (intentional state preservation for future floating conversion)
- Supports independent horiz/vert maximization

**↔ STICKY**
- Trigger: set_window_sticky()
- Actions: Set/clear sticky, _NET_WM_DESKTOP 0xFFFFFFFF/actual, update visibility

**↔ SHADED**
- Trigger: set_window_shaded()
- Actions: Set flag, iconify (LWM implements shade as iconify)
  - Enabling shade: Only calls iconify() if window is not already iconic
  - Disabling shade: Only calls deiconify() if window is currently iconic
  - Prevents redundant state changes

**→ WITHDRAWN**
- Trigger: UnmapNotify/DestroyNotify (unmanage_window)
- Actions: WM_STATE Withdrawn, erase from clients_, remove from workspace

 **↔ HIDDEN**
- Trigger: hide_window() / show_window()
- Actions:
  - Set/clear hidden flag
  - Move to/from OFF_SCREEN_X (preserves y coordinate)
  - Window remains mapped at all times
- Used for workspace visibility, showing desktop, iconification

**↔ SHOWING_DESKTOP**
- Trigger: _NET_SHOWING_DESKTOP client message or keybind
- Actions:
  - Enter mode: Hide all non-sticky windows (off-screen via hide_window()), clear_focus()
  - Exit mode: rearrange_all_monitors(), update_floating_visibility_all(), focus_or_fallback()
- Note: Sticky windows remain visible during showing_desktop mode
- Side effects: rearrange_monitor() returns early when showing_desktop_ is true (no layout)

**→ NO_FOCUS**
- Trigger: Last window unmanaged, workspace becomes empty, all windows iconic on current workspace, or pointer crosses to different monitor and lands on root window
- Actions: active_window_ = XCB_NONE, _NET_ACTIVE_WINDOW = XCB_NONE, clear_focus()
- Condition: All candidates in select_focus_candidate() return nullopt (no focus-eligible windows)

### State Conflicts

- **fullscreen ↔ above/below**: Incompatible, clear others when enabling fullscreen
- **fullscreen ↔ maximized**: Superseded, clear maximized flags when enabling fullscreen (but maximize_restore geometry is preserved for restoration)
- **above ↔ below**: Mutually exclusive, clear opposite when enabling
- **modal ⇔ above**: Modal state and above state are tightly coupled
   - When modal is enabled, above is also enabled (set_window_modal calls set_window_above(true))
   - When modal is disabled, above is also cleared (set_window_modal calls set_window_above(false))
   - This coupling is NOT bidirectional: disabling modal ALWAYS disables above (even if above was set independently before modal was enabled)
- **hidden vs iconic**: Iconic always implies hidden (iconic=true ⇒ hidden=true), but hidden can be true without iconic

---

 ## Startup and Initialization

### Window Management Initialization

**Initialization Sequence** (from main() → WindowManager construction → run()):
1. setup_signal_handlers()
2. init_mousebinds()
3. create_wm_window()
4. setup_root() (SubstructureRedirect)
5. grab_buttons()
6. claim_wm_ownership() (WM_S0 selection)
7. intern_atoms()
8. window_rules_.load_rules()
9. layout_.set_sync_request_callback()
10. detect_monitors()
11. setup_ewmh()
12. setup_monitor_bars()
13. scan_existing_windows()
14. run_autostart()
15. keybinds_.grab_keys()
16. update_ewmh_client_list()
17. update_all_bars()

### Scan Existing Windows

**scan_existing_windows()** - Restores windows from previous session:
1. Query root window tree for all top-level windows
2. Filter out override-redirect and non-viewable windows
3. For each window: classify → manage without suppressing focus
4. Determine pointer location using xcb_query_pointer
5. Set focused_monitor_ based on pointer position
6. Clear suppress_focus_ flag (was set to prevent focus changes during scanning)
7. Call focus_or_fallback() to restore focus on appropriate monitor

**suppress_focus_ flag**:
- Set before scan_existing_windows() to prevent spurious focus changes
- Cleared after scanning completes
- Prevents focus operations from taking effect during WM startup

### Initial Window State Detection

**Initial Iconic State Precedence**:
1. Check _NET_WM_STATE_HIDDEN property (takes precedence)
2. If not set, check WM_HINTS.initial_state == IconicState
3. Precedence: _NET_WM_STATE_HIDDEN > WM_HINTS.initial_state

---

## Event Loop

### Main Loop Structure

```
run()
├─ while (running_)
│  ├─ Calculate timeout from pending operations
│  ├─ poll() for X events or timeout
│  ├─ while (event = xcb_poll_for_event())
│  │  └─ handle_event(*event)
│  ├─ handle_timeouts()
│  │  ├─ Clean up expired pending_pings_ entries
│  │  └─ Force-kill windows with expired pending_kills_
│  └─ Check for X connection errors
│     └─ If error: LOG_ERROR and initiate shutdown
```

### Event Handlers (wm_events.cpp)

| Event | Handler | Effect |
|-------|---------|--------|
| MapRequest | handle_map_request | Classify and manage window. Handles deiconify if already managed |
| UnmapNotify | handle_window_removal | With off-screen visibility, ALL unmaps are client-initiated withdraw requests → unmanage (via handle_window_removal) |
| DestroyNotify | handle_window_removal | Unmanage window |
| EnterNotify | handle_enter_notify | Focus-follows-mouse. Ignores hidden windows. Updates focused_monitor on root window entry |
| MotionNotify | handle_motion_notify | Re-establish focus in window. Ignores hidden windows. Updates focused_monitor if crossing monitor |
| ButtonPress | handle_button_press | Focus window, begin drag. Ignores hidden windows. Updates focused_monitor |
| ButtonRelease | handle_button_release | End drag |
| KeyPress | handle_key_press | Execute keybind actions |
| KeyRelease | handle_key_release | Track for auto-repeat |
| ClientMessage | handle_client_message | EWMH/ICCCM commands |
 | ConfigureRequest | handle_configure_request | Geometry requests. Tiled: send synthetic ConfigureNotify. Floating: apply within size hints. Fullscreen: apply fullscreen geometry |
| PropertyNotify | handle_property_notify | State changes, title, hints, sync counter, struts, user_time_window indirection for _NET_WM_USER_TIME |
| Expose | handle_expose | Redraw status bars. Checks e.count != 0 to filter redundant events (only redraw on last one). Finds which monitor's bar window received the expose and calls bar_->update() with current window information. Only acts if bar_ is enabled. |
| SelectionClear | (handled in run) | New WM taking over, exit |
| RANDR change | handle_randr_screen_change | Monitor hotplug |

### Time Tracking

- `last_event_time_` - Updated from X event timestamps
- Used for focus stealing prevention (user_time comparison)
- Used for auto-repeat detection (same KeyPress/KeyRelease timestamp)

---

## Focus System

### Focus Eligibility

```
is_focus_eligible(window) =
    NOT (kind == Dock OR kind == Desktop)
    AND (accepts_input_focus OR supports_take_focus)
```

### Focus Assignment

**focus_window(window)** - Tiled windows
1. Check not showing_desktop, not iconic, is_focus_eligible
2. Deiconify if needed
3. Call focus_window_state() - may switch workspace
4. Clear all borders (set all windows to black pixel), set active border color on focused window
5. Send WM_TAKE_FOCUS if supported
6. Set X input focus
7. Update _NET_ACTIVE_WINDOW
8. Apply fullscreen geometry, restack transients
   - Restacks all visible, non-iconic transient windows (where client.transient_for == parent) above the focused window
   - Skips hidden, iconic, or transients on other workspaces
   - Also occurs when showing floating windows via update_floating_visibility()
9. Update bars

**focus_floating_window(window)** - Floating windows
1. Same checks as tiled (including NOT hidden)
2. Promote to end of floating_windows_ (MRU ordering)
3. Stack above (XCB_STACK_MODE_ABOVE)
4. Switch workspace if needed

**focus_or_fallback(monitor)** - Smart focus selection
1. Build candidates (order of preference):
    - `workspace.focused_window` if exists in workspace AND eligible (validated to exist)
    - Current workspace tiled windows (reverse iteration = MRU)
    - Sticky tiled windows from other workspaces (reverse iteration)
    - Floating windows visible on monitor (reverse iteration = MRU)
2. Call focus_policy::select_focus_candidate()
3. Focus selected or clear focus

### Focus Policy

**Focus-Follows-Mouse**
- EnterNotify on window → Focus if focus_eligible
- Filter: mode=NORMAL, detail≠INFERIOR
- MotionNotify within window → Re-establish focus if lost
- Hidden windows (client.hidden == true) are filtered out in EnterNotify, MotionNotify, and ButtonPress handlers
  - Prevents off-screen windows from receiving focus
  - Check occurs early, returns immediately for hidden windows
  - Note: is_focus_eligible() does NOT check client.hidden; filtering happens in event handlers

**Monitor Crossing**
- Pointer moves to different monitor → Set `focused_monitor_` to new monitor (via update_focused_monitor_at_point())
- `focused_monitor_` update does NOT automatically clear focus
- Focus is cleared only when pointer enters root window (empty space) or unmanaged area
- If pointer lands directly on a window on new monitor → that window becomes focused
- If pointer lands on empty space (root window) → Focus remains cleared until pointer enters a window

**Click-to-Focus**
- ButtonPress on window → Focus immediately

 **Focus Restoration**

Two-tier focus restoration model:

1. **Workspace focus memory** (tiled windows only):
   - Each Workspace.focused_window stores the last-focused tiled window
   - Only tiled windows are remembered per-workspace
   - This limitation is by design; floating window focus is not tracked per-workspace

2. **Global floating window search** (MRU order):
   - When workspace focus memory is not applicable, floating_windows_ is searched in reverse iteration (MRU)
   - Provides fallback focus restoration for floating windows

**Restoration scenarios**:
- Window unmanaged:
  - If removed window was on focused_monitor_ AND on current_workspace: focus_or_fallback()
  - Otherwise: clear_focus()
  - Before restoration, workspace.focused_window is updated to last window in workspace (or XCB_NONE if empty)
- Workspace switch: restore workspace.focused_window (may be stale/iconic, focus_or_fallback handles this)
- Fullscreen exit: No automatic focus restoration (window remains focused if it was active)
- Sticky toggle: No automatic focus restoration
- Monitor switch via pointer: No automatic restoration when focused_monitor_ updates
- Showing Desktop exit: rearrange_all_monitors(), update_floating_visibility_all(), focus_or_fallback()
- Focus fallback: MRU order (floating windows, then tiled)

**Focus Cycling** (focus_next / focus_prev)
- Uses `build_cycle_candidates()` to collect tiled and floating windows visible on current monitor/workspace
- Selection order: Tiled windows first, then floating windows (both in MRU order via reverse iteration)
- Wrap-around behavior: Cycles from last→first (next) and first→last (prev)
- Returns without focus if no candidates exist (nullopt)

**Focus Restoration**
- Window unmanaged:
  - If removed window was on focused_monitor_ AND on current_workspace: focus_or_fallback()
  - Otherwise: clear_focus()
- Workspace switch: restore workspace.focused_window (may be stale/iconic, focus_or_fallback handles this)
- Fullscreen exit: No automatic focus restoration (window remains focused if it was active)
- Sticky toggle: No automatic focus restoration
- Monitor switch via pointer: No automatic restoration when focused_monitor_ updates
- Focus fallback: MRU order (floating windows, then tiled)
- Showing Desktop exit: rearrange all, update floating visibility, focus_or_fallback()

 ### Focus Barriers Summary

All conditions that prevent focus:

1. **showing_desktop_ == true** - blocks all focus except desktop mode exit
   - focus_window() and focus_floating_window() return early without attempting to focus

2. **!is_focus_eligible(window)** - Dock/Desktop kinds or no input focus support
   - is_focus_eligible() checks: NOT (kind == Dock OR kind == Desktop) AND (accepts_input_focus OR supports_take_focus)

3. **client.hidden == true** - off-screen windows
   - Filtered in EnterNotify, MotionNotify, and ButtonPress handlers
   - is_focus_eligible() does NOT check client.hidden (design choice)
   - Prevents off-screen windows from receiving focus

4. **is_client_iconic(window)** - minimized windows
   - Deiconified before focusing (iconify reverses via deiconify)

5. **Workspace not visible (for non-sticky windows)** - triggers workspace switch on focus attempt
   - Focusing a window on a non-visible workspace automatically switches to that workspace (and monitor)
   - The 'workspace not visible' check in focus_barriers applies to focus candidates that would require workspace switches
   - Only visible windows can be focused without workspace switching

**Focus behavior across monitors**:
- Explicit monitor switch (keybind): Warps cursor if warp_cursor_on_monitor_change configured
- Automatic monitor switch (focus/window movement): Does NOT warp cursor
- Focusing a window on a different monitor via focus_window() implicitly switches the monitor (as a side effect of workspace switch)

**INFERIOR event filtering**:
- EnterNotify with detail=INFERIOR is filtered out
- INFERIOR means pointer entered a window because a child window closed, not because mouse moved
- This is a spurious event that should be ignored

### Focus Stealing Prevention

_NET_ACTIVE_WINDOW source=1 (application)
- Compare request timestamp with active_window's user_time
- If timestamp == 0 → Focus allowed (no timestamp check)
- If request older → set demands_attention instead of focusing
- Only newer timestamps can steal focus

**User Time Window Indirection**:
- `_NET_WM_USER_TIME` property may be on Client.user_time_window instead of main window
- PropertyNotify events are tracked on user_time_window during window management
- When _NET_WM_USER_TIME changes on user_time_window:
  - Finds parent Client that owns this user_time_window
  - Updates parent Client.user_time with the new value
- Focus stealing prevention uses parent Client.user_time for comparison
- _NET_WM_PING response may also come from user_time_window (data[2] = ping origin)

**_NET_ACTIVE_WINDOW source=2 (pager)**
- Always allowed (no timestamp check)

---

## Workspace Management

### Workspace Structure

```
Monitor
├─ workspaces: [Workspace, Workspace, ...]
├─ current_workspace: N
├─ previous_workspace: M
└─ bar_window: (optional)

Workspace
├─ windows: [window1, window2, ...]  // Tiling order
└─ focused_window: windowX  // Last-focused
```

### Desktop Mapping

```
desktop_index = monitor_idx * workspaces_per_monitor + workspace_idx

_NET_CURRENT_DESKTOP = active_monitor's current workspace
_NET_NUMBER_OF_DESKTOPS = monitors × workspaces_per_monitor
_NET_DESKTOP_VIEWPORT = Each workspace's monitor origin
```

### Previous Workspace Tracking

`monitor.previous_workspace` is per-monitor, not global. Updated on:

1. **Explicit workspace switch**: `workspace_policy::apply_workspace_switch()` sets to old workspace
2. **Focus-induced workspace switch**: `focus_window_state()` sets to old workspace when focus triggers switch

Used for `toggle_workspace()` behavior to restore previous workspace. Note that `previous_workspace` is independent per monitor.

 ### Workspace Switch

```
switch_workspace(target_ws)
├─ Validate workspace index
├─ workspace_policy::apply_workspace_switch()
│  ├─ Update previous_workspace = current
│  └─ Update current_workspace = target
├─ hide_window() for floating windows (old workspace, non-sticky)
├─ hide_window() for tiled windows (old workspace, non-sticky)
├─ conn_.flush()  ← Critical sync point!
├─ update_ewmh_current_desktop()
├─ rearrange_monitor(new workspace)
│  ├─ For each visible window: show_window() (clears hidden flag)
│  └─ layout_.arrange() (applies on-screen geometry)
├─ update_floating_visibility(new workspace)
├─ focus_or_fallback(monitor)
└─ Update bars
```

**Critical Ordering**:
1. Hide old workspace windows (floating first, then tiled)
2. Flush X connection to ensure hide operations complete
3. Show new workspace via rearrange_monitor and update_floating_visibility

The flush prevents old floating windows from briefly appearing over new workspace content due to X11 async rendering.

**Off-Screen Visibility**: Windows are hidden using hide_window(), NOT unmapped.

 **Tiled Window Visibility Restoration**:
- hide_window() moves tiled windows off-screen (sets client.hidden = true, moves to OFF_SCREEN_X)
- rearrange_monitor() restores visibility by:
  1. Explicitly calls show_window() for each visible window (clears hidden flag)
  2. Calculates geometry for all windows in the new workspace
  3. Configures each window to its calculated geometry (which is on-screen)
- show_window() is called for tiled windows before layout_.arrange(), not implicitly via layout
- For floating windows: update_floating_visibility() calls show_window()/hide_window() explicitly

 ### Workspace Toggle

```
toggle_workspace()
├─ Check workspace count <= 1 → Return (no toggle with only 1 workspace)
├─ Check previous_workspace invalid or same as current → Return
├─ Auto-repeat detection (same keysym, same timestamp as KeyRelease)
├─ target = monitor.previous_workspace
└─ switch_workspace(target)
```

**Early Return Conditions**:
- Workspace count <= 1: No toggle possible with only one workspace
- previous_workspace invalid: After monitor hotplug or workspace configuration changes, previous_workspace may be out-of-bounds. Returns without switching (same as 'same as current' case).
- previous_workspace == current: Already on previous workspace, no action needed

**Auto-Repeat Prevention**:
- X11 auto-repeat: KeyRelease → KeyPress (same timestamp)
- Blocks KeyPress if same keysym && same timestamp as KeyRelease
- Prevents rapid workspace toggling from key hold

**Note**: toggle_workspace() does NOT update previous_workspace when switching back (it's the target of the toggle, so the next toggle switches to the other workspace).

### Move Window to Workspace

**Tiled windows**
```
move_window_to_workspace(target_ws)
├─ workspace_policy::move_tiled_window()
│  ├─ Remove from source workspace.windows
│  ├─ Add to target workspace.windows
│  └─ Update source/target focused_window
├─ Update Client.monitor, Client.workspace
├─ Set _NET_WM_DESKTOP
├─ hide_window() if target not visible
├─ Rearrange affected monitor(s)
├─ Focus fallback in source workspace
└─ Update bars
```

**Floating windows**
- Update Client.monitor, Client.workspace
- Reposition to target monitor's working area
- Update visibility on both monitors
- Focus moved window if visible
- Note: If floating window crosses monitor boundary via geometry update (not explicit move), `update_floating_monitor_for_geometry()` auto-assigns to new monitor without focus restoration

### Sticky Windows

- Visible on ALL workspaces of owning monitor (not global)
- NOT hidden during workspace switch
- hide_window() skips sticky windows (early return, does not set hidden flag or move off-screen)
- Included in tiling layout on all workspaces (appears in all Workspace::windows vectors)
- _NET_WM_DESKTOP = 0xFFFFFFFF
- Focusing sticky window does NOT switch workspaces
 - Sticky windows remain visible during showing_desktop mode

---

## Monitor Behavior

### Monitor Hotplug (RANDR Screen Change)

**RANDR screen change event** triggers comprehensive reconfiguration:

1. **Exit fullscreen on all windows** (before reconfiguring)
   - Prevents stale geometry in Client.fullscreen_restore after monitor layout changes
   - Clear Client.fullscreen_monitors (monitor indices may have changed)

2. **Save window locations by monitor name**
   - Save WindowLocation for each client (includes monitor_name, not index)
   - Preserve floating window geometries (no repositioning)
   - Save focused_monitor_name

3. **Destroy status bars** (old bar windows)

4. **Detect new monitor configuration**
   - detect_monitors() rebuilds monitors_ vector

5. **Restore window locations**
   - Build name_to_index map for new monitor configuration
   - Reassign windows to monitors with matching names
   - Fallback to monitor 0 if original monitor no longer exists
   - Restore focused_monitor_ by name lookup

6. **Clamp workspace indices**
   - If target workspace >= workspaces.size(), fall back to current workspace
   - Prevents out-of-bounds workspace access after configuration change

7. **Recreate status bars** for new monitor configuration

8. **Update all EWMH properties**
   - _NET_NUMBER_OF_DESKTOPS
   - _NET_DESKTOP_NAMES
   - _NET_DESKTOP_VIEWPORT
   - _NET_WORKAREA
   - _NET_DESKTOP_GEOMETRY

### Monitor Switch Behavior

**Explicit monitor switch** (via keybind: focus_monitor_left/right):
- Switch to target monitor (update focused_monitor_)
- Warp cursor to center of new monitor if warp_cursor_on_monitor_change configured
- No automatic focus restoration

**Automatic monitor switch** (via focus or window movement):
- Focusing a window on different monitor implicitly changes focused_monitor_
- Moving a window to different monitor updates focused_monitor_ if window was active
- Does NOT warp cursor (warping only happens for explicit switch)

### Monitor Assignment for Floating Windows

**Explicit monitor move** (via keybind: move_to_monitor_left/right):
- Update Client.monitor and Client.workspace
- Reposition to target monitor's working area
- Update visibility on both monitors
- Focus moved window if visible
- Update _NET_WM_DESKTOP

**Implicit monitor assignment** (via geometry change):
- update_floating_monitor_for_geometry() called when floating window center crosses monitor boundary
- Auto-assign to new monitor
- Update Client.workspace to new monitor's current_workspace
- Update _NET_WM_DESKTOP
- **NO focus restoration** (different from explicit move)
- Visibility must be manually updated via update_floating_visibility()

---

## Layout Algorithm

### Master-Stack Tiling (layout.cpp)

**Configuration**
- `padding` - Gap around all windows
- `border_width` - Window border thickness
- `status_bar_height` - Internal bar height

**Layout Cases**

| Windows | Layout |
|---------|--------|
| 0 | Empty |
| 1 | Single window fills usable area |
| 2 | Two windows side-by-side, equal width of available space |
| 3+ | Master on left (50%), stack on right (50%) |

**Master-Stack Calculation**

Note: Borders are drawn outside client area in X11, so layout calculations account for `border_width`.

```
working_area = monitor.geometry - struts - (bar ? status_bar_height : 0)
total_borders = 4 * border
slot_width = (working_area.width - 3*padding - total_borders) / 2

Master:
  x = working_area.x + padding + border
  y = working_area.y + padding + border
  width = slot_width
  height = working_area.height - 2*padding - 2*border

Stack (N windows):
  total_v_borders = N * 2 * border
  stack_avail_height = working_area.height - (N+1)*padding - total_v_borders
  stack_slot_height = stack_avail_height / N
  x = working_area.x + slot_width + 2*padding + border
  y = working_area.y + padding + border + i * stack_slot_height
  width = slot_width
  height = stack_slot_height - border
```

**Minimum Dimensions**
- `MIN_DIM = 50` pixels enforced as lower bound for available space calculations

 ### Size Hints

**Tiled windows**: ALL size hints ignored
- Size hints are NEVER queried (window parameter unused in apply_size_hints, explicitly cast to void)
- WM controls geometry completely
- Applications must handle smaller sizes (scrolling, truncating, adapting)
- Matches other tiling WMs (DWM, i3, bspwm)
- Only clamp to minimum 1 pixel to prevent zero-size

**Floating windows**: Preferred size hints honored, constraints NOT enforced
- Position hints (P_POSITION, US_POSITION) honored for initial placement
- Preferred size (P_SIZE, US_SIZE) honored for initial geometry
- min_width/min_height NOT enforced
- max_width/max_height NOT enforced
- Only clamp to minimum 1 pixel to prevent zero-size
- Zero-sized windows fall back to 300x200 default

 **Size Hints on Tiled Windows During Changes**:
- When WM_NORMAL_HINTS change on tiled windows, trigger rearrange_monitor()
- Even though size hints are ignored, WM must reaffirm its control
- Applications may request geometry changes; WM reapplies its geometry

### Internal Bar

**Internal Bar** (optional, configurable via enable_internal_bar):
- Height: appearance.status_bar_height (from config)
- Applied during layout calculation in calculate_slots()
- Condition: has_internal_bar ? appearance.status_bar_height : 0
- External docks (Polybar, etc.) are handled via struts, not internal bar height

**Bar State Building**:
- build_bar_state() for each monitor:
  - workspace_has_windows flags for all workspaces
  - Includes floating windows in workspace tracking
  - Determines focused_title based on active window or workspace.focused_window
- Updated on: manage, unmanage, focus change, workspace switch, title change

**Expose Event Handling**:
- Only acts on last expose (e.count == 0) to filter redundant events
- Finds which monitor's bar window received the event
- Calls bar_->update() with current window information for that specific monitor
- Only acts if bar_ is enabled
- Other monitors' bars are unaffected

---

## Floating Windows

### Floating Window Management

**manage_floating_window()**
1. Determine placement:
   - Position hints from ICCCM WM_NORMAL_HINTS
   - Centered on parent for transients
   - Centered on monitor otherwise
2. Create FloatingWindow record
3. Create Client record (kind = Floating)
4. Set WM_STATE (Normal/Iconic)
5. Apply geometry states (fullscreen, maximized)
6. xcb_map_window() (always mapped)
7. If not visible: hide_window() (move off-screen)
8. Apply non-geometry states (above, below, skip_*)
9. Focus if appropriate (respects suppress_focus_ flag)

### Floating Visibility

```
is_floating_visible(window) =
    NOT client.hidden
    AND NOT showing_desktop
    AND NOT iconic
    AND (sticky OR workspace == current_workspace)
```

**Note**: This is the conceptual visibility condition. In code, update_floating_visibility() explicitly sets client.hidden based on this formula and applies geometry. The function checks showing_desktop separately and returns early if true.

### Floating Geometry

- **Client.floating_geometry** is authoritative (persistent state)
- **FloatingWindow.geometry** is runtime state (transient, for layout calculations)
- These are always updated together via apply_floating_geometry(), which:
  1. Reads from Client.floating_geometry (accounting for fullscreen/maximized overrides)
  2. Updates FloatingWindow.geometry
  3. Configures the X window to match

### Floating Restack

- MRU ordering in floating_windows_ vector
- Promoted to end when focused
- _NET_CLIENT_LIST_STACKING reflects MRU order

---

## Drag Operations

 ### Drag State Machine (wm_drag.cpp)

```
[ButtonRelease] OR [_NET_WM_MOVERESIZE direction=11 (cancel)]
    ↓
end_drag()
    ├─ If tiled:
    │  ├─ Silent abort conditions:
    │  │  ├─ If monitor_index_for_window() lookup fails → Abort
    │  │  ├─ If workspace_index_for_window() lookup fails → Abort
    │  │  ├─ If monitors_.empty() → Abort
    │  │  └─ If client unmanaged → Abort
    │  ├─ Determine target monitor from pointer
    │  ├─ If target workspace empty → Insert at position 0
    │  ├─ Else → Insert at nearest slot (drop_target_index)
    │  ├─ Rearrange monitors
    │  └─ Focus moved window
    └─ Ungrab pointer
```

**Silent Abort Conditions** (end_drag()):
- Source monitor or workspace lookup fails (window state corrupted)
- Client has been unmanaged during drag
- monitors_.empty() (all monitors disconnected)

**Drag Rejection Conditions** (begin_drag() / begin_tiled_drag()):
- Fullscreen windows (is_client_fullscreen returns true)
- For tiled drag only: showing_desktop mode active
- For tiled drag only: window is floating (has FloatingWindow record)
- For tiled drag only: monitors_.empty()

**Tiled window drag**: Window follows cursor visually (temporary geometry), tiling layout NOT recalculated until drop
- Drop target: nearest slot center (Euclidean distance)
- Insertion position: layout_policy::drop_target_index()
- Empty workspace target: insertion defaults to position 0 (beginning)
- Same workspace drop: reorders within current workspace
- Cross-workspace drop: moves window to target workspace, updates Client.monitor and Client.workspace

### Tiled Window Drag (Reorder Mode)

- Window follows cursor visually (temporary geometry)
- Tiling layout NOT recalculated until drop
- Drop target: nearest slot center (Euclidean distance)
- Insertion position: `layout_policy::drop_target_index()`
- Empty workspace target: If target workspace has no windows, insertion defaults to position 0 (beginning)

### Floating Window Drag

**Move**: Update x/y, apply within working area

**Resize**:
- Update width/height with size hints
- Constrain to working area
- Apply geometry immediately

---

## EWMH Protocol

### Root Window Properties

| Property | Value | Update Timing |
|----------|-------|---------------|
| _NET_SUPPORTED | List of all supported atoms | Startup |
| _NET_SUPPORTING_WM_CHECK | WM's own window | Startup |
| _NET_WM_NAME | "LWM" | Startup |
| _NET_NUMBER_OF_DESKTOPS | monitors × workspaces_per_monitor | Monitor change |
| _NET_DESKTOP_NAMES | Null-separated workspace names | Monitor change |
| _NET_CURRENT_DESKTOP | Active monitor's current workspace | Workspace switch |
| _NET_ACTIVE_WINDOW | Currently focused window | Focus change |
| _NET_CLIENT_LIST | Managed windows (map order) | Manage/unmanage |
| _NET_CLIENT_LIST_STACKING | Stacking order | Focus/restack |
| _NET_WM_DESKTOP | Per-window desktop | Manage/move/sticky |
| _NET_DESKTOP_VIEWPORT | Monitor origins per desktop | Monitor change |
| _NET_DESKTOP_GEOMETRY | Total desktop dimensions | Monitor change |
| _NET_WORKAREA | Usable area minus struts | Strut change |
| _NET_FRAME_EXTENTS | (0,0,0,0) - no decorations | Query |
| _NET_WM_ALLOWED_ACTIONS | Per-window supported operations | Manage |
| _NET_SHOWING_DESKTOP | Desktop mode toggle | Toggle |

### Window State Atoms

All fully implemented:

| State | Atom | Behavior |
|-------|------|----------|
| Fullscreen | _NET_WM_STATE_FULLSCREEN | Full monitor geometry |
| Above | _NET_WM_STATE_ABOVE | Stack above all |
| Below | _NET_WM_STATE_BELOW | Stack below all |
| Hidden | _NET_WM_STATE_HIDDEN | Iconified (WM_STATE Iconic) |
| Sticky | _NET_WM_STATE_STICKY | Visible on all workspaces |
| Maximized Horz | _NET_WM_STATE_MAXIMIZED_HORZ | Full width (floating) |
| Maximized Vert | _NET_WM_STATE_MAXIMIZED_VERT | Full height (floating) |
| Shaded | _NET_WM_STATE_SHADED | Implemented as iconify |
| Modal | _NET_WM_STATE_MODAL | Modal dialog flag |
| Skip Taskbar | _NET_WM_STATE_SKIP_TASKBAR | Exclude from taskbar |
| Skip Pager | _NET_WM_STATE_SKIP_PAGER | Exclude from pager |
| Focused | _NET_WM_STATE_FOCUSED | Currently focused (WM-set) |
| Demands Attention | _NET_WM_STATE_DEMANDS_ATTENTION | Urgency indicator |

### Window Types

Classification priority:

1. **DESKTOP** → Kind::Desktop (below all, not focusable)
2. **DOCK** → Kind::Dock (reserves struts, not focusable)
3. **TOOLBAR/MENU/SPLASH** → Kind::Floating (skip_taskbar, skip_pager)
4. **UTILITY** → Kind::Floating (skip_taskbar, skip_pager, above)
5. **DIALOG** → Kind::Floating
6. **DROPDOWN_MENU/POPUP_MENU/TOOLTIP/NOTIFICATION/COMBO/DND** → Kind::Popup (mapped directly, not managed)
7. **NORMAL** → Kind::Tiled (or Floating if transient/rule)

### Client Message Handlers

| Message | Behavior |
|---------|----------|
| _NET_ACTIVE_WINDOW | Focus window, switch desktop if needed. Focus stealing prevention via user_time (source=1) or allow (source=2). If timestamp == 0, always allowed |
| _NET_CLOSE_WINDOW | Send WM_DELETE_WINDOW or force kill after timeout |
| _NET_WM_STATE | ADD/REMOVE/TOGGLE state atoms (data[1], data[2]) |
| _NET_CURRENT_DESKTOP | Switch to desktop (monitor/workspace). If invalid monitor index, silently ignored |
| _NET_WM_DESKTOP | Move window to desktop (0xFFFFFFFF = sticky) |
| _NET_FULLSCREEN_MONITORS | Store monitor bounds, apply if fullscreen |
| _NET_REQUEST_FRAME_EXTENTS | Return (0,0,0,0) |
| _NET_MOVERESIZE_WINDOW | Programmatic move/resize (floating only). Flags: bits 8-11=gravity, 12=x, 13=y, 14=width, 15=height |
| _NET_MOVERESIZE | Interactive move/resize (floating only). Direction: 8=move, 11=cancel, 0-7=resize edges. Cancel (11) calls end_drag() directly |
| _NET_RESTACK_WINDOW | Restack relative to sibling (above/below) |
| _NET_SHOWING_DESKTOP | Toggle desktop mode (hide all/restore using hide_window) |
| WM_PROTOCOLS:_NET_WM_PING | Response → cancel pending kill (window alive). May come from user_time_window (data[2] = ping origin) |
| WM_PROTOCOLS:WM_DELETE_WINDOW | Graceful close request |
| WM_PROTOCOLS:WM_TAKE_FOCUS | Used for input=False windows |
| WM_CHANGE_STATE | Iconify/Deiconify (IconicState/NormalState) |

 ### Ping-Kill Protocol

```
kill_window()
├─ Check for WM_DELETE_WINDOW support
├─ If supported:
│  ├─ Send WM_DELETE_WINDOW client message (graceful close)
│  ├─ Send _NET_WM_PING
│  ├─ Schedule pending kill (5 second timeout)
│  ├─ If ping response → cancel pending kill (window alive)
│  └─ If timeout → xcb_kill_client() force kill
└─ If NOT supported:
   └─ Force kill immediately (no graceful close)

handle_timeouts()
├─ Clean up expired pending_pings_ entries (remove old entries)
├─ Force-kill windows with expired pending_kills_
└─ xcb_kill_client() for unresponsive windows
```

**Constants**:
- PING_TIMEOUT = 5 seconds
- KILL_TIMEOUT = 5 seconds

**User Time Window Handling**:
- `_NET_WM_USER_TIME` property may be on `Client.user_time_window` instead of main window
- WM selects PROPERTY_CHANGE on user_time_window during window management
- PropertyNotify events on user_time_window are tracked during window management
- When _NET_WM_USER_TIME changes on user_time_window:
  - Finds parent Client that owns this user_time_window
  - Updates parent Client.user_time with the new value
- If PropertyNotify arrives on user_time_window that is not associated with any client, update is silently ignored
- Focus stealing prevention uses parent Client.user_time for comparison
- _NET_WM_PING response may also come from user_time_window:
  - data[2] = ping origin (original window)
  - WM identifies which pending ping/kill to cancel by matching data[2]

 ### Sync Request (Fire-and-Forget)

- Sent before WM-initiated resizes
- Non-blocking: does NOT wait for client update
- Client::sync_value incremented
- Split 64-bit value into two 32-bit data fields for X11 protocol
- Prevents event loop blocking
- Implementation tries to wait for sync counter with 50ms timeout:
  - wait_for_sync_counter() attempts to read updated counter value
  - If client doesn't respond within 50ms, operation proceeds anyway (non-blocking fallback)
  - Compromise: most clients respond quickly, but hung clients don't block event loop

---

## ICCCM Compliance

### Selection & Ownership

- Acquire `WM_S0` selection on `WM_Sn` atom
- Fail if another WM already owns selection
- Broadcast `MANAGER` client message after acquiring
- Respond to `SelectionClear` by exiting
- Select `SubstructureRedirectMask` on root

### WM_STATE Management

| State | Value | When Set |
|-------|--------|-----------|
| Withdrawn | 0 | On unmanage_window() |
| Normal | 1 | On manage/map (non-iconic), deiconify_window() |
| Iconic | 3 | On iconify_window(), WM_HINTS.initial_state=Iconic |

**Critical**: WM_STATE NOT changed for workspace visibility or showing_desktop mode. Only iconification/deiconification changes WM_STATE (set to Normal or Iconic on management).

### Unmap Tracking

**Off-Screen Visibility Model**: LWM does NOT use unmap/map for visibility control. Windows are always mapped (XCB_MAP_STATE_VIEWABLE). Visibility is controlled by moving windows off-screen to OFF_SCREEN_X (-20000), chosen as a safe value unlikely to intersect with any reasonable monitor configuration.

- All managed windows remain in XCB_MAP_STATE_VIEWABLE state at all times
- Visibility is controlled by moving windows off-screen (OFF_SCREEN_X = -20000)
- `hide_window()` and `show_window()` manage off-screen positioning
- No ICCCM unmap tracking is needed

**UnmapNotify Handling**: With off-screen visibility, ANY UnmapNotify is treated as a client-initiated withdraw request → unmanage_window(). No counter tracking exists.

### Properties Read & Honored

| Property | Behavior |
|----------|----------|
| WM_NAME / _NET_WM_NAME | Window title |
| WM_CLASS | Window identification, rule matching |
| WM_CLIENT_MACHINE | Display/session management |
| WM_NORMAL_HINTS | Position (P_POSITION/US_POSITION) honored for initial placement. Size (P_SIZE/US_SIZE) honored for initial geometry. min/max NOT enforced. inc/aspect/gravity ignored. Tiled: triggers monitor rearrange on change (hints are ignored but rearrangement occurs) |
| WM_HINTS | input (focus eligibility), initial_state (iconic), urgency (demands_attention) |
| WM_TRANSIENT_FOR | Transient relationship, inherit workspace, auto skip_*, stack above parent |
| WM_PROTOCOLS | DELETE_WINDOW, TAKE_FOCUS, _NET_WM_PING, _NET_WM_SYNC_REQUEST |
| _NET_WM_USER_TIME | User time for focus stealing prevention. May be on user_time_window (indirect) |
| _NET_WM_STATE_HIDDEN | Initial iconic state check (takes precedence over WM_HINTS.initial_state) |

 ### Properties Written

| Property | Timing |
|----------|--------|
| WM_STATE | Manage, unmanage, iconify, deiconify |
| ConfigureNotify (synthetic) | After layout, fullscreen, floating geometry changes |
| _NET_WM_STATE | All state flag changes |
| _NET_WM_DESKTOP | Manage, move to workspace, sticky toggle, monitor assignment |
| _NET_CLIENT_LIST | Manage, unmanage |
| _NET_CLIENT_LIST_STACKING | Focus change, restack |
| _NET_ACTIVE_WINDOW | Focus change |
| _NET_WORKAREA | Strut change |
| _NET_FULLSCREEN_MONITORS | Multi-monitor fullscreen |

**Synthetic ConfigureNotify**:
- Sent after layout_.arrange() to inform clients of their geometry
- Sent after fullscreen transition
- Sent after floating geometry changes
- Critical for Electron/Chrome apps that need to know their geometry immediately
- Construction: event_type=ConfigureNotify, synthetic=true, geometry from WM

**WM_PROTOCOLS Selection on Client Windows**:
- Event mask: XCB_EVENT_MASK_ENTER_WINDOW | XCB_EVENT_MASK_FOCUS_CHANGE | XCB_EVENT_MASK_PROPERTY_CHANGE
- Floating windows additionally select: XCB_EVENT_MASK_BUTTON_PRESS
- Note: STRUCTURE_NOTIFY is NOT selected on client windows
  - Selecting STRUCTURE_NOTIFY causes duplicate UnmapNotify events
  - Duplicate events would break off-screen visibility model (would incorrectly unmanage windows during workspace switches)

### Window Lifecycle Events

| Event | Handling |
|-------|-----------|
| MapRequest | If already managed: deiconify_window(). Else: Classify → manage/floating/dock/desktop/popup. Apply rules. Honor EWMH states |
| UnmapNotify | With off-screen visibility, treat as client-initiated withdraw → unmanage_window(). No counter check |
| DestroyNotify | Remove from all structures, restore focus if was active |
| ConfigureRequest | Tiled: synthetic ConfigureNotify. Floating: apply within hints. Fullscreen: fullscreen geometry |
| PropertyNotify | Update state, title, hints, sync counter, struts. Handle user_time_window indirection for _NET_WM_USER_TIME |

**See also**: State Transitions → Window Lifecycle (lines 11226) for detailed sequence diagrams

---

## Configuration

### Configuration Structure

**[appearance]** - Visual settings
- `padding` - Gap around all windows
- `border_width` - Window border thickness
- `border_color` - Active window border (hex)
- `status_bar_height` - Internal bar height (pixels)
- `status_bar_color` - Bar background color (hex, default: 0x808080)
- `enable_internal_bar` - Enable/disable internal bar

**[focus]** - Focus behavior
- `warp_cursor_on_monitor_change` - Warp cursor to monitor center on explicit switch (bool)

**[programs]** - External programs
- `terminal` - Terminal emulator path
- `browser` - Web browser path
- `launcher` - Application launcher path

**[workspaces]** - Workspace configuration
- `count` - Number of workspaces per monitor
- `names` - Workspace names (auto-normalized)

**[autostart]** - Startup programs
- `commands` - Array of commands to execute on startup

**[[keybinds]]** - Keyboard shortcuts
- `mod` - Modifier key (super, super+shift, super+ctrl)
- `key` - Key to bind (1-0 keys support AZERTY and QWERTY)
- `action` - Action to execute
- `command` - Program path (for spawn action)
- `workspace` - Workspace number (for workspace actions)

**[[mousebinds]]** - Mouse bindings
- `mod` - Modifier key
- `button` - Mouse button (1=left, 3=right)
- `action` - drag_window or resize_floating

**[[rules]]** - Window classification rules
- `class` - WM_CLASS class name pattern (regex)
- `instance` - WM_CLASS instance name pattern (regex)
- `title` - Window title pattern (regex)
- `type` - EWMH type (normal/dialog/utility/toolbar/splash/menu)
- `transient` - Match only transients (bool)
- Actions (applied if criteria match):
    - `floating` - Force floating or tiled
    - `workspace` / `workspace_name` - Assign to workspace
    - `monitor` / `monitor_name` - Assign to monitor
    - `fullscreen` - Start fullscreen
    - `above` / `below` - Set stacking order
    - `sticky` - Visible on all workspaces
    - `skip_taskbar` / `skip_pager` - Exclude from UI
     - `geometry` - {x, y, width, height} for floating
     - `center` - Center floating window

### Window Rules Engine

**Matching** (AND logic)
- All specified criteria must match
- First match wins (stop after finding)
- Empty criteria matches all

**Criteria**
- `class` - WM_CLASS class name (regex)
- `instance` - WM_CLASS instance name (regex)
- `title` - Window title (regex)
- `type` - EWMH type (normal/dialog/utility/toolbar/splash/menu)
- `transient` - Match only transients (bool)

**Regex Compilation and Fallback**
- Patterns compiled to regex at startup
- Invalid patterns fall back to literal string matching
- Warning logged to stderr for invalid regex
- Ensures rules always work, even with invalid patterns

**Actions**
- `floating` - Force floating or tiled
- `workspace` / `workspace_name` - Assign workspace
- `monitor` / `monitor_name` - Assign monitor
- `fullscreen` - Start fullscreen
- `above` / `below` - Stacking order
- `sticky` - Visible on all workspaces
- `skip_taskbar` / `skip_pager` - Exclude from UI
- `geometry` - {x, y, width, height} for floating
- `center` - Center floating window

**Application Timing**: AFTER EWMH classification, BEFORE management. Cannot override DOCK, DESKTOP, or POPUP types (EWMH protocol protected).

 **Window Rule Application Order**:
1. EWMH type classification (classify_window)
2. Initial _NET_WM_STATE read from window properties
3. Window rules matching and application (can override EWMH states except Dock/Desktop/Popup)
4. Geometry application (rule geometry takes precedence)
   - Rule geometry (if specified) → Applied BEFORE mapping
   - Otherwise → Placement logic (center on monitor or relative to parent)
   - Position hints honored for initial placement (P_POSITION, US_POSITION)
   - Preferred size hints honored for initial geometry (P_SIZE, US_SIZE)
5. Final state flag application (fullscreen, above, below, skip_*)
6. Window mapping (xcb_map_window)
7. Visibility control (hide_window if not visible)

**Geometry Precedence**:
1. Rule geometry (highest priority)
2. Fullscreen geometry (if fullscreen enabled)
3. Maximized geometry (if maximized enabled and not fullscreen)
4. Placement logic / hints (lowest priority)

**Rule State Precedence**:
- Window rules CAN override initial _NET_WM_STATE (except for Dock/Desktop/Popup types)
- Rule states are applied after reading initial EWMH atoms
- Rule actions like `fullscreen`, `above`, `below` are applied AFTER initial state read
- Rule geometry (`geometry` or `center`) is applied BEFORE mapping

### Supported Keybind Actions

| Action | Description | Parameters |
|---------|-------------|------------|
| spawn | Launch program | command |
| kill | Close window | None |
| switch_workspace | Switch workspace | workspace (0-9) |
| toggle_workspace | Switch to previous | None |
| move_to_workspace | Move window to workspace | workspace (0-9) |
| focus_monitor_left/right | Switch active monitor | None |
| move_to_monitor_left/right | Move window to monitor | None |
| toggle_fullscreen | Toggle fullscreen | None |
| focus_next/prev | Cycle focus | None |

**Modifiers**: super, super+shift, super+ctrl

### Workspace Normalization

- names only → count = names.size
- count only → names = "1".."count"
- names < count → pad with numeric names
- names > count → truncate
- Minimum count = 1

---

## State Transitions

 ### Window Lifecycle

```
MapRequest
    ↓
If already managed:
    ├─ Determine focus = (window.monitor == focused_monitor_ && window.workspace == current_workspace)
    ├─ deiconify_window(window, focus)
    └─ Return (deiconify only, do not remanage)
    ↓
If override-redirect → Ignore (menus, dropdowns bypass state machine)
    ↓
classify_window() → Kind (Tiled/Floating/Dock/Desktop/Popup)
    ├─ If Popup (menus, tooltips, notifications): Map directly, NOT MANAGED, return
    └─ Continue for Tiled/Floating/Dock/Desktop
    ↓
apply_window_rules()
    ├─ Can override EWMH states except Dock/Desktop/Popup types
    └─ Applies rule geometry or uses placement logic
    ↓
Create Client record (order = next_client_order_++)
    ↓
Read initial EWMH state (precedence: _NET_WM_STATE_HIDDEN > WM_HINTS.initial_state)
    ├─ Check _NET_WM_STATE_HIDDEN → start_iconic
    └─ If not set, check WM_HINTS.initial_state → start_iconic
    ↓
If Floating: Create FloatingWindow, determine placement
    ↓
Add to workspace.windows (Tiled) or floating_windows_ (Floating)
    ↓
Set WM_STATE (Normal/Iconic)
    ↓
Apply geometry-affecting states (fullscreen, maximized)
    ├─ Fullscreen: geometry applied if not iconic
    └─ Maximized: geometry saved but not applied if fullscreen
    ↓
Configure geometry
    ├─ Apply rule geometry, fullscreen geometry, or placement logic
    └─ Send synthetic ConfigureNotify after applying geometry
    ↓
xcb_map_window() (always, even if will be hidden)
    ↓
If start_iconic or not visible: hide_window() (move off-screen)
    ↓
Apply non-geometry states (above, below, skip_*)
    ↓
Update _NET_CLIENT_LIST, bars
    ↓
[Window active] → Focus, drag, state changes, workspace moves
    ↓
UnmapNotify / DestroyNotify
    ↓
handle_window_removal()
    ├─ Set WM_STATE = Withdrawn
    ├─ Erase from clients_, remove from workspace/floating
    ├─ Update workspace.focused_window to last window (or XCB_NONE if empty)
    ├─ If removed window was active:
    │  ├─ If on focused_monitor_ AND on current_workspace: focus_or_fallback()
    │  └─ Otherwise: clear_focus()
    └─ Rearrange monitor, update _NET_CLIENT_LIST, bars
```

**See also**: ICCCM Compliance → Window Lifecycle Events (lines 4982) for ICCCM-specific event handling

### Fullscreen Transition

```
[Trigger: keybind or _NET_WM_STATE]
    ↓
set_fullscreen(window, enabled)
    ↓
If enabled:
    ├─ Check already fullscreen?
    ├─ Save geometry → client.fullscreen_restore
    ├─ Set client.fullscreen = true
    ├─ Clear above/below (incompatible)
    ├─ Clear maximized (superseded)
    ├─ Set _NET_WM_STATE_FULLSCREEN
    ├─ Configure to fullscreen geometry
    └─ Stack above all
    ↓
If disabled:
    ├─ Check already not fullscreen?
    ├─ Set client.fullscreen = false
    ├─ Clear _NET_WM_STATE_FULLSCREEN
    ├─ Restore geometry from fullscreen_restore
    └─ Rearrange (tiled) or apply_floating_geometry (floating)
```

### Maximized Transition

```
[Trigger: keybind or _NET_WM_STATE]
    ↓
set_window_maximized(window, horiz, vert)
    ↓
If not fullscreen (maximize changes ignored when fullscreen):
    ├─ If enabling (horz or vert):
    │  ├─ Save geometry → client.maximize_restore (if not already set)
    │  ├─ Set client.maximized_horz = horiz
    │  ├─ Set client.maximized_vert = vert
    │  ├─ Set _NET_WM_STATE_MAXIMIZED_HORZ/VERT
    │  └─ Apply maximized geometry (floating only, working_area dimensions)
    │     ├─ maximized_horz only: width fills working_area, height unchanged
    │     ├─ maximized_vert only: height fills working_area, width unchanged
    │     └─ Both: window fills entire working_area
    └─ If disabling (both horz and vert false):
       ├─ Set client.maximized_horz = false
       ├─ Set client.maximized_vert = false
       ├─ Clear _NET_WM_STATE_MAXIMIZED_HORZ/VERT
       ├─ Restore geometry from maximize_restore
       └─ apply_floating_geometry (floating only)
```

**Note**: Tiled windows ignore maximize state (layout controls geometry).
- Maximize state flags ARE stored on tiled windows
- `maximize_restore` geometry IS saved when maximizing tiled windows
- Geometry is never actually applied to tiled windows (layout controls all geometry)
- This is because tiled windows don't have a FloatingWindow record, not an explicit design choice
- Intentional behavior to preserve state for future floating conversion
- Maximize state changes are ignored when a window is fullscreen (checked: `if (!client->fullscreen)`)

### Iconify Transition

```
[Trigger: keybind, _NET_WM_STATE, or WM_CHANGE_STATE]
    ↓
iconify_window(window)
    ↓
Check already iconic?
    ↓
Set client.iconic = true
    ↓
Set WM_STATE = IconicState
    ↓
Set _NET_WM_STATE_HIDDEN = true
    ↓
hide_window() (move off-screen, NOT unmapping)
    ↓
If was active_window_:
    ├─ If on focused_monitor_ AND on current_workspace → focus_or_fallback()
    └─ Else → clear_focus()
    ↓
Update bars
```

### Sticky Toggle

```
[Trigger: keybind or _NET_WM_STATE]
    ↓
set_window_sticky(window, enabled)
    ↓
If enabled:
    ├─ Set client.sticky = true
    ├─ Set _NET_WM_STATE_STICKY = true
    ├─ Set _NET_WM_DESKTOP = 0xFFFFFFFF
    └─ Update visibility (show on all workspaces)
    ↓
If disabled:
    ├─ Set client.sticky = false
    ├─ Clear _NET_WM_STATE_STICKY
    ├─ Set _NET_WM_DESKTOP = actual workspace
    └─ Update visibility (show only on current workspace)
```

### Focus Transition

```
[Trigger: EnterNotify, ButtonPress, keybind, _NET_ACTIVE_WINDOW]
    ↓
focus_window() or focus_floating_window()
    ↓
Check showing_desktop (reject if true)
    ↓
Check is_focus_eligible
    ↓
If iconic: deiconify first
    ↓
focus_window_state():
    ├─ Find window's monitor/workspace
    ├─ If not sticky AND workspace != current:
    │  ├─ workspace_changed = true
    │  └─ Switch workspace
    ├─ Update workspace.focused_window
    ├─ Set active_monitor_ = monitor
    └─ Set active_window_ = window
    ↓
If workspace_changed:
    ├─ hide_window() for non-sticky windows (old workspace)
    ├─ Rearrange monitor
    └─ Update floating visibility
    ↓
Clear all borders, set active border color
    ↓
Send WM_TAKE_FOCUS (if supported)
    ↓
SetInputFocus
    ↓
Update _NET_ACTIVE_WINDOW
    ↓
Clear demands_attention
    ↓
Set _NET_WM_STATE_FOCUSED
    ↓
Update client.user_time
    ↓
Apply fullscreen geometry if needed (calls apply_fullscreen_if_needed()):
  ├─ Verifies window is fullscreen AND not iconic AND on current workspace
  ├─ Sends sync request
  ├─ Configures window to fullscreen geometry
  ├─ Sends synthetic ConfigureNotify
  └─ Restacks window above all
    ↓
Restack transients above parent (visible, non-iconic transients only):
  └─ Skips hidden or iconic transients, transients on other workspaces
    ↓
Update _NET_CLIENT_LIST_STACKING
    ↓
Update bars
```

### Workspace Switch

```
[Trigger: keybind or _NET_CURRENT_DESKTOP]
    ↓
switch_workspace(target_ws)
    ↓
Validate workspace index
    ↓
workspace_policy::apply_workspace_switch():
    ├─ previous_workspace = current_workspace
    └─ current_workspace = target
    ↓
hide_window() for floating windows (old workspace, non-sticky)  ← Critical!
    ↓
hide_window() for tiled windows (old workspace, non-sticky)
    ↓
update_ewmh_current_desktop()
    ↓
rearrange_monitor(new_workspace)
    ↓
update_floating_visibility(new_workspace)
    ↓
focus_or_fallback(monitor)
    ├─ Try workspace.focused_window first (validated to exist in workspace)
    ├─ Fall back to any eligible window
    └─ Clear focus if none
    ↓
Update bars
```

### Window Removal

```
[Trigger: UnmapNotify or DestroyNotify]
    ↓
handle_window_removal() (no counter check with off-screen visibility)
    ↓
Set WM_STATE = Withdrawn
    ↓
Erase pending_kills_, pending_pings_
    ↓
Erase clients_[window]
    ↓
Remove from workspace.windows or floating_windows_
    ↓
Update workspace.focused_window
    ↓
If was active_window_:
    ├─ If removed window is on focused_monitor_ AND on current_workspace:
    │  └─ focus_or_fallback(focused_monitor())  // Try to restore focus
    └─ Else:
       └─ clear_focus()  // Just clear focus (different monitor/workspace)
    ↓
Update _NET_CLIENT_LIST, bars
```

**Note**: The conditional logic ensures focus is only restored when the removed window was on the currently focused monitor and workspace. If a window on a different monitor/workspace is removed, focus doesn't need to be restored on the current monitor.

---

## RANDR Monitor Hotplug Handling

**Trigger**: RANDR screen change (monitor hotplug, resolution change)

```
handle_randr_screen_change()
├─ Exit fullscreen for all windows (prevents stale fullscreen_restore)
│  ├─ Clear fullscreen flag from all clients
│  ├─ Clear fullscreen_restore from all clients
│  └─ Clear fullscreen_monitors from all clients (indices invalid)
├─ Save window locations by monitor NAME (not index):
│  ├─ Tiled: monitor_name + workspace_index
│  └─ Floating: monitor_name + workspace_index + geometry
├─ Destroy old bar windows
├─ Detect monitors (detect_monitors)
├─ If monitors_.empty() → create_fallback_monitor() (default monitor with screen dimensions)
├─ Recreate bar windows
├─ Update struts from dock windows
├─ Restore windows:
│  ├─ Find monitor by name (if not found, falls back to monitor 0)
│  ├─ Clamp workspace to valid range (falls back to current workspace if invalid)
│  ├─ Restore Client.monitor/workspace
│  └─ For floating: reposition if target monitor exists
│     └─ Uses floating::place_floating() on new monitor
├─ Restore focused monitor by name (if not found, defaults to monitor 0)
├─ Update EWMH (all root properties)
├─ Rearrange all monitors
├─ focus_or_fallback()
└─ Update all bars
```

**Edge Cases:**
- All monitors disconnected: Creates fallback monitor with screen dimensions, name="default"
- Monitor name not found: Windows silently fall back to monitor 0
- Focused monitor not found: Defaults to monitor 0

**Rationale**:
- Saving by monitor NAME handles monitors being turned off/on (index changes but name persists)
- Exiting fullscreen before reconfiguration prevents stale geometries
- Clearing fullscreen_monitors prevents invalid monitor indices after hotplug

---

## WM_TAKE_FOCUS Protocol

**Purpose**: Support windows with `input=False` in WM_HINTS that cannot receive `SetInputFocus`.

**When Sent**:
- Always sent when focusing any managed window
- Sent via `WM_PROTOCOLS` ClientMessage (not directly as TAKE_FOCUS)

**Protocol Flow**:
```
focus_window() / focus_floating_window()
    ↓
send_wm_take_focus(window, timestamp)
    ├─ Check WM_PROTOCOLS atom exists
    ├─ Check window supports WM_TAKE_FOCUS
    └─ Send ClientMessage with:
        ├─ type = WM_PROTOCOLS
        ├─ data[0] = WM_TAKE_FOCUS atom
        └─ data[1] = timestamp (last_event_time_ or XCB_CURRENT_TIME)
    ↓
    └─ Client must call SetInputFocus in response (ICCCM requirement)
```

**Input Focus Logic**:
```
should_set_input_focus(window):
    ├─ If WM_HINTS not specified → allow (default true)
    ├─ If WM_HINTS.input == True → allow
    └─ If WM_HINTS.input == False → disallow
```

- If allowed: Both WM_TAKE_FOCUS and SetInputFocus sent
- If disallowed: Only WM_TAKE_FOCUS sent (client must focus itself)

---

## Auto-Repeat Detection

**Purpose**: Prevent toggle_workspace action from rapid-fire execution on key hold.

**Mechanism**:
- X11 auto-repeat sends: KeyRelease → KeyPress (identical timestamps)
- LWM tracks: `last_toggle_keysym_` and `last_toggle_release_time_`

```
toggle_workspace()
    ├─ On KeyPress:
    │  ├─ If same_keysym && same_time → BLOCK (auto-repeat)
    │  └─ Else proceed with workspace switch
    └─ On KeyRelease:
       └─ If same_key → Record last_toggle_release_time_
```

**Constants**:
- `PING_TIMEOUT = 5 seconds` (ping response window)
- `KILL_TIMEOUT = 5 seconds` (force-kill timeout)

---

## Special Behaviors and Edge Cases

### Focus Suppression During Initial Scan

**Purpose**: Prevent spurious focus changes during window scan on WM startup.

**Mechanism**:
- `suppress_focus_` flag is set to `true` during `scan_existing_windows()`
- Reset to `false` after scan completes
- `manage_floating_window()` checks `suppress_focus_` before focusing new windows
- Only allows focus if window is on focused monitor and visible workspace

### Monitor Switching via Pointer

**Function**: `update_focused_monitor_at_point(x, y)`

**When called**:
- EnterNotify on root window (pointer enters empty space/gaps)
- MotionNotify when pointer crosses monitor boundary
- ButtonPress on any window

**Effect**:
- Updates `focused_monitor_` to the monitor containing the point
- Does NOT clear focus when entering root window
- Enables seamless monitor crossing without focus disruption

### Monitor Switching (focus_monitor_left/right)

**Function**: Explicit monitor switch via keybind

**Behavior**:
- Uses `wrap_monitor_index()` to cycle through monitors
- Calls `focus_or_fallback()` on new monitor to restore appropriate focus
- Calls `update_ewmh_current_desktop()` to update EWMH state
- Conditionally calls `warp_to_monitor()` if `warp_cursor_on_monitor_change` is set
- Calls `update_all_bars()` to update bar state
- Early return if only 1 monitor exists

### Move Window to Monitor (move_to_monitor_left/right)

**Function**: Moves active window to left/right monitor via keybind

**For floating windows**:
- Repositions to center of target monitor's working area using `floating::place_floating()`
- Updates Client.monitor/workspace and FloatingWindow.geometry
- Updates `_NET_WM_DESKTOP` property
- Updates floating visibility on both source and target monitors
- Moves focused_monitor_ to target and calls `focus_floating_window()`
- Warps cursor if enabled

**For tiled windows**:
- Removes from source workspace.windows
- Updates source workspace.focused_window to last remaining window (or XCB_NONE if empty)
- Adds to target monitor's current workspace.windows
- Sets target workspace.focused_window to moved window
- Rearranges both source and target monitors
- Updates focused_monitor_ to target and calls `focus_window()`
- Warps cursor if enabled

### Floating Window Monitor Auto-Assignment

**Function**: `update_floating_monitor_for_geometry(window)`

**When called**:
- After `handle_configure_request()` for floating windows
- During drag operations (`update_drag()`)
- Any time floating window geometry changes

**Behavior**:
- Calculates window center point
- Determines which monitor contains the center
- If center moved to different monitor:
  - Updates `Client.monitor` to new monitor
  - Updates `Client.workspace` to new monitor's current workspace
  - Updates `_NET_WM_DESKTOP` property
- Does NOT:
  - Call focus_or_fallback()
  - Update focused_monitor_
  - Restack the window
  - Change focus state
- This is intentional - the window simply "belongs" to a different monitor after moving

### User Time Window Indirection

**Purpose**: Some applications use a separate window for user time tracking.

**Behavior**:
- `_NET_WM_USER_TIME` property may be on `Client.user_time_window` instead of main window
- PropertyNotify events are tracked on user time windows
- When user time changes on user_time_window:
  - Finds the parent Client that owns this window
  - Updates parent Client.user_time with the new value
- If PropertyNotify arrives on user_time_window not associated with any client, update is silently ignored
- Focus stealing prevention uses parent Client.user_time

### Ping Response from User Time Window

**Behavior**:
- `_NET_WM_PING` response may come from user time window
- ClientMessage data[2] contains the ping origin window
- If data[2] == XCB_NONE, response came from main window
- Otherwise, response came from user time window
- Code handles both cases when erasing from pending_pings_
- If ping origin is not in pending_pings_ (e.g., already timed out), no action taken

### MapRequest on Already Managed Window

**Behavior**:
- When MapRequest arrives for an already-managed window, treated as deiconify request
- Condition: If window exists in `clients_` registry (get_client(window) returns valid client)
- Calls deiconify_window(window, focus)
- Focus parameter is true only if:
  - Window is on focused_monitor_
  - Window is on current workspace
- This is an alternative to _NET_WM_STATE requests for deiconification
- deiconify_window() calls apply_fullscreen_if_needed() which:
  - Verifies window is fullscreen AND not iconic AND on current workspace
  - Sends sync request
  - Configures window to fullscreen geometry
  - Sends synthetic ConfigureNotify
  - Restacks window above all

### Override-Redirect Window Filtering

**When**: During MapRequest handling

**Behavior**:
- Check `is_override_redirect_window(window)`
- If true, immediately return without managing
- These windows (menus, dropdowns, tooltips) are managed by clients directly
- Not part of WM's window management

### Window Rules Type Override Restrictions

**Restriction**: Window rules cannot override certain EWMH types.

**Protected types**:
- Dock (_NET_WM_WINDOW_TYPE_DOCK)
- Desktop (_NET_WM_WINDOW_TYPE_DESKTOP)
- Popup (_NET_WM_WINDOW_TYPE_NOTIFICATION/TOOLTIP/... menus)

**Reason**: These types are EWMH protocol-protected and have special handling rules.

### Fullscreen Geometry Multi-Monitor Support

**When**: Window has `_NET_WM_FULLSCREEN_MONITORS` set

**Behavior**:
- Stores monitor bounds in `Client.fullscreen_monitors` (top, bottom, left, right indices)
- Calculates union of specified monitor geometries for fullscreen
- Uses union geometry instead of single monitor geometry
- Cleared on RANDR hotplug (indices become invalid)

### Workspace Focused Window Staleness

**Behavior**:
- `workspace.focused_window` may reference a window that no longer exists in `workspace.windows`
- May reference a window that is now iconic or not focus-eligible
- This is a valid transient state caused by:
  - Window becomes iconic (focused_window remains set)
  - Window is removed (focused_window is set to XCB_NONE or back() of vector)
  - Window is moved to different workspace (focused_window unchanged on source)
- Staleness is corrected lazily: when `select_focus_candidate()` is called, it validates that the focused_window exists in workspace.windows before considering it
- `workspace.focused_window` is a "best-effort" hint for focus restoration

### Empty Workspace Handling

**Behavior**:
- Workspaces can have zero windows
- focus_or_fallback() returns no focus candidate
- Focus is cleared when no eligible windows exist
- Layout algorithm handles empty workspace case (no windows to tile)
- Empty workspace drag target: When dragging a tiled window to an empty workspace, insertion defaults to position 0

### Zero-Size Window Fallback

**Behavior**:
- Windows with zero width or height default to 300x200
- Applies after geometry query and size hint checks, before apply_size_hints()
- Also applies to handle_configure_request() for floating windows
- Minimum dimensions clamped to 1 pixel after size hints applied
- Size hint enforcement happens after zero-size check: if hints produce 0, clamp to 1 occurs in apply_floating_geometry()
- Affects both tiled (though hints ignored) and floating windows

### X Connection Error Detection

**Behavior**:
- Main loop checks `xcb_connection_has_error()` on each iteration
- If error detected: LOG_ERROR and initiate graceful shutdown
- Prevents hanging on X server disconnection

### Mousebind Validation

**Behavior**:
- Invalid mousebind configurations are silently filtered during initialization
- Filtered out: button <= 0, button > 255, empty action
- Config loading continues with valid mousebinds only

---

## Terminology Notes

**Key Terms:**
- **Off-screen visibility**: LWM's visibility model using OFF_SCREEN_X (-20000) instead of unmap/map cycles
- **Hidden flag** (`client.hidden`): The flag that implements off-screen visibility (true = at OFF_SCREEN_X)
- **Iconic state**: ICCCM term for minimized windows (WM_STATE = IconicState)
- **Minimized**: User-facing term, implemented via iconic state in LWM
- **MRU**: Most Recently Used ordering (reverse iteration for focus restoration and floating window stacking)

**Distinctions:**
- "Off-screen" refers to the visibility concept
- "Hidden" refers to the `client.hidden` flag that implements it
- Use consistent: off-screen for visibility state, hidden for flag name

**Structure Relationships:**
- `Client` struct: Authoritative source of truth for all window state
- `FloatingWindow` struct: Runtime geometry only (synced with Client.floating_geometry)
- `Workspace::windows`: Derived organization state for tiled windows

 ---

## System Invariants

These invariants are validated by LWM_DEBUG builds via LWM_ASSERT_INVARIANTS() (see core/invariants.hpp):

**Client Registration Invariants**:
- Window in clients_ must have valid monitor index (monitor < monitors_.size())
- Window in clients_ must have valid workspace index (workspace < monitors_[monitor].workspaces.size())
- Each tiled window appears in exactly one Workspace.windows vector
- Each floating window appears exactly once in floating_windows_ vector

**Focus Consistency Invariants**:
- active_window_ must be in clients_ (or XCB_NONE)
- active_window_ must not be iconic (if not XCB_NONE)
- active_window_ must be focus-eligible (not Dock/Desktop, accepts input or supports take_focus)
- active_window_ must have showing_desktop_ == false (if not XCB_NONE)
- active_window_ must be on visible workspace (if not XCB_NONE and not sticky)

**State Consistency Invariants**:
- Can't be fullscreen AND iconic (iconic windows are off-screen, fullscreen requires visible)
- Can't be above AND below (mutually exclusive stacking)
- modal=true implies above=true (modal forces above state)
- iconic=true implies hidden=true (minimized windows are off-screen)
- hidden=false implies iconic=false (on-screen windows can't be minimized)

**Desktop Validity Invariants**:
- Desktop index from get_ewmh_desktop_index() must be valid or 0xFFFFFFFF (sticky)
- _NET_WM_DESKTOP queries return early if workspace_idx >= monitor.workspaces.size()

**Workspace Consistency Invariants**:
- Sticky windows appear in all Workspace.windows vectors (all workspaces of their monitor)
- Each non-sticky tiled window appears in exactly one Workspace.windows vector
- Floating windows are tracked globally in floating_windows_, not per-workspace

**Monitor Bounds Invariants**:
- All monitor index lookups must check bounds before access
- Workspace indices are clamped to valid range when switching
- Monitor lookups fail gracefully (early return) if index is invalid

These invariants help catch bugs early and ensure the window manager state remains consistent.

---

## Additional State Conflicts and Invalid Transitions

**Invalid/Ineffective Transitions**:
- set_fullscreen() on iconic window: Flag is set, but geometry is NOT applied (apply_fullscreen_if_needed returns early)
- set_window_maximized() on iconic window: Flags are set and geometry is saved, but geometry is NOT applied until window is deiconified
- begin_drag() / begin_tiled_drag() on fullscreen window: Silently rejected
- begin_tiled_drag() during showing_desktop mode: Silently rejected
- begin_tiled_drag() on floating window: Silently rejected

**State Conflicts**:
- **fullscreen ↔ above/below**: Incompatible, clear others when enabling fullscreen
- **fullscreen ↔ maximized**: Superseded, clear maximized flags when enabling fullscreen (but maximize_restore geometry is preserved for restoration)
- **maximized when fullscreen**: set_window_maximized() has NO effect when fullscreen is enabled. Geometry is applied only when fullscreen is disabled.
- **above ↔ below**: Mutually exclusive, clear opposite when enabling
- **modal ⇔ above**: Modal state and above state are tightly coupled
   - When modal is enabled, above is also enabled (set_window_modal calls set_window_above(true))
   - When modal is disabled, above is also cleared (set_window_modal calls set_window_above(false))
   - This coupling is NOT bidirectional: disabling modal ALWAYS disables above (even if above was set independently before modal was enabled)
- **hidden vs iconic**: Iconic always implies hidden (iconic=true ⇒ hidden=true), but hidden can be true without iconic

**Valid State Combinations**:
- hidden=true, iconic=false: Off-screen but not minimized (workspace switch)
- hidden=true, iconic=true: Off-screen and minimized (iconified)
- hidden=false, iconic=false: On-screen, visible (normal)
- hidden=false, iconic=true: NOT VALID (iconic implies hidden)
- fullscreen=true, maximized_*=(anything): Maximization flags cleared but maximize_restore preserved
- fullscreen=false, maximized_*=true: Normal maximized state
- modal=true, above=true: Both set (modal enables above)
- modal=false, above=true: Possible (above set independently)
- modal=true, above=false: NOT VALID (modal forces above=true)
- modal=false, above=false: Normal state

 ---

## Special Behaviors and Edge Cases

### Workspace and Monitor Edge Cases

**Empty Workspace Target**:
- When dragging tiled window to empty workspace, insertion defaults to position 0 (beginning)
- When switching to empty workspace, focus_or_fallback() finds no candidates and clears focus

**Invalid Monitor Indices**:
- All monitor index lookups check bounds before access
- Early returns for invalid monitor indices in various functions:
  - focus_window(): Checks client.monitor < monitors_.size()
  - update_floating_visibility(): Early return if monitor_idx >= monitors_.size()
  - Many other functions have similar checks
- Fallback behavior: Return without action or use monitor 0

**Workspace Index Clamping**:
- Workspace indices are clamped to valid range when switching
- target_ws = std::min(target_ws, monitors_[target_mon].workspaces.size() - 1)
- Prevents out-of-bounds access to workspaces array
- During monitor hotplug: workspace indices validated before assignment

### Focus Edge Cases

**Focus Restoration with No Candidates**:
- focus_or_fallback() builds candidates in order:
  1. workspace.focused_window (if exists and eligible)
  2. Current workspace tiled windows (reverse iteration = MRU)
  3. Sticky tiled windows from other workspaces (reverse iteration)
  4. Floating windows visible on monitor (reverse iteration = MRU)
- If all candidates filtered out (no focus-eligible windows), clear_focus() is called

**Focus on Iconic Window**:
- Focusing iconic window triggers deiconify() automatically
- Window is restored (brought on-screen) then focused
- workspace.focused_window updated to this window

**Focus Across Monitors**:
- Focusing window on different monitor does NOT warp cursor (warping only for explicit keybind)
- Implicitly changes focused_monitor_ (as side effect of workspace switch)
- Window's workspace becomes current_workspace on its monitor

### State Change Edge Cases

**Fullscreen on Non-Visible Workspace**:
- Fullscreen windows on non-current workspaces remain off-screen (iconic or just hidden)
- Fullscreen geometry is not applied until workspace becomes active
- apply_fullscreen_if_needed() returns early for iconic or off-screen windows

**Maximizing Iconic Windows**:
- Maximizing an iconic window sets flags and saves geometry
- Geometry is NOT applied until window is deiconified
- This is intentional state preservation

**Setting Fullscreen on Iconic Window**:
- set_fullscreen() sets the fullscreen flag
- Geometry is NOT applied (apply_fullscreen_if_needed returns early)
- This is a silent no-op (flag is set but no visible effect until deiconify)

**State Transitions During Workspace Switch**:
- hide_window() is called for old workspace windows (floating first, then tiled)
- conn_.flush() ensures hide operations complete before showing new workspace
- rearrange_monitor() explicitly calls show_window() for new workspace windows
- update_floating_visibility() handles floating window show/hide

### Drag Operation Edge Cases

**Silent Abort in end_drag()**:
- If monitor_index_for_window() or workspace_index_for_window() fails → Abort without error
- If client unmanaged during drag → Abort without error
- If monitors_.empty() (all monitors disconnected) → Abort without error

**Drag Rejection**:
- Fullscreen windows: Cannot start drag (begin_drag returns silently)
- showing_desktop mode: Cannot start tiled drag (begin_tiled_drag returns silently)
- Floating windows: Cannot start tiled drag (begin_tiled_drag returns silently)

**Cross-Workspace Drag**:
- Tiled windows: Moves window to target workspace, updates Client.monitor and Client.workspace
- Floating windows: Drag to different monitor triggers implicit monitor assignment via update_floating_monitor_for_geometry()
  - Does NOT restore focus (different from explicit monitor move)
  - Visibility must be updated manually

### Protocol Interaction Edge Cases

**PropertyNotify on User Time Window**:
- When _NET_WM_USER_TIME changes on user_time_window, parent Client.user_time is updated
- If PropertyNotify arrives on user_time_window not associated with any client, update is silently ignored
- This handles race conditions where window is unmapped before property change

**UnmapNotify with Off-Screen Visibility**:
- With off-screen visibility, ANY UnmapNotify is treated as client-initiated withdraw request
- WM never unmaps windows (always uses off-screen positioning)
- No counter tracking exists (unlike traditional unmap-based visibility)

**Duplicate UnmapNotify Prevention**:
- WM does NOT select STRUCTURE_NOTIFY on client windows
- Selecting STRUCTURE_NOTIFY causes duplicate UnmapNotify events
- Duplicate events would break off-screen visibility model

**Sync Request Timeout**:
- wait_for_sync_counter() attempts to read updated counter value
- If client doesn't respond within 50ms, operation proceeds anyway (non-blocking fallback)
- Most clients respond quickly, but hung clients don't block event loop

---

## Invariants

### Runtime Invariant Check Status

**The formal invariants defined in core/invariants.hpp are NEVER actually invoked in the codebase.** The invariant assertion system (LWM_ASSERT_INVARIANTS, LWM_ASSERT_CLIENT_STATE, LWM_ASSERT_FOCUS_CONSISTENCY) exists but provides no runtime protection. These invariants are documentation-only.

### Invariant 1: Client Management Consistency

**Every managed window has a valid Client record:**
- Window exists in `clients_` registry
- If kind is Tiled/Floating:
  - `client.monitor < monitors.size()`
  - `client.workspace < monitors[monitor].workspaces.size()`
- Dock/Desktop kinds have monitor/workspace fields but they are meaningless

### Invariant 2: Focus Consistency

**Active window is always valid and visible:**
- If `active_window_ != XCB_NONE`:
  - Window exists in `clients_`
  - Window is NOT iconic
  - Window is NOT hidden
  - Window is NOT on a hidden workspace

### Invariant 3: Client State Internal Consistency

**Mutually exclusive states are not both true:**
- NOT (fullscreen AND iconic)
- NOT (above AND below)

**Enforced invariants:**
- `modal ⇒ above`: When modal is enabled, above is also enabled (set_window_modal enforces this)

### Invariant 4: Desktop Index Validity

**Desktop index is in valid range or sticky:**
- desktop < monitors × workspaces_per_monitor
- OR desktop == 0xFFFFFFFF (sticky marker)

### Invariant 5: Workspace Consistency

**Every tiled window appears in exactly one workspace:**
- Each window in workspace.windows exists in `clients_`
- Each window with Kind::Tiled appears in exactly one workspace
- No duplicate windows across workspaces
- Sticky windows appear in ALL workspace.windows vectors (participate in tiling on all workspaces)

### Invariant 6: Window Kind Container Uniqueness

**Each window appears in exactly one window-specific container based on kind:**
- Kind::Tiled → Appears in exactly one Workspace::windows
- Kind::Floating → Appears in exactly one floating_windows_ entry
- Kind::Dock → Appears in exactly one dock_windows_ entry
- Kind::Desktop → Appears in exactly one desktop_windows_ entry
- All windows appear in clients_ registry (unified source of truth)

### Invariant 7: Floating Window Uniqueness

**Each xcb_window_t appears at most once in floating_windows_:**
- No duplicate windows in the vector
- MRU promotion validates uniqueness before moving

### Invariant 8: Client Order Monotonicity

**client.order values are unique and monotonically increasing:**
- Assigned via next_client_order_++ on window management
- Used for consistent _NET_CLIENT_LIST ordering

### Invariant 9: Workspace Focused Window Validity

**workspace.focused_window is a best-effort hint:**
- May reference a window no longer in workspace.windows (e.g., after window removal)
- May reference an iconic or focus-ineligible window
- select_focus_candidate() validates existence and eligibility before using it
- If validation fails, falls back to reverse iteration (MRU order)
- May reference a window that is no longer in the workspace
- May reference a window that is iconic or not focus-eligible
- select_focus_candidate() validates existence before use
- Updated when focus changes, may become stale after window removal

### Invariant 10: Fullscreen Geometry Application Preconditions

**Fullscreen geometry is only applied when ALL of:**
- Window is fullscreen (is_client_fullscreen(window))
- Window is NOT iconic (!is_client_iconic(window))
- Window's monitor is valid (client.monitor < monitors_.size())
- Window is on its monitor's current workspace

### Invariant 11: Hidden Flag Coordination

**The Client.hidden flag is separate from iconic:**
- iconic ⇒ hidden (iconic windows are always hidden off-screen)
- hidden does NOT ⇒ iconic (can be hidden for workspace visibility without being iconic)
- hidden = true ⇒ window is at OFF_SCREEN_X (-20000)
- hidden = false ⇒ window is at on-screen position

### Single Source of Truth

**Client struct is authoritative for all window state.**
- FloatingWindow.geometry is transient (runtime state)
- Workspace vectors are derived (organization state)
- All other structures reference `clients_`

### State Synchronization Guarantees

1. **Client ↔ EWMH**: Every state flag change updates EWMH property
2. **Client ↔ ICCCM**: WM_STATE property tracks iconic state
3. **FloatingWindow ↔ Client**: Geometry synced on all operations
4. **Workspace ↔ Focus**: `workspace.focused_window` tracks last-focused (best-effort)
5. **Global State**: All visible state has EWMH root property

### Geometry Application Order

**Critical for preventing "flash" artifacts:**
1. Apply geometry-affecting states (fullscreen, maximized)
2. Configure geometry
3. Map window (xcb_map_window - always mapped)
4. Apply non-geometry states (above, below, skip_*)
5. If not visible: hide_window() (move off-screen)

### Workspace Visibility Model

```
Window is visible IF:
- NOT client.hidden
- NOT showing_desktop
- NOT iconic
- (sticky OR workspace == current)
```

### Focus Eligibility Model

```
Window is focus_eligible IF:
- Kind is NOT Dock or Desktop
- (accepts_input_focus OR supports_take_focus)
```

**Additional focus barriers (these can prevent focus even for eligible windows):**
- Hidden windows (client.hidden == true) - filtered in event handlers (EnterNotify, MotionNotify, ButtonPress)
- Windows on non-visible workspaces
- showing_desktop_ mode blocks all focus except exit actions
