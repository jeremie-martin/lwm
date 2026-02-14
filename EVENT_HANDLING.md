# LWM Event Handling

Event-by-event handling specifications. For window lifecycle and state transitions, see [STATE_MACHINE.md](STATE_MACHINE.md).

---

## Event Loop

```
run()
├─ while (running_)
│  ├─ Calculate timeout from pending operations
│  ├─ poll() for X events or timeout
│  ├─ while (event = xcb_poll_for_event())
│  │  └─ handle_event(*event)
│  ├─ handle_timeouts()
│  │  ├─ Clean up expired pending_pings_
│  │  └─ Force-kill windows with expired pending_kills_
│  └─ Check for X connection errors
```

---

## Event Handlers

All handlers in src/lwm/wm_events.cpp.

| Event | Handler | Effect |
|-------|---------|--------|
| MapRequest | handle_map_request | Classifies and manages window. Deiconifies if already managed |
| UnmapNotify | handle_window_removal | ALL unmaps are client-initiated withdraw requests (off-screen visibility) |
| DestroyNotify | handle_window_removal | Unmanages window |
| EnterNotify | handle_enter_notify | Focus-follows-mouse. Ignores hidden windows |
| MotionNotify | handle_motion_notify | Re-establishes focus. Ignores hidden windows |
| ButtonPress | handle_button_press | Focuses window, begins drag. Ignores hidden windows |
| ButtonRelease | handle_button_release | Ends drag |
| KeyPress | handle_key_press | Executes keybind actions |
| KeyRelease | handle_key_release | Tracks for auto-repeat detection |
| ClientMessage | handle_client_message | EWMH/ICCCM commands |
| ConfigureRequest | handle_configure_request | Geometry requests |
| PropertyNotify | handle_property_notify | State, title, hints, struts, sync counter, user_time |
| RANDR change | handle_randr_screen_change | Monitor hotplug |

### MapRequest

