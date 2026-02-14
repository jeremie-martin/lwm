# LWM Window State Machine

> **Documentation Navigation**
> - Previous: [IMPLEMENTATION.md](IMPLEMENTATION.md) (Architecture overview)
> - Related: [EVENT_HANDLING.md](EVENT_HANDLING.md) (Event handling) | [BEHAVIOR.md](BEHAVIOR.md) (User-facing behavior)

This document defines the complete window state machine, state transitions, and lifecycle management for LWM. For architecture details, see [IMPLEMENTATION.md](IMPLEMENTATION.md). For event handling specifications, see [EVENT_HANDLING.md](EVENT_HANDLING.md).

---

## Table of Contents
1. [Window States](#window-states)
2. [State Model](#state-model)
3. [State Transitions](#state-transitions)
4. [Window Lifecycle](#window-lifecycle)
5. [Specific State Transitions](#specific-state-transitions)
6. [Focus System](#focus-system)

---

## Window States

### Window Kind Classification

LWM classifies windows by EWMH window type and properties:

| Kind | Description | EWMH Window Type | Tiled/Floating | Visibility | Focus |
|------|-------------|-------------------|----------------|------------|--------|
| **Tiled** | Participates in tiling layout | NORMAL (default) | Workspace-controlled | Yes |
| **Floating** | Independent positioning | DIALOG, UTILITY, TOOLBAR, SPLASH | Workspace-controlled | Yes |
| **Dock** | Panel/bar (reserves strut) | DOCK | Always visible | No |
| **Desktop** | Background layer | DESKTOP | Always visible | No |
| **Popup** | Menu/tooltip/notification | MENU, TOOLTIP, NOTIFICATION, POPUP_MENU, DROPDOWN_MENU, COMBO, DND | Never managed | No |

**Popup windows** are mapped directly but NOT managed. They bypass the state machine entirely.

### State Flags

Each Client maintains state flags (src/lwm/core/types.hpp:118-134):

| Flag | Purpose | _NET_WM_STATE Atom | Conflict Rules |
|------|---------|-------------------|---------------|
| `hidden` | Window moved off-screen | (internal) | - |
| `fullscreen` | Fullscreen mode | FULLSCREEN | Supersedes maximized |
| `above` | Stacks above normal windows | ABOVE | Mutually exclusive with below |
| `below` | Stacks below normal windows | BELOW | Mutually exclusive with above |
| `iconic` | Minimized (user action) | HIDDEN | - |
| `sticky` | Visible on all workspaces | STICKY | - |
| `maximized_horz` | Maximized horizontally | MAXIMIZED_HORZ | Ignored when fullscreen |
| `maximized_vert` | Maximized vertically | MAXIMIZED_VERT | Ignored when fullscreen |
| `shaded` | Show only title bar | SHADED | Implemented as iconify |
| `modal` | Modal dialog | MODAL | Sets above flag |
| `skip_taskbar` | Exclude from taskbar | SKIP_TASKBAR | - |
| `skip_pager` | Exclude from pager | SKIP_PAGER | - |
| `demands_attention` | Urgency indicator | DEMANDS_ATTENTION | - |

---

## State Model

### Visual State Diagram

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
                ┌─────▼─────────┐  ┌───▼─────────┐
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
```

### State Model Notes

**Top-level window states**: UNMANAGED, MANAGED (with VISIBLE/ICONIC substates), WITHDRAWN (ICCCM WM_STATE)

**State definitions**:
- **UNMANAGED**: Window not yet managed
- **MANAGED**: Window is managed (in clients_ registry)
  - **VISIBLE**: Logically visible (`!iconic && !hidden`)
  - **ICONIC**: Minimized by user action
- **WITHDRAWN**: Withdrawn from WM (client-initiated)

**State flag relationships**:
- **`hidden`**: Physical state flag (positioned off-screen at OFF_SCREEN_X = -20000).
- **`iconic`**: Logical state flag (minimized by user, WM_STATE Iconic).
- **iconic ⇒ hidden for NON-STICKY windows**: Minimized windows off-screen; sticky windows exception.
- **hidden does NOT ⇒ iconic**: Workspace switches hide windows without iconifying.
- **hidden=false ⇒ iconic=false for NON-STICKY windows**: On-screen windows cannot be minimized.
- **Sticky window exception**: Iconic sticky windows have `iconic=true` but `hidden=false` (hide_window() returns early).

**State modifiers**: Apply to VISIBLE windows: fullscreen, above, below, sticky, maximized, shaded, modal.
- `fullscreen` combines with VISIBLE state; fullscreen windows are excluded from tiling.

### State Truth Table

| hidden | iconic | fullscreen | Visible On-Screen? | Valid? | Notes |
|--------|--------|-----------|-------------------|--------|-------|
| true   | true   | false     | No                | Yes    | Iconic (minimized) window |
| true   | true   | true      | No                | Yes    | Iconic fullscreen window (fullscreen flag preserved while off-screen) |
| true   | false  | false     | No                | Yes    | Hidden (workspace switch) |
| true   | false  | true      | No                | Yes    | Fullscreen on non-visible workspace |
| false  | false  | false     | Yes               | Yes    | Normal visible window |
| false  | false  | true      | Yes               | Yes    | Visible fullscreen window |
| false  | true   | any       | Yes (on-screen, marked minimized) | **SPECIAL** | Only for sticky windows: hide_window() returns early |

**fullscreen+iconic**: Window retains fullscreen flag while off-screen until deiconified (fullscreen and iconic are NOT mutually exclusive)

### State Conflicts

- **Modal and Above**: Modal windows automatically stacked above others (setting modal also sets above). However, above flag can be set independently without making window modal. This is one-way coupling: modal ⇒ above, but above ⇏ modal.
- **Fullscreen and Maximized**: Fullscreen supersedes maximized. When fullscreen enabled, maximized flags are cleared. Maximization changes are ignored while window is fullscreen.
- **Above and Below**: Mutually exclusive - window cannot be both above and below simultaneously.
- **Fullscreen and Iconic**: Windows can be fullscreen while iconic (off-screen). Fullscreen geometry is applied when window becomes deiconified and on current workspace.

---

## State Transitions

### State Transition Summary

| From State | To State | Trigger | Key Actions |
|------------|-----------|---------|-------------|
| UNMANAGED | MANAGED | MapRequest | Create Client, classify, configure, map window |
| MANAGED | WITHDRAWN | UnmapNotify/DestroyNotify | WM_STATE Withdrawn, erase from clients_, remove from workspace |
| VISIBLE | ICONIC | iconify_window() | Set iconic=true, hide_window(), WM_STATE Iconic |
| ICONIC | VISIBLE | deiconify_window() | Set iconic=false, show_window(), restore geometry |
| VISIBLE | FULLSCREEN | set_fullscreen(true) | Save geometry, set fullscreen flag, apply fullscreen geometry |
| FULLSCREEN | VISIBLE | set_fullscreen(false) | Restore geometry, clear fullscreen flag |
| VISIBLE | MAXIMIZED | set_window_maximized() | Save geometry, set maximized flags, apply maximized geometry |
| MAXIMIZED | VISIBLE | set_window_maximized(false) | Restore geometry, clear maximized flags |
| VISIBLE | ABOVE | set_window_above(true) | Set above flag, restack above normal windows |
| ABOVE | VISIBLE | set_window_above(false) | Clear above flag, restack normally |
| VISIBLE | BELOW | set_window_below(true) | Set below flag, restack below all windows |
| BELOW | VISIBLE | set_window_below(false) | Clear below flag, restack normally |
| VISIBLE | STICKY | set_window_sticky(true) | Set sticky flag, _NET_WM_DESKTOP=0xFFFFFFFF |
| STICKY | VISIBLE | set_window_sticky(false) | Clear sticky flag, _NET_WM_DESKTOP=actual workspace |
| VISIBLE | SHOWING_DESKTOP | _NET_SHOWING_DESKTOP true | Hide non-sticky windows, clear focus |
| SHOWING_DESKTOP | VISIBLE | _NET_SHOWING_DESKTOP false | Show hidden windows, rearrange, focus_or_fallback() |
| FOCUS | NO_FOCUS | Last window removed, empty workspace, pointer on root | active_window_=XCB_NONE, clear_focus() |
| NO_FOCUS | FOCUS | Focus request, EnterNotify, ButtonPress | Set active_window_, focus window |
| FOCUS | FOCUS | focus_next/prev | Cycle to next/previous eligible window |

---

## Window Lifecycle

### Complete Lifecycle Flow

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
If Floating: Determine placement, store in Client.floating_geometry
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
xcb_map_window() (always, even if will be hidden) (src/lwm/wm.cpp:674)
    ↓
If start_iconic or not visible: hide_window() (move off-screen)
    ↓
Apply non-geometry states (above, below, skip_*)
    ↓
Update _NET_CLIENT_LIST
    ↓
[Window active] → Focus, drag, state changes, workspace moves
    ↓
UnmapNotify / DestroyNotify
    ↓
handle_window_removal()
    ├─ Set WM_STATE = Withdrawn
    ├─ Erase pending_kills_, pending_pings_
    ├─ Erase clients_[window]
    ├─ Remove from workspace.windows or floating_windows_
    ├─ Update workspace.focused_window (set to workspace.windows.back() or XCB_NONE if empty)
    ├─ If was active_window_:
    │  ├─ If removed window's workspace is the **current workspace** of its monitor AND that monitor is the currently focused monitor:
    │  │  └─ focus_or_fallback(removed window's monitor)
    │  └─ Else:
    │     └─ clear_focus()
    └─ Rearrange monitor, update _NET_CLIENT_LIST
```

**Focus restoration note**: LWM restores focus only when the removed window was on the **current workspace** of its monitor AND that monitor is the currently focused monitor. The workspace.focused_window update sets it to `workspace.windows.back()` or XCB_NONE if empty.

---

## Specific State Transitions

### Fullscreen Transition

**Implementation**: src/lwm/wm.cpp:799

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

**Fullscreen Window Visibility**:
- Fullscreen windows on non-visible workspaces have `hidden=true` (off-screen) but `fullscreen=true`
- When workspace becomes visible, `show_window()` and `apply_fullscreen_if_needed()` restore on-screen geometry
- During showing_desktop mode, fullscreen windows (non-sticky) hidden like normal windows

**apply_fullscreen_if_needed() Preconditions** (src/lwm/wm.cpp):
1. Window must be fullscreen (`client.fullscreen == true`)
2. Window must not be iconic (`client.iconic == false`)
3. Client must exist in `clients_`
4. Client must have valid monitor index (`client.monitor < monitors_.size()`)
5. Client's workspace must match the monitor's current workspace

If any precondition fails, the function returns early without applying geometry.

### Maximized Transition

**Implementation**: src/lwm/wm.cpp:981

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

**Note**: Tiled windows ignore maximize state (layout controls geometry). Maximize state flags ARE stored and `maximize_restore` geometry IS saved when maximizing tiled windows. This preserves state for future floating conversion. Maximize state changes are ignored when window is fullscreen.

### Iconify Transition

**Implementation**: src/lwm/wm.cpp:1288-1330

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
Flush connection
```

**Iconic Fullscreen Windows**:
- Iconic fullscreen windows have `fullscreen=true` but `hidden=true` (off-screen)
- When deiconified, `deiconify_window()` calls `apply_fullscreen_if_needed()` which:
  - Verifies window is fullscreen AND not iconic AND on current workspace
  - Sends sync request
  - Configures window to fullscreen geometry
  - Sends synthetic ConfigureNotify
  - Restacks window above all
- Iconic fullscreen windows on non-current workspaces remain off-screen until both deiconified AND workspace becomes active

### Deiconify Transition

**Implementation**: src/lwm/wm.cpp:1333-1367

```
[Trigger: Focus request or explicit action]
    ↓
deiconify_window(window, focus)
    ↓
Set client.iconic = false
    ↓
Set WM_STATE = NormalState
    ↓
Clear _NET_WM_STATE_HIDDEN
    ↓
If floating:
    └─ update_floating_visibility(client->monitor)
    ↓
If tiled:
    └─ rearrange_monitor(monitors_[client->monitor])
    ↓
If focus && on current workspace: focus_any_window()
    ↓
apply_fullscreen_if_needed(window)
    ↓
Flush connection
```

### Sticky Toggle

**Implementation**: src/lwm/wm.cpp:948

```
[Trigger: keybind or _NET_WM_STATE]
    ↓
set_window_sticky(window, enabled)
    ↓
If enabled:
    ├─ Set client.sticky = true
    ├─ Set _NET_WM_STICKY = true
    ├─ Set _NET_WM_DESKTOP = 0xFFFFFFFF
    └─ Update visibility (show on all workspaces)
    ↓
If disabled:
    ├─ Set client.sticky = false
    ├─ Clear _NET_WM_STICKY
    ├─ Set _NET_WM_DESKTOP = actual workspace
    └─ Update visibility (show only on current workspace)
```

**Sticky window behavior**: See [IMPLEMENTATION.md §6](IMPLEMENTATION.md#6-visibility-and-off-screen-positioning)

**Edge cases for sticky toggle**:

**Iconic Window Becoming Sticky**:
- If `set_window_sticky(true)` is called on an iconic (off-screen, hidden=true) window:
   - sticky flag is set
   - window remains off-screen (hidden=true, at OFF_SCREEN_X)
   - No call to show_window() occurs
   - When deiconified, window becomes visible on current workspace, then behaves as sticky on subsequent workspace switches

**Sticky Window Becoming Non-Sticky**:
- If `set_window_sticky(false)` is called on an iconic window:
   - sticky flag is cleared
   - _NET_WM_DESKTOP is set to actual workspace (the workspace where the window was when it became sticky)
   - Window remains iconic (still off-screen)
   - When deiconified, window becomes visible on its assigned workspace only

**Fullscreen Window Becoming Sticky**:
- If sticky is toggled on a fullscreen window:
   - fullscreen flag remains set
   - sticky flag is set/cleared
   - If sticky on non-current workspace: window is off-screen (hidden) but fullscreen flag is set
   - When switching to its workspace: apply_fullscreen_if_needed() applies fullscreen geometry
- Fullscreen windows respect sticky flag for workspace visibility (like normal windows)

---

## Focus System

### Focus Assignment

**focus_any_window(window)** - Unified entry point (src/lwm/wm_focus.cpp):
1. Check not showing_desktop
2. Check is_focus_eligible
3. Look up Client, determine tiled vs floating via `Client::kind`
4. If iconic: deiconify first (clears iconic flag)
5. **Tiled path**: Call `focus_window_state()` (pure decision function) — apply result, call `perform_workspace_switch()` if workspace changed
6. **Floating path**: Update workspace tracking + call `perform_workspace_switch()` if needed + MRU promotion + set `active_window_`
7. Update EWMH current desktop
8. Clear previous window's border (targeted, not all borders)
9. Apply focused border visuals only when target window is not fullscreen
10. Send WM_TAKE_FOCUS if supported
11. Set X input focus
12. If floating: stack above (XCB_STACK_MODE_ABOVE)
13. Update _NET_ACTIVE_WINDOW, _NET_WM_STATE_FOCUSED
14. Update user_time, restack transients, update _NET_CLIENT_LIST

**restack_transients()** - Restacks modal/transient windows above parent (src/lwm/wm.cpp):
- Identifies transients via client.transient_for field.
- Only restacks transients that are:
    - Visible (client.hidden == false).
    - Not iconic.
    - On current workspace.
- Skips transients on other workspaces.
- Ensures modal windows stay above parent during focus changes.

**Fullscreen focus invariant**:
- Focus transitions do not reapply fullscreen geometry (`fullscreen_policy::ApplyContext::FocusTransition` is excluded).
- Fullscreen windows keep zero border width when focus leaves and returns.

**focus_or_fallback(monitor)** - Smart focus selection (src/lwm/wm_focus.cpp):
1. Build candidates (order of preference):
    - `workspace.focused_window` if exists in workspace AND eligible (validated to exist).
    - Current workspace tiled windows (reverse iteration = MRU).
    - Sticky tiled windows from other workspaces (reverse iteration).
    - Floating windows visible on monitor (reverse iteration = MRU).
2. Call focus_policy::select_focus_candidate().
3. Focus selected or clear focus.

**Empty workspace behavior**: When switching to an empty workspace or when the last window on a workspace is removed, `focus_or_fallback()` finds no candidates and clears focus (sets `active_window_ = XCB_NONE`).

### Focus Restoration

LWM uses a two-tier focus restoration model:

**1. Workspace focus memory** (tiled windows only):
- Each Workspace.focused_window stores the last-focused tiled window
- When updating after window removal, LWM sets it to `workspace.windows.back()` (most recent window) or XCB_NONE if empty
- This is a best-effort value that may become stale
- Callers validate existence and eligibility before using it

**2. Global floating window search** (MRU order):
- When workspace focus memory is not applicable or validation fails, LWM searches floating_windows_ in reverse iteration (MRU) for fallback focus restoration

**Focus restoration on window removal** (src/lwm/wm_events.cpp:443-449, src/lwm/wm.cpp:749-897):
- LWM calls handle_window_removal() when UnmapNotify or DestroyNotify is received
- handle_window_removal() sets WM_STATE = Withdrawn, erases from clients_, removes from workspace/floating, and updates workspace.focused_window
- If the removed window was active_window_:
  - If removed window's workspace is the **current workspace** of its monitor AND that monitor is the currently focused monitor: LWM calls focus_or_fallback(removed window's monitor) to restore focus
  - Otherwise: LWM calls clear_focus() (different monitor/workspace)

**Focus restoration on workspace switch**:
- When switching to a workspace that contains the active window, that window remains focused
- When switching to a different workspace, LWM calls focus_or_fallback() to select a new focus target on the new workspace

### Focus Barriers

The following conditions can prevent focus even for focus-eligible windows:
1. **showing_desktop_ == true** - Blocks all focus except desktop mode exit
2. **client.hidden == true** - Off-screen windows (filtered in event handlers)
3. **iconic windows** - For NON-STICKY windows: iconic ⇒ hidden, so they are blocked by hidden check. For sticky windows: iconic windows have hidden=false but are still blocked from receiving focus because deiconification is required before focusing; focus operations explicitly check and handle iconic windows.
4. **Dock and Desktop kinds** - These window kinds are never focus-eligible by is_focus_eligible() definition.
5. **Windows on non-visible workspaces** - Wrong workspace

### Focus Stealing Prevention

**_NET_ACTIVE_WINDOW source=1 (application)** (src/lwm/wm_ewmh.cpp):
- LWM compares request timestamp with active_window's user_time
- If timestamp == 0 → Focus allowed (no timestamp check)
- If request is older → LWM sets demands_attention instead of focusing
- Only newer timestamps can steal focus

**User Time Window Indirection**:
- `_NET_WM_USER_TIME` property may be on Client.user_time_window instead of main window
- `get_user_time()` implementation checks Client.user_time_window and queries property accordingly
- LWM tracks PropertyNotify events on user_time_window during window management
- Focus stealing prevention uses parent Client.user_time for comparison

**_NET_ACTIVE_WINDOW source=2 (pager)**:
- LWM always allows pager focus requests (no timestamp check)

---

## Related Documents

- **[IMPLEMENTATION.md](IMPLEMENTATION.md)** - Architecture, data structures, invariants
- **[EVENT_HANDLING.md](EVENT_HANDLING.md)** - Event-by-event handling specifications
- **[BEHAVIOR.md](BEHAVIOR.md)** - User-facing behavior (focus, workspaces, monitors)
- **[COMPLIANCE.md](COMPLIANCE.md)** - Protocol obligations (ICCCM/EWMH)
- **[SPEC_CLARIFICATIONS.md](SPEC_CLARIFICATIONS.md)** - Design decisions on spec ambiguities
