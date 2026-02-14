# LWM Implementation Overview

> **Documentation Navigation**
> - Previous: [DOCS_INDEX.md](DOCS_INDEX.md) (Documentation roadmap)
> - Related: [STATE_MACHINE.md](STATE_MACHINE.md) (Window states) | [EVENT_HANDLING.md](EVENT_HANDLING.md) (Event handling)
> - See also: [CLAUDE.md](CLAUDE.md) (Development guide) | [BEHAVIOR.md](BEHAVIOR.md) (User-facing behavior)

This document provides the implementation-level architecture, data structures, and invariants for LWM. For window state machine details, see [STATE_MACHINE.md](STATE_MACHINE.md). For event handling specifications, see [EVENT_HANDLING.md](EVENT_HANDLING.md).

---

## Table of Contents
1. [Architecture Overview](#architecture-overview)
2. [Data Structures](#data-structures)
3. [Terminology](#terminology)
4. [Invariants](#invariants)
5. [Error Handling and Validation](#error-handling-and-validation)
6. [Visibility and Off-Screen Positioning](#visibility-and-off-screen-positioning)
7. [Focus System Overview](#focus-system-overview)
8. [Workspace and Monitor Management](#workspace-and-monitor-management)

---

## Architecture Overview

### Core Components

```
WindowManager (wm.cpp)
├── clients_ (unordered_map<window, Client>)  ← Authoritative state
├── monitors_ (vector<Monitor>)
├── floating_windows_ (vector<xcb_window_t>)  ← MRU order
├── dock_windows_ (vector<window>)
├── desktop_windows_ (vector<window>)
└── Ewmh, KeybindManager, Layout
```

### Entry Points

**main()** → WindowManager construction → run() → Main event loop

**Initialization Sequence** (src/lwm/wm.cpp:47-96):
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
12. scan_existing_windows()
13. run_autostart()
14. keybinds_.grab_keys()
15. update_ewmh_client_list()

### Source File Organization

| File | Purpose | Lines |
|------|---------|-------|
| wm.cpp | Main window management, initialization, core operations | ~2300 |
| wm_events.cpp | XCB event handlers | ~1600 |
| wm_floating.cpp | Floating window management | ~560 |
| wm_focus.cpp | Focus handling logic | ~490 |
| wm_workspace.cpp | Workspace switching, toggling, moving windows | ~330 |
| wm_ewmh.cpp | EWMH protocol handling | ~260 |
| wm_drag.cpp | Mouse drag state machine | ~250 |

---

## Data Structures

### Client (src/lwm/core/types.hpp:83-152)

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

**WindowManager class members**:
- `next_client_order_`: Counter for generating unique, monotonically increasing Client.order values (src/lwm/wm.cpp:630)

### Workspace (src/lwm/core/types.hpp:154-160)

```cpp
struct Workspace {
    std::vector<xcb_window_t> windows;  // Tiling order
    xcb_window_t focused_window = XCB_NONE;
};
```

### Monitor (src/lwm/core/types.hpp:162-175)

```cpp
struct Monitor {
    xcb_randr_output_t output;
    std::string name;
    int16_t x, y;
    uint16_t width, height;
    std::vector<Workspace> workspaces;
    size_t current_workspace = 0;
    size_t previous_workspace = 0;
    Strut strut;
};
```

### DragState (src/lwm/wm.hpp:29-40)

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

### Geometry Types (src/lwm/core/types.hpp:200-217)

```cpp
struct Geometry {
    int16_t x = 0;
    int16_t y = 0;
    uint16_t width = 0;
    uint16_t height = 0;
};

struct Strut {
    uint32_t left = 0;
    uint32_t right = 0;
    uint32_t top = 0;
    uint32_t bottom = 0;
};

struct FullscreenMonitors {
    uint32_t top = 0;
    uint32_t bottom = 0;
    uint32_t left = 0;
    uint32_t right = 0;
};
```

**Strut Aggregation**:
- Multiple dock windows can coexist on the same monitor.
- Struts are aggregated by taking the maximum value for each side.
- Example: dock A (top=30) and dock B (top=50) → effective top strut is 50.

### Key Constants (src/lwm/core/types.hpp:17; wm.cpp:21-24)

```cpp
constexpr int16_t OFF_SCREEN_X = -20000;
constexpr auto SYNC_WAIT_TIMEOUT = std::chrono::milliseconds(50);
constexpr auto PING_TIMEOUT = std::chrono::seconds(5);
constexpr auto KILL_TIMEOUT = std::chrono::seconds(5);

// WM_STATE values
constexpr uint32_t WM_STATE_WITHDRAWN = 0;
constexpr uint32_t WM_STATE_NORMAL = 1;
constexpr uint32_t WM_STATE_ICONIC = 3;
```

---

## Terminology

**Synthetic event**: Generated by X server or window manager rather than natural X11 events. Example: synthetic ConfigureNotify sent by WM to inform client of geometry after layout changes.

**Intentional no-op**: Graceful early return with no error logging for invalid state or unsupported operations.

**Best-effort**: Reflects known-good state when set, but may become stale if underlying state changes. Callers must validate before use.

**MRU (Most Recently Used)**: Reverse iteration through vectors (newest items at end) provides MRU ordering for focus restoration and floating window stacking.

---

## Invariants

### Single Source of Truth

**Client struct is authoritative for all window state.**
- `Client.floating_geometry` is the single source of truth for floating window geometry
- Workspace vectors are derived (organization state)
- All other structures reference `clients_`

### Data Structure Relationships

**Monitor → Workspace containment:**
- Each Monitor contains a vector of Workspace objects
- Monitors track current_workspace and previous_workspace indices
- Workspaces belong to exactly one monitor

**Window containment (one-to-many):**
- Each window appears in exactly one container based on kind:
  - Kind::Tiled → Appears in exactly one Workspace::windows vector
  - Kind::Floating → Appears in exactly one floating_windows_ entry
  - Kind::Dock → Appears in exactly one dock_windows_ entry
  - Kind::Desktop → Appears in exactly one desktop_windows_ entry
- All managed windows appear in clients_ registry (unified source of truth)

**Cross-references:**
- Workspace.focused_window references a window ID (best-effort hint)

### State Synchronization Guarantees

1. **Client ↔ EWMH**: Every state flag change updates EWMH property
2. **Client ↔ ICCCM**: WM_STATE property tracks iconic state
3. **Workspace ↔ Focus**: `workspace.focused_window` tracks last-focused (best-effort)
4. **Global State**: All visible state has EWMH root property

### Client Management Invariants

**Every managed window has a valid Client record:**
- Window exists in `clients_` registry
- If kind is Tiled/Floating:
    - `client.monitor < monitors.size()`
    - `client.workspace < monitors[monitor].workspaces.size()`
- Dock/Desktop kinds have monitor/workspace fields but they are meaningless

**client.order values are unique and monotonically increasing:**
- Assigned via `next_client_order_++` on window management
- Used for consistent _NET_CLIENT_LIST ordering

### Focus Consistency Invariants

**Active window is always valid and visible:**
- If `active_window_ != XCB_NONE`:
    - Window exists in `clients_`
    - Window is NOT iconic
    - Window is NOT hidden
    - Window is NOT on a hidden workspace

**Window is focus_eligible IF:**
- Kind is NOT Dock or Desktop
- (accepts_input_focus OR supports_take_focus)

**Additional focus barriers** (prevent focus even for eligible windows):
- Hidden windows (client.hidden == true) - filtered in event handlers (EnterNotify, MotionNotify, ButtonPress)
- Windows on non-visible workspaces
- showing_desktop_ mode blocks all focus except exit actions

### Client State Internal Consistency

**Mutually exclusive states are not both true:**
- NOT (above AND below)

**State flag relationships:**
- iconic ⇒ hidden for NON-STICKY windows (iconic windows always hidden off-screen; sticky windows exception)
- hidden does NOT ⇒ iconic (can be hidden for workspace visibility without being iconic)
- hidden = true ⇒ window is at OFF_SCREEN_X (-20000)
- hidden = false ⇒ window is at on-screen position
- modal ⇒ above: When `set_window_modal(enabled)` called, it calls `set_window_above(enabled)`. When `set_window_modal(false)` called, it calls `set_window_above(false)`. Coupling is one-way from modal to above: `set_window_above()` can be called independently without affecting modal state

### Desktop Index Validity

**Desktop index is in valid range or sticky:**
- desktop < monitors × workspaces_per_monitor
- OR desktop == 0xFFFFFFFF (sticky marker)

### Workspace and Monitor Consistency

**Every tiled window appears in exactly one workspace:**
- Each window in workspace.windows exists in `clients_`
- Each window with Kind::Tiled appears in exactly one workspace
- No duplicate windows across workspaces
- Sticky windows appear in exactly one workspace.windows vector (visible on all via hide_window skip, overlay on other workspaces)

**workspace.focused_window is a best-effort hint:**
- Normally updated correctly on window removal (set to workspace.windows.back() or XCB_NONE)
- May become stale in edge cases:
   - After explicit window move between workspaces (if source workspace's focused_window isn't updated)
   - After unusual state transitions that bypass normal update paths
- May reference an iconic or focus-ineligible window
- `select_focus_candidate()` validates existence and eligibility before using
- If validation fails, falls back to reverse iteration (MRU order)

### Visibility Conditions

**Window is visible IF:**
- NOT client.hidden
- NOT showing_desktop
- NOT iconic
- (sticky OR workspace == current)

### Initial Values

**Initial values during WM startup:**
- `previous_workspace`: Initially equals `current_workspace` (starts at 0)
- `focused_monitor`: Initially set to monitor 0 during initialization
- `active_window`: Initially XCB_NONE (no focus)

### Floating Window Invariants

**Client.floating_geometry is the single source of truth for floating window geometry.**
- `apply_floating_geometry(window)` reads from `Client.floating_geometry` and configures the X window
- `floating_windows_` is a plain `vector<xcb_window_t>` maintaining MRU order only

**Each xcb_window_t appears at most once in floating_windows_:**
- No duplicate windows in vector
- MRU promotion validates uniqueness before moving

### Fullscreen Invariants

**Fullscreen windows are excluded from tiling layout calculations:**
- Fullscreen windows never appear in `visible_windows` during `rearrange_monitor()`
- Fullscreen windows have dedicated fullscreen_geometry calculation
- Layout algorithm only considers non-fullscreen tiled windows
- Invariant enforced implicitly (fullscreen windows filtered before layout)

---

## Error Handling and Validation

### Error Handling Philosophy

LWM uses graceful error handling to prevent crashes and maintain stability:

**Intentional no-ops**: Many operations return early without error logging for invalid state, preventing cascading failures (e.g., focusing non-existent window, dragging fullscreen window).

**Bounds checking**: All array/vector access is validated before use (monitor indices checked against `monitors_.size()`, workspace indices clamped, client lookups validated).

**Fallback behaviors**: When operations cannot proceed, return default values or use fallback monitor/monitor 0 rather than crashing.

### Validation Strategy

**Bounds Checking Reference Table**:

| Function | Bounds Check | Fallback Behavior |
|----------|-------------|-------------------|
| update_floating_visibility (wm_floating.cpp) | monitor_idx < monitors_.size() | Return early (LOG_TRACE) |
| switch_to_ewmh_desktop | monitor_idx < monitors_.size(), ws_idx < workspaces.size() | Return early (LOG_TRACE) |
| focus_any_window | client->monitor < monitors_.size() (floating path) | Return early (LOG_TRACE) |
| apply_maximized_geometry | client->monitor < monitors_.size() | Return early |
| Rule application | target_mon < monitors_.size() | Skip entire rule |
| apply_floating_geometry | Returns early if no Client record | No-op |
| workspace_index_for_window | Returns std::nullopt for unmanaged windows | Caller handles null |

---

## Visibility and Off-Screen Positioning

### Overview

LWM uses off-screen visibility instead of unmap/map cycles for controlling window visibility. This avoids GPU rendering issues with Chromium, Electron, Qt/PyQt with OpenGL applications.

**Mechanism**:
- Windows are always mapped (XCB_MAP_STATE_VIEWABLE) at all times.
- Visibility is controlled by moving windows to off-screen position: `OFF_SCREEN_X = -20000` (src/lwm/core/types.hpp:18).
- `client.hidden` flag tracks off-screen state: true = at OFF_SCREEN_X, false = at on-screen position.

### Key Functions

**hide_window(window)** (src/lwm/wm.cpp:2208-2239):
- Return early if no Client record (intentional no-op)
- Return early if client.sticky (sticky windows never hidden)
- Return early if client.hidden (prevent redundant off-screen moves)
- Set client.hidden=true
- Move window to OFF_SCREEN_X, preserve y coordinate

**show_window(window)** (src/lwm/wm.cpp:2251-2271):
- Return early if no Client record (intentional no-op)
- Return early if client.hidden == false (already visible)
- Set client.hidden=false only
- Does NOT send configure events or restore position
- Caller must call `rearrange_monitor()` (tiled) or `apply_floating_geometry()` (floating)

### Visibility Control Channels

1. **Iconification**: `iconify_window()` sets iconic=true AND calls hide_window() (src/lwm/wm.cpp:1288-1331)
2. **Workspace switches**: `hide_window()` for windows leaving, `show_window()` for entering (src/lwm/wm_workspace.cpp:8-80)
3. **Showing Desktop**: `hide_window()` for all non-sticky windows
4. **Sticky windows**: Never hidden by `hide_window()` (early return, remain visible across workspace switches)

### Interaction with ICCCM

With off-screen visibility, ALL UnmapNotify events are client-initiated withdraw requests:
- WM never unmaps windows (always uses off-screen positioning).
- No counter tracking exists (unlike traditional unmap-based visibility).
- Any UnmapNotify triggers `handle_window_removal()` (src/lwm/wm_events.cpp:66-73).

### Sticky Window Visibility

- `hide_window()` returns early for sticky windows (src/lwm/wm.cpp:2220-2223)
- Sticky windows visible on all workspaces by design
- Controlled via workspace tiling algorithm and layout filtering, not via visibility management

### Desktop Window Visibility

- LWM never calls hide_window/show_window() on desktop windows
- Desktop windows always positioned as background windows below all others
- hidden flag never modified for desktop windows

---

## Focus System Overview

### Focus Eligibility

**is_focus_eligible(window)** formal definition (src/lwm/core/policy.hpp:69-74):
```
is_focus_eligible(window) =
    NOT (kind == Dock OR kind == Desktop)
    AND (accepts_input_focus OR supports_take_focus)
```

This is the formal definition of focus eligibility. Additional barriers can prevent focus even for eligible windows (see Focus Barriers below).

**Where this function is used:**
- In focus restoration logic (focus_or_fallback)
- When validating candidate windows
- NOT in event handlers (those filter differently)

**What it checks:**
- Window kind (excludes Dock and Desktop)
- WM_HINTS.input flag
- WM_PROTOCOLS for WM_TAKE_FOCUS support

**What it does NOT check:**
- `client.hidden` - filtered in event handlers instead
- `client.iconic` - handled via deiconification transition
- `showing_desktop_` - checked in focus functions directly
- Workspace visibility - checked in focus functions directly

### Focus Barriers

The following conditions can prevent focus even for focus-eligible windows:
1. **showing_desktop_ == true** - Blocks all focus except desktop mode exit
2. **client.hidden == true** - Off-screen windows (filtered in event handlers)
3. **iconic windows** - For NON-STICKY windows: iconic ⇒ hidden, so they are blocked by hidden check. For sticky windows: iconic windows have hidden=false but are still blocked from receiving focus because deiconification is required before focusing.
4. **Dock and Desktop kinds** - These window kinds are never focus-eligible by is_focus_eligible() definition.
5. **Windows on non-visible workspaces** - Wrong workspace

### Focus Assignment Functions

**focus_any_window(window)** (src/lwm/wm_focus.cpp) - Unified entry point for all window types:
1. Check not showing_desktop
2. Check is_focus_eligible
3. Look up Client, determine tiled vs floating via `Client::kind`
4. If iconic: deiconify first (clears iconic flag)
5. **Tiled path**: Call `focus_window_state()` - may switch workspace
6. **Floating path**: Manual workspace switch + MRU promotion + set `active_window_`
7. Update EWMH current desktop
8. Clear all borders
9. Apply focused border visuals only when target window is not fullscreen
10. Send WM_TAKE_FOCUS if supported
11. Set X input focus
12. If floating: stack above (XCB_STACK_MODE_ABOVE)
13. Update _NET_ACTIVE_WINDOW, _NET_WM_STATE_FOCUSED
14. Update user_time, restack transients, update _NET_CLIENT_LIST

**Fullscreen focus invariant**:
- Focus transitions do not reapply fullscreen geometry (`fullscreen_policy::ApplyContext::FocusTransition` is excluded).
- Fullscreen windows keep zero border width when focus leaves and returns.

**focus_or_fallback(monitor)** - Smart focus selection:
1. Build candidates (order of preference):
    - `workspace.focused_window` if exists in workspace AND eligible (validated to exist).
    - Current workspace tiled windows (reverse iteration = MRU).
    - Sticky tiled windows from other workspaces (reverse iteration).
    - Floating windows visible on monitor (reverse iteration = MRU).
2. Call focus_policy::select_focus_candidate().
3. Focus selected or clear focus.

For complete focus system details, see [STATE_MACHINE.md §6](STATE_MACHINE.md#6-focus-system).

---

## Workspace and Monitor Management

### Workspace Structure

Each Monitor contains a vector of Workspace objects (src/lwm/core/types.hpp:162-175):
- **current_workspace**: Currently visible workspace index.
- **previous_workspace**: Last visible workspace (for toggle functionality).
- Each workspace maintains ordered list of tiled windows and focused_window hint.

### Monitor Detection and Hotplug

Monitor detection via RANDR (src/lwm/wm.cpp: detect_monitors):
- Queries available outputs
- Creates Monitor objects with geometry and workspace allocation
- Handles RANDR screen change events (src/lwm/wm_events.cpp:55-59)
- Workspace state preserved across hotplug where possible

### Workspace Switching

**switch_workspace(int ws)** (src/lwm/wm_workspace.cpp:8-80):
1. Policy validation (workspace_policy::apply_workspace_switch)
2. Hide floating windows from old workspace
3. Hide tiled windows from old workspace
4. Update EWMH _NET_CURRENT_DESKTOP
5. Rearrange monitor for new workspace
6. Update floating visibility
7. Focus restoration (focus_or_fallback)

### Window Movement Between Workspaces

When moving a window to another workspace:
1. Remove from source workspace.windows vector
2. Insert into destination workspace.windows vector
3. Update client.workspace field
4. If destination not visible: hide_window()
5. Rearrange both monitors (layout recalculation)

For complete workspace behavior details, see [STATE_MACHINE.md §10](STATE_MACHINE.md#10-workspace-management).

---

## Related Documents

- **[STATE_MACHINE.md](STATE_MACHINE.md)** - Complete window state machine, state transitions, lifecycle
- **[EVENT_HANDLING.md](EVENT_HANDLING.md)** - Event-by-event handling specifications
- **[BEHAVIOR.md](BEHAVIOR.md)** - User-facing behavior (focus, workspaces, monitors)
- **[COMPLIANCE.md](COMPLIANCE.md)** - Protocol obligations (ICCCM/EWMH)
- **[SPEC_CLARIFICATIONS.md](SPEC_CLARIFICATIONS.md)** - Design decisions on spec ambiguities