Window lifecycle is defined in [STATE_MACHINE.md §Window Lifecycle](STATE_MACHINE.md#window-lifecycle). Key points specific to the event handler:

1. If already managed → deiconify (focus only if on current workspace of focused monitor)
2. If override-redirect → ignore
3. classify_window() → Kind. Popups mapped directly, NOT MANAGED.
4. apply_window_rules() can override EWMH states except Dock/Desktop/Popup types

### EnterNotify

1. If mode != NORMAL or detail == INFERIOR: return (spurious)
2. If client.hidden: return (off-screen windows don't receive focus)
3. If not is_focus_eligible: return
4. Update focused_monitor_ to monitor containing pointer
5. focus_any_window()

### MotionNotify

1. If in DRAG mode: call update_drag() and return
2. If client.hidden or not focus_eligible: return
3. If window == active_window_: return
4. Re-focus window (focus_any_window)

**Purpose**: Re-establishes focus if lost (e.g., focus stealing prevention).

### ButtonPress

1. If root window: clear focus, execute mousebind action
2. If client.hidden: return
3. Update focused_monitor_, focus window
4. If drag_window action: begin_tiled_drag() or begin_drag()

### ConfigureRequest

- **Tiled**: Acknowledge with synthetic ConfigureNotify reflecting WM-determined geometry. Do NOT change geometry.
- **Floating**: Apply requested changes within size hint constraints. Update `Client.floating_geometry`.
- **Fullscreen**: Apply fullscreen geometry (ignores client request).
- **Hidden**: Configure applied normally. Window may briefly become visible; visibility management reasserts correct state.

### PropertyNotify

Tracks: `_NET_WM_STATE`, `WM_STATE`, `WM_NAME`/`_NET_WM_NAME`, `WM_NORMAL_HINTS`, `WM_HINTS`, struts, sync counter, `_NET_WM_USER_TIME_WINDOW`, `_NET_WM_USER_TIME`.

**User time window indirection**: If PropertyNotify arrives on `user_time_window` for `_NET_WM_USER_TIME`, find parent Client and update its `user_time`.

### ClientMessage

Handled messages: `_NET_ACTIVE_WINDOW`, `_NET_CLOSE_WINDOW`, `_NET_WM_STATE`, `_NET_WM_DESKTOP`, `_NET_CURRENT_DESKTOP`, `_NET_SHOWING_DESKTOP`, `_NET_WM_MOVERESIZE`, `_NET_RESTACK_WINDOW`, `_NET_FULLSCREEN_MONITORS`, `WM_PROTOCOLS`.

---

## Time Tracking

`last_event_time_` tracks the most recent X server timestamp. Updated from key/button/motion/enter/leave/property events.

Used for:
1. Focus stealing prevention (user_time comparison)
2. Auto-repeat detection (same KeyPress/KeyRelease timestamp)
3. SetInputFocus timestamp (using CurrentTime can cause focus reordering bugs)

---

## Interaction Modes

### DRAG Mode

Active when `drag_state_.active = true`. Both begin_tiled_drag() and begin_drag() reject fullscreen and showing_desktop.

- Ignores EnterNotify and MotionNotify (except for drag updates)
- KeyPress events ARE processed (keybinds work during drag)
- Exit: ButtonRelease → end_drag(). Window destroyed → drag reset silently.

### SHOWING_DESKTOP Mode

Entry: `_NET_SHOWING_DESKTOP` with value=1. Exit: value=0.

- `hide_window()` for all non-sticky windows; clears focus
- `rearrange_monitor()` returns early (no layout)
- Cannot start tiled drag operations
- Iconic sticky windows remain on-screen but cannot receive focus

---

## Workspace Management

### Workspace Switch

Detailed flow in [IMPLEMENTATION.md §Workspace Switching](IMPLEMENTATION.md#workspace-switching). Event-specific notes:

**Auto-repeat prevention** (toggle_workspace): X11 auto-repeat sends KeyRelease → KeyPress with identical timestamps. LWM tracks `last_toggle_keysym_` and `last_toggle_release_time_` to block spurious toggles.

### Move Window to Workspace

**Tiled**: `workspace_policy::move_tiled_window()` removes from source, adds to target, updates focused_window in both. Then: update Client fields, set `_NET_WM_DESKTOP`, hide if target not visible, rearrange, focus fallback in source.

**Floating**: Update Client.monitor/workspace, reposition to target working area, update visibility on both monitors, focus if visible.

---

## Monitor Behavior

### Monitor Hotplug

```
handle_randr_screen_change()
├─ Exit fullscreen for all windows (prevents stale geometry)
├─ Save window locations by monitor NAME (not index)
├─ detect_monitors() (or create_fallback_monitor if none)
├─ Update struts from dock windows
├─ Restore windows by name (fallback to monitor 0 if not found)
├─ Restore focused monitor by name (fallback to monitor 0)
├─ Update EWMH, rearrange all, focus_or_fallback()
```

**Why by name**: Monitor indices change on hotplug, but names persist.

### Monitor Switching

**Explicit** (focus_monitor_left/right): Wrap index, focus_or_fallback on new monitor, warp cursor if configured.

**Automatic** (via focus/window movement): Implicitly changes focused_monitor_. Does NOT warp cursor.

**Via pointer** (update_focused_monitor_at_point): Updates focused_monitor_ on root EnterNotify, MotionNotify crossing, ButtonPress. Clears focus when crossing to different monitor via empty space.

### Move Window to Monitor

- Floating: Reposition to target working area center, update Client fields, update visibility on both monitors, focus moved window, warp if enabled.
- Tiled: Remove from source workspace, add to target's current workspace, rearrange both, focus moved window, warp if enabled.

### Floating Monitor Auto-Assignment

`update_floating_monitor_for_geometry()`: After geometry changes, calculates window center and reassigns to whichever monitor contains it. Updates Client.monitor/workspace and `_NET_WM_DESKTOP`. Does NOT change focus state or call focus_or_fallback.
