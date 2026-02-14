# LWM Event Handling

> **Documentation Navigation**
> - Previous: [IMPLEMENTATION.md](IMPLEMENTATION.md) (Architecture overview)
> - Related: [STATE_MACHINE.md](STATE_MACHINE.md) (Window states) | [BEHAVIOR.md](BEHAVIOR.md) (User-facing behavior)

This document provides complete event-by-event handling specifications for LWM. For window state machine details, see [STATE_MACHINE.md](STATE_MACHINE.md). For architecture details, see [IMPLEMENTATION.md](IMPLEMENTATION.md).

---

## Table of Contents
1. [Event Loop](#event-loop)
2. [Event Handlers](#event-handlers)
3. [Time Tracking](#time-tracking)
4. [Interaction Modes](#interaction-modes)
5. [Workspace Management](#workspace-management)
6. [Monitor Behavior](#monitor-behavior)

---

## Event Loop

### Main Loop Structure

**Implementation**: src/lwm/wm.cpp:98-xxx

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

### Timeout Calculation

- Derived from pending operations (PING_TIMEOUT, KILL_TIMEOUT)
- Prevents indefinite blocking when waiting for X events
- Allows periodic timeout processing even without events

---

## Event Handlers

### Event Handler Summary

All event handlers are in src/lwm/wm_events.cpp.

| Event | Handler | Effect | Source |
|-------|---------|--------|---------|
| MapRequest | handle_map_request | Classifies and manages window. Handles deiconify if already managed | wm_events.cpp |
| UnmapNotify | handle_window_removal | With off-screen visibility, ALL unmaps are client-initiated withdraw requests | wm_events.cpp:66-73 |
| DestroyNotify | handle_window_removal | Unmanages window | wm_events.cpp:74-79 |
| EnterNotify | handle_enter_notify | Implements focus-follows-mouse. Ignores hidden windows. Updates focused_monitor on root window entry | wm_events.cpp:80-82 |
| MotionNotify | handle_motion_notify | Re-establishes focus in window. Ignores hidden windows. Updates focused_monitor if crossing monitor | wm_events.cpp:83-84 |
| ButtonPress | handle_button_press | Focuses window and begins drag. Ignores hidden windows. Updates focused_monitor | wm_events.cpp:85-87 |
| ButtonRelease | handle_button_release | Ends drag | wm_events.cpp:88-89 |
| KeyPress | handle_key_press | Executes keybind actions | wm_events.cpp:90-92 |
| KeyRelease | handle_key_release | Tracks for auto-repeat detection | wm_events.cpp:93-94 |
| ClientMessage | handle_client_message | Handles EWMH/ICCCM commands | wm_events.cpp:95-96 |
| ConfigureRequest | handle_configure_request | Handles geometry requests | wm_events.cpp:97-98 |
| PropertyNotify | handle_property_notify | Tracks state changes, title, hints, sync counter, struts, and user_time_window indirection for _NET_WM_USER_TIME | wm_events.cpp:99-100 |
| Expose | handle_expose | No-op (internal bar removed). External bars are handled via struts. | wm_events.cpp:101-102 |
| SelectionClear | (handled in run) | New WM taking over. Exits | wm.cpp:98-105 |
| RANDR change | handle_randr_screen_change | Handles monitor hotplug | wm_events.cpp:55-59 |

### Key Event Handlers

#### MapRequest

**Implementation**: src/lwm/wm_events.cpp:141-442

**Behavior**:
1. Check if window already managed → call deiconify_window() (focus is only set if window is on current workspace of focused monitor, otherwise just deiconifies).
2. If override-redirect → Ignore (menus, dropdowns).
3. classify_window() → Kind (Tiled/Floating/Dock/Desktop/Popup) (if Popup: Map directly, NOT MANAGED, return).
4. apply_window_rules() (can override EWMH states except Dock/Desktop/Popup types).
5. Create Client record (order = next_client_order_++), call parse_initial_ewmh_state(client).
6. Read iconic state (precedence: _NET_WM_STATE_HIDDEN > WM_HINTS.initial_state).
7. If Floating: Determine placement, store in Client.floating_geometry.
8. Add to workspace.windows (Tiled) or floating_windows_ (Floating).
9. Set WM_STATE (Normal/Iconic).
10. Apply geometry-affecting states (fullscreen, maximized).
11. Configure geometry (apply rule geometry, fullscreen geometry, or placement logic).
12. Send synthetic ConfigureNotify.
13. xcb_map_window() (always, even if will be hidden).
14. If start_iconic or not visible: hide_window() (move off-screen).
15. apply_post_manage_states() — applies non-geometry states (sticky, above, below, modal, skip_*).
16. Apply maximized/shaded (tiled: post-map; floating: pre-map).
17. Update _NET_CLIENT_LIST.

#### UnmapNotify

**Implementation**: src/lwm/wm_events.cpp:66-73 (case), 443-450 (function)

**Behavior with off-screen visibility**:
- ALL UnmapNotify events are client-initiated withdraw requests
- WM never calls xcb_unmap_window(), so unmaps are always client actions
- Calls handle_window_removal() to unmanage window

**handle_window_removal() actions**:
1. Look up client->kind; dispatch to the appropriate unmanage function (Tiled/Floating/Dock/Desktop).
2. Set WM_STATE = Withdrawn.
3. Erase pending_kills_, pending_pings_.
4. Erase clients_[window].
5. Remove from workspace.windows or floating_windows_.
6. Update workspace.focused_window via `fixup_workspace_focus()` (selects last non-iconic window, or XCB_NONE if empty).
7. If was active_window_:
    - If removed window's workspace is the **current workspace** of its monitor AND that monitor is the currently focused monitor: focus_or_fallback(removed window's monitor).
    - Else: clear_focus().
8. Rearrange monitor, update _NET_CLIENT_LIST.

#### EnterNotify

**Implementation**: src/lwm/wm_events.cpp:451-523

**Behavior**:
1. Extract event timestamp (via extract_event_time()).
2. If mode != NORMAL: return.
3. If detail == INFERIOR: return (spurious event).
4. If window has no client: return.
5. If client.hidden == true: return (off-screen windows don't receive focus).
6. If not is_focus_eligible(window): return.
7. Update focused_monitor_ to monitor containing pointer.
8. Focus window (calls focus_any_window()).

**Note**: Iconic windows are filtered via hidden check for non-sticky windows (iconic ⇒ hidden).

#### MotionNotify

**Implementation**: src/lwm/wm_events.cpp

**Behavior**:
1. Extract event timestamp.
2. If in DRAG mode: call update_drag() and return.
3. If window has no client: return.
4. If client.hidden == true: return.
5. If not is_focus_eligible(window): return.
6. If window == active_window_: return (already focused).
7. Update focused_monitor_ to monitor containing pointer (if crossed boundary).
8. Re-focus window (calls focus_any_window()).

**Purpose**: Re-establishes focus if lost (e.g., due to focus stealing prevention).

#### ButtonPress

**Implementation**: src/lwm/wm_events.cpp

**Behavior**:
1. Extract event timestamp.
2. If in DRAG mode: call update_drag() and return.
3. Find matching mousebinding (modifier + button).
4. If window is root window:
    - Clear focus (active_window_ = XCB_NONE).
    - Execute mousebind action (if any).
    - Return.
5. If window has no client: return.
6. If client.hidden == true: return.
7. Update focused_monitor_ to monitor containing pointer.
8. Focus window (calls focus_any_window()).
9. If mousebind action is "drag_window":
    - If tiled: begin_tiled_drag().
    - If floating: begin_drag().
10. Else: Execute mousebind action.

#### ConfigureRequest

**Implementation**: src/lwm/wm_events.cpp:1222-1306

**Behavior**:

**For tiled/managed windows**:
- Acknowledge with synthetic ConfigureNotify reflecting WM-determined geometry.
- Apply size hint constraints.
- Do NOT actually change geometry (tiling layout controls it).

**For floating windows**:
- Apply requested changes within size hint constraints.
- Honor position requests if reasonable.
- Apply to Client.floating_geometry and call apply_floating_geometry().

**For fullscreen windows**:
- Apply fullscreen geometry (ignores client request).
- Ensures fullscreen state is maintained.

**For hidden windows**:
- Configure request is applied normally.
- If request changes x coordinate, window moves from OFF_SCREEN_X to the new position.
- Subsequent visibility updates (via rearrange_monitor, workspace switch, or other triggers) may re-hide the window if it should not be visible.
- This is intentional behavior: window briefly becomes visible at requested position, then visibility management reasserts correct state.

**For override-redirect windows**:
- Ignore (not managed).

#### PropertyNotify

**Implementation**: src/lwm/wm_events.cpp:1307-1406

**Behavior**: Tracks property changes for:

- **State changes**: _NET_WM_STATE, WM_STATE
- **Title**: WM_NAME, _NET_WM_NAME, WM_ICON_NAME, _NET_WM_ICON_NAME
- **Hints**: WM_NORMAL_HINTS, WM_HINTS
- **Struts**: _NET_WM_STRUT, _NET_WM_STRUT_PARTIAL
- **Sync counter**: _NET_WM_SYNC_REQUEST_COUNTER
- **User time window**: _NET_WM_USER_TIME_WINDOW
- **User time**: _NET_WM_USER_TIME (on main window or user_time_window)

**User time window indirection**:
- If PropertyNotify arrives on user_time_window for _NET_WM_USER_TIME:
    - Find parent Client via get_client().
    - Update parent Client.user_time.
- If no parent found (window unmanaged): Silently ignore.
- Race condition: If window is unmanaged after finding match but before get_client(), drop update.

#### ClientMessage

**Implementation**: src/lwm/wm_events.cpp (dispatcher + sub-handlers: handle_wm_state_change, handle_active_window_request, handle_desktop_change, handle_moveresize_window, handle_wm_moveresize, handle_showing_desktop)

**Handled messages**:
- _NET_ACTIVE_WINDOW (activate window, switch desktop if needed, apply focus stealing prevention)
- _NET_CLOSE_WINDOW (initiate close, send WM_DELETE_WINDOW or destroy)
- _NET_WM_STATE (toggle window states)
- _NET_WM_DESKTOP (move window to desktop)
- _NET_CURRENT_DESKTOP (switch desktop)
- _NET_SHOWING_DESKTOP (toggle desktop mode)
- _NET_WM_MOVERESIZE (initiate interactive move/resize for floating windows)
- _NET_RESTACK_WINDOW (restack window)
- _NET_FULLSCREEN_MONITORS (set fullscreen monitor span)
- WM_PROTOCOLS (handles WM_DELETE_WINDOW, WM_TAKE_FOCUS, _NET_WM_PING, _NET_WM_SYNC_REQUEST)

---

## Time Tracking

### last_event_time_

**Implementation**: src/lwm/wm_events.cpp:24-44

**Purpose**: Tracks most recent event timestamp from X server

**Updated from**:
- XCB_KEY_PRESS
- XCB_KEY_RELEASE
- XCB_BUTTON_PRESS
- XCB_BUTTON_RELEASE
- XCB_MOTION_NOTIFY
- XCB_ENTER_NOTIFY
- XCB_LEAVE_NOTIFY
- XCB_PROPERTY_NOTIFY

**Used for**:
1. Focus stealing prevention (user_time comparison)
2. Auto-repeat detection (same KeyPress/KeyRelease timestamp)
3. SetInputFocus timestamp (ensures proper focus ordering)

**Note**: Using CurrentTime for SetInputFocus can cause focus to be ignored or reordered incorrectly. LWM always uses last_event_time_.

---

## Interaction Modes

LWM has three global interaction modes that affect event processing:

### NORMAL Mode

Default interaction mode (drag_state_.active = false, showing_desktop_ = false).

**Behavior**:
- All events processed normally
- EnterNotify: Focus-follows-mouse applies
- MotionNotify: Re-establish focus if lost
- ButtonPress: Focus window, begin drag

### DRAG Mode

Drag state active (drag_state_.active = true).

**Entry conditions**:
- begin_tiled_drag() called on tiled window
- begin_drag() called on floating window
- Both functions reject fullscreen windows and showing_desktop mode

**Behavior**:
- Ignores EnterNotify events (focus-follows-mouse disabled).
- Ignores MotionNotify events (except for drag updates via update_drag()).
- ButtonRelease calls end_drag().
- KeyPress events ARE processed: keybinds can execute during drag (e.g., toggle_fullscreen, kill_window).

**Exit conditions**:
- ButtonRelease → end_drag().
- Window destroyed during drag → end_drag() returns early (monitors check fails).
- All monitors disconnected → end_drag() returns early.

**Edge cases**:
- If window is destroyed during drag: drag_state_ is reset but window geometry is not restored.
- If all monitors disconnected: pointer is ungrabbed but window remains at last position.
- No error logging for these cases (intentional no-op).

### SHOWING_DESKTOP Mode

Desktop mode enabled (showing_desktop_ = true).

**Entry condition**:
- _NET_SHOWING_DESKTOP ClientMessage with value=1.

**Behavior**:
- LWM calls hide_window() for all non-sticky windows.
- hide_window() returns early for sticky windows, so they remain visible (physically on-screen but not focusable if iconic).
- Clears focus (active_window_ = XCB_NONE).
- rearrange_monitor() returns early (no layout calculations).
- Cannot start tiled drag operations.

**Exit condition**:
- _NET_SHOWING_DESKTOP ClientMessage with value=0.

**Iconic sticky windows during showing_desktop**: Iconic sticky windows (iconic=true, hidden=false) remain physically visible (on-screen position) during showing_desktop mode but cannot receive focus. They are not focusable because they are iconic, not because showing_desktop mode hides them.

---

## Workspace Management

### Workspace Switch

**Implementation**: src/lwm/wm_workspace.cpp

```
switch_workspace(target_ws)
├─ workspace_policy::validate_workspace_switch()
│  ├─ Validate workspace index (returns nullopt if invalid - out of bounds, negative, or same as current)
│  └─ Return WorkspaceSwitchResult{ old, target } (pure — does not mutate monitor)
├─ perform_workspace_switch({ monitor, old_ws, new_ws })
│  ├─ Update previous_workspace = old, current_workspace = new
│  ├─ hide_window() for floating windows (iterates global floating_windows_, filters by: on monitor, non-sticky, on old_workspace)
│  ├─ hide_window() for tiled windows (iterates old_workspace.windows directly, filters by: non-sticky)
│  ├─ conn_.flush()  ← Critical sync point!
│  ├─ update_ewmh_current_desktop()
│  ├─ rearrange_monitor(new workspace)
│  │  ├─ For each visible window: show_window() (clears hidden flag)
│  │  └─ layout_.arrange() (applies on-screen geometry)
│  └─ update_floating_visibility()  ← Show new workspace's floating windows, hide old ones
├─ focus_or_fallback(monitor)
└─ conn_.flush()  ← Final sync point after all updates
```

Note: `perform_workspace_switch()` is the unified core operation shared by `switch_workspace()`, `switch_to_ewmh_desktop()`, and `focus_any_window()` (when focusing triggers a workspace change).

**Critical Ordering**:
1. Hide old workspace windows (floating first, then tiled)
2. Flush X connection
3. Show new workspace via rearrange_monitor and update_floating_visibility

**Rationale for floating-first hiding**:
- Prevents visual glitches where old floating windows appear over new workspace content.
- Flush ensures hide configurations apply before rendering new workspace.
- Prevents "flash" artifacts during workspace transitions.

**Off-Screen Visibility**: Windows are hidden using hide_window(), NOT unmapped.

### Workspace Toggle

**Implementation**: src/lwm/wm_workspace.cpp:82-xxx

```
toggle_workspace()
├─ Check workspace count <= 1 → Return (no toggle with only 1 workspace)
├─ Check previous_workspace invalid or same as current → Return
├─ Auto-repeat detection (same keysym, same timestamp as KeyRelease)
├─ target = monitor.previous_workspace
└─ switch_workspace(target)
```

**Early Return Conditions**:
- Workspace count <= 1: No toggle possible with only one workspace.
- previous_workspace invalid: After monitor hotplug or workspace configuration changes, previous_workspace may be out-of-bounds. Returns without switching (same as 'same as current' case).
- previous_workspace == current: Already on previous workspace, no action needed.

**Auto-Repeat Prevention**:
- X11 auto-repeat sends KeyRelease → KeyPress (identical timestamps).
- toggle_workspace() tracks last_toggle_keysym_ and last_toggle_release_time_.
- Blocks KeyPress if same keysym AND same timestamp as KeyRelease.
- After allowing new toggle, last_toggle_release_time_ is reset to 0.
- Prevents multiple workspace toggles from single key hold.

### Move Window to Workspace

**Implementation**: src/lwm/wm_workspace.cpp:xxx-xxx

**Tiled windows**:
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

**Floating windows**:
- Update Client.monitor, Client.workspace.
- Reposition to target monitor's working area.
- Update visibility on both monitors.
- Focus moved window if visible.
- Note: If floating window crosses monitor boundary via geometry update (not explicit move), `update_floating_monitor_for_geometry()` auto-assigns to new monitor without focus restoration.

---

## Monitor Behavior

### Monitor Hotplug

**Trigger**: RANDR screen change (monitor hotplug, resolution change)

**Implementation**: src/lwm/wm_events.cpp (handle_randr_screen_change calls src/lwm/wm.cpp functions)

```
handle_randr_screen_change()
├─ Exit fullscreen for all windows (prevents stale fullscreen_restore)
│  ├─ Clear fullscreen flag from all clients
│  ├─ Clear fullscreen_restore from all clients
│  └─ Clear fullscreen_monitors from all clients (indices invalid)
├─ Save window locations by monitor NAME (not index):
│  ├─ Tiled: monitor_name + workspace_index
│  └─ Floating: monitor_name + workspace_index + geometry
├─ Detect monitors (detect_monitors)
├─ If monitors_.empty() → create_fallback_monitor() (default monitor with screen dimensions)
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
└─ focus_or_fallback()
```

**Edge Cases**:
- All monitors disconnected: Creates fallback monitor with screen dimensions, name="default".
- Monitor name not found: Windows silently fall back to monitor 0.
- Focused monitor not found: Defaults to monitor 0.

**Rationale**:
- Saving by monitor NAME handles monitors being turned off/on (index changes but name persists).
- Exiting fullscreen before reconfiguration prevents stale geometries.
- Clearing fullscreen_monitors prevents invalid monitor indices after hotplug.

### Empty Monitor State

**When monitors_.empty() == true**:
- Occurs when all monitors are disconnected (no X11 outputs).
- Several operations handle this case:
    - `fullscreen_geometry_for_window()` returns empty geometry.
    - `end_drag()` aborts silently.
    - `begin_tiled_drag()` rejects.
    - `begin_drag()` on fullscreen windows rejects.
- Fallback behavior: Return default values or early returns without errors.
- No explicit user notification; WM continues operating with fallback monitor.

### Monitor Switching

**Explicit monitor switch** (via keybind: focus_monitor_left/right):
- Uses wrap_monitor_index() to cycle through monitors.
- Calls focus_or_fallback() on new monitor to restore appropriate focus.
- Updates focused_monitor_ to new monitor.
- Calls update_ewmh_current_desktop() to update EWMH state.
- Warps cursor to center of new monitor if warp_cursor_on_monitor_change configured.
- Early return if only 1 monitor exists.

**Automatic monitor switch** (via focus or window movement):
- Focusing window on different monitor: Implicitly changes focused_monitor_, window becomes focused.
- Moving focused window to different monitor: Updates focused_monitor_, window remains focused.
- Does NOT warp cursor (warping only for explicit switch).
- Moving non-focused window to different monitor: Does NOT change focus or focused_monitor_.

**Monitor Index Cycling** (wrap_monitor_index):
- Wraps monitor indices to stay within valid range.
- For positive direction: if idx >= monitors_.size(), wraps to 0.
- For negative direction: if idx < 0, wraps to monitors_.size() - 1.
- Returns clamped index that is always valid (or 0 if no monitors exist).

**Monitor Switching via Pointer** (update_focused_monitor_at_point):
- Updates focused_monitor_ to monitor containing pointer point
- Called on: EnterNotify on root window, MotionNotify crossing monitor boundary, ButtonPress on any window
- Clears focus when pointer crosses to a different monitor via root/empty-space traversal
- Preserves strict no-focus semantics for cross-monitor empty-space behavior

### Move Window to Monitor

**For floating windows** (move_to_monitor_left/right):
- Repositions to center of target monitor's working area using floating::place_floating().
- Updates Client.monitor/workspace and Client.floating_geometry.
- Updates _NET_WM_DESKTOP property.
- Updates floating visibility on both source and target monitors.
- Moves focused_monitor_ to target and calls focus_any_window().
- Warps cursor if enabled.

**For tiled windows**:
- Removes from source workspace.windows.
- Updates source workspace.focused_window to last remaining window (or XCB_NONE if empty).
- Adds to target monitor's current workspace.windows.
- Sets target workspace.focused_window to moved window.
- Rearranges both source and target monitors.
- Updates focused_monitor_ to target and calls focus_any_window().
- Warps cursor if enabled.

### Floating Window Monitor Auto-Assignment

**Function**: update_floating_monitor_for_geometry(window)

**When called**:
- After handle_configure_request() for floating windows.
- During drag operations (update_drag()).
- Any time floating window geometry changes.

**Behavior**:
- Calculates window center point: (x + width/2, y + height/2).
- Determines which monitor contains center (monitor_index_at_point()).
- If center moved to different monitor:
    - Updates Client.monitor to new monitor.
    - Updates Client.workspace to new monitor's current workspace.
    - Updates _NET_WM_DESKTOP property.
    - If window IS active window: Updates focused_monitor_, calls update_ewmh_current_desktop().
- Does NOT:
    - Call focus_or_fallback().
    - Update focused_monitor_ (for non-active windows).
    - Restack window.
    - Change focus state.
- If center not on any monitor (off all screens): No change to monitor assignment.
- This is intentional - window simply "belongs" to a different monitor after moving.

---

## Related Documents

- **[IMPLEMENTATION.md](IMPLEMENTATION.md)** - Architecture, data structures, invariants
- **[STATE_MACHINE.md](STATE_MACHINE.md)** - Complete window state machine and transitions
- **[BEHAVIOR.md](BEHAVIOR.md)** - User-facing behavior (focus, workspaces, monitors)
- **[COMPLIANCE.md](COMPLIANCE.md)** - Protocol obligations (ICCCM/EWMH)
- **[SPEC_CLARIFICATIONS.md](SPEC_CLARIFICATIONS.md)** - Design decisions on spec ambiguities
