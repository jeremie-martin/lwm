# LWM Implementation Overview

This document covers architecture, data structures, and invariants. For window states and transitions, see [STATE_MACHINE.md](STATE_MACHINE.md). For event handling, see [EVENT_HANDLING.md](EVENT_HANDLING.md).

---

## Table of Contents
1. [Initialization Sequence](#initialization-sequence)
2. [Data Structures](#data-structures)
3. [Terminology](#terminology)
4. [Invariants](#invariants)
5. [Visibility and Off-Screen Positioning](#visibility-and-off-screen-positioning)
6. [Workspace Switching](#workspace-switching)

---

## Initialization Sequence

**main()** → WindowManager construction → run() → Main event loop

Steps (src/lwm/wm.cpp):
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

---

## Data Structures

All types defined in `src/lwm/core/types.hpp`.

### Client

Authoritative source of truth for all window state. Key design notes:
- `Kind` (Tiled/Floating/Dock/Desktop) determines container membership and dispatch
- `hidden` is physical position state (off-screen at -20000); `iconic` is logical minimization
- `floating_geometry` is the single source of truth for floating window position/size
- `order` is unique, monotonically increasing — used for `_NET_CLIENT_LIST` ordering
- State flags are synced bidirectionally with `_NET_WM_STATE` atoms
- `modal ⇒ above` (one-way coupling): `set_window_modal()` calls `set_window_above()`, but `set_window_above()` can be called independently without affecting modal

### Strut Aggregation

Multiple dock windows' struts are aggregated by taking the maximum value per side (e.g., dock A top=30, dock B top=50 → effective top=50).

### Key Constants

```cpp
constexpr int16_t OFF_SCREEN_X = -20000;       // Hidden window x-coordinate
constexpr auto PING_TIMEOUT = std::chrono::seconds(5);
constexpr auto KILL_TIMEOUT = std::chrono::seconds(5);
```

---

## Terminology

- **Synthetic event**: ConfigureNotify sent by WM to inform client of geometry, not natural X11 event
- **Intentional no-op**: Graceful early return for invalid state (no error logging)
- **MRU**: Reverse iteration through vectors (newest at end) for focus restoration and floating stacking

---

## Invariants

### Window Containment

Each window appears in exactly one container based on kind:
- Kind::Tiled → exactly one `Workspace::windows` vector
- Kind::Floating → `floating_windows_`
- Kind::Dock → `dock_windows_`
- Kind::Desktop → `desktop_windows_`

All managed windows also appear in `clients_` (unified source of truth). Container ↔ Client kind consistency is enforced by `assert_floating_consistency()` and `assert_container_consistency()` in debug builds.

### State Synchronization

1. **Client ↔ EWMH**: Every state flag change updates the corresponding EWMH property
2. **Client ↔ ICCCM**: `WM_STATE` tracks iconic state
3. **Workspace ↔ Focus**: `workspace.focused_window` tracks last-focused (best-effort hint)

### Client Management

- If kind is Tiled/Floating: `client.monitor < monitors.size()` and `client.workspace < monitors[monitor].workspaces.size()`
- Dock/Desktop monitor/workspace fields are meaningless
- `client.order` values are unique and monotonically increasing (assigned via `next_client_order_++`)

### Focus Consistency

If `active_window_ != XCB_NONE`:
- Window exists in `clients_`
- Window is NOT iconic, NOT hidden, NOT on a hidden workspace

Focus eligibility: `kind != Dock && kind != Desktop && (accepts_input_focus || supports_take_focus)`

### Client State Consistency

- NOT (above AND below) — mutually exclusive
- iconic ⇒ hidden for NON-STICKY windows (sticky exception: `hide_window()` returns early)
- hidden does NOT ⇒ iconic (workspace switches hide without iconifying)
- hidden = true ⇒ window at `OFF_SCREEN_X`; hidden = false ⇒ on-screen

### Workspace Consistency

- Each tiled window appears in exactly one workspace (no duplicates)
- Sticky windows appear in one `workspace.windows` vector (visible on all via `hide_window()` skip)
- `workspace.focused_window` is a best-effort hint, updated via `fixup_workspace_focus()` on removal (selects last non-iconic window, or `XCB_NONE`)
- `select_focus_candidate()` validates existence and eligibility before using; falls back to MRU

### Desktop Index Validity

Desktop index < monitors × workspaces_per_monitor, OR 0xFFFFFFFF (sticky).

### Fullscreen

Fullscreen windows are excluded from tiling layout (`rearrange_monitor()` filters them before layout). Focus transitions do not reapply fullscreen geometry.

### Visibility

Window is visible iff: NOT hidden, NOT showing_desktop, NOT iconic, AND (sticky OR workspace == current).

---

## Visibility and Off-Screen Positioning

LWM uses off-screen positioning (`OFF_SCREEN_X = -20000`) instead of unmap/map cycles. This avoids GPU rendering issues with Chromium, Electron, and Qt/OpenGL applications. See [SPEC_CLARIFICATIONS.md §8](SPEC_CLARIFICATIONS.md#8-window-visibility-off-screen-positioning) for rationale and comparison.

**Key consequence**: WM never calls `xcb_unmap_window()`, so ALL `UnmapNotify` events are client-initiated withdraw requests.

**hide_window()**: Returns early for sticky windows (never hidden). Sets `client.hidden=true`, moves to `OFF_SCREEN_X`.

**show_window()**: Sets `client.hidden=false` only. Caller must restore geometry via `rearrange_monitor()` (tiled) or `apply_floating_geometry()` (floating).

---

## Workspace Switching

All workspace switching flows converge on `perform_workspace_switch(WorkspaceSwitchContext)` (src/lwm/wm_workspace.cpp):

1. Set `monitor.previous/current_workspace`
2. Hide floating windows (old ws, non-sticky)
3. Hide tiled windows (old ws, non-sticky)
4. **Flush X connection** (critical: ensures hides apply before shows)
5. Update EWMH current desktop
6. `rearrange_monitor()` (shows + lays out new workspace)
7. `update_floating_visibility()`

Three callers: `switch_workspace()`, `switch_to_ewmh_desktop()`, and `focus_any_window()` (when focus triggers workspace change). Each adds its own focus restoration after the switch.
