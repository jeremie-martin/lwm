# Protocol Compliance Requirements

This document defines the external protocol obligations LWM must satisfy for X11 window management.
It is normative and testable. All items describe what the window manager MUST or SHOULD implement.

---

## ICCCM Compliance

### 1. Window Manager Selection and Startup

#### WM_S{screen} Selection
- Acquire the `WM_S0` selection (or `WM_Sn` for screen n) on startup.
- If the selection is already owned, either:
  - Fail to start, OR
  - Request the current owner to release (and wait for SelectionClear).
- After acquiring ownership, broadcast a `MANAGER` client message to the root window.
- Respond to `SelectionClear` by releasing management and exiting gracefully.

#### SubstructureRedirect
- Select `SubstructureRedirectMask` on the root window.
- If another client already has this selected, fail to start.

### 2. Client Window Properties (Read)

The WM must read and honor these client-set properties:

#### WM_NAME / _NET_WM_NAME
- Use `_NET_WM_NAME` (UTF-8) if present; fall back to `WM_NAME`.
- Display in title bars, taskbars, or other UI as appropriate.

#### WM_ICON_NAME / _NET_WM_ICON_NAME
- Use for iconified/minimized representation if displayed.

#### WM_CLASS
- Read `WM_CLASS` (instance name, class name) for window identification.
- Use for window matching rules and grouping.

#### WM_CLIENT_MACHINE
- Read for display purposes and session management.

#### WM_NORMAL_HINTS (Size Hints)
- `min_width`, `min_height`: enforce minimum dimensions.
- `max_width`, `max_height`: **not enforced** (tiling WM controls sizing).
- `base_width`, `base_height`: **not enforced** (increments are ignored for all windows).
- `width_inc`, `height_inc`: **intentionally not enforced** for any windows (tiled or floating).
  - Rationale: Resize increments (e.g., terminal character sizes) would block
    pixel-granular resizing. LWM prioritizes smooth, consistent layout and
    uniform gaps over character-cell alignment for every window type.
- `min_aspect`, `max_aspect`: **not enforced** (could cause layout gaps).
- `win_gravity`: use for positioning after resize.
- `PPosition`, `USPosition`: honor user-specified position for floating windows.
- `PSize`, `USSize`: honor user-specified size for initial mapping.

#### WM_HINTS
- `input`: if False, never call `SetInputFocus` on the window.
- `initial_state`: honor `IconicState` vs `NormalState` on first map.
- `icon_pixmap`, `icon_mask`, `icon_window`: use for iconified representation.
- `window_group`: associate windows into groups.
- `urgency`: reflect via `_NET_WM_STATE_DEMANDS_ATTENTION`.

#### WM_TRANSIENT_FOR
- Identify transient relationships.
- Transients inherit workspace from their parent.
- Transients stack above their parent when both visible.
- Transients should not appear in taskbars/pagers independently.

#### WM_PROTOCOLS
- Check which protocols the client supports.
- Required protocols to handle:
  - `WM_DELETE_WINDOW`: send instead of killing the client.
  - `WM_TAKE_FOCUS`: send when focusing windows with `input=False`.
- Optional protocols:
  - `_NET_WM_PING`: liveness checking.
  - `_NET_WM_SYNC_REQUEST`: synchronized resizing.

#### WM_COLORMAP_WINDOWS
- Install colormaps for listed windows when appropriate (legacy).

### 3. Client Window Properties (Write)

The WM must set and maintain:

#### WM_STATE
- Set on all managed windows.
- Values: `WithdrawnState` (0), `NormalState` (1), `IconicState` (3).
- State transitions:
  - `NormalState`: Window is managed and logically visible (may be unmapped due to workspace).
  - `IconicState`: Window is explicitly minimized/iconified by user action.
  - `WithdrawnState`: Window is unmanaged (client withdrew or WM unmanaged it).
- **Workspace visibility does NOT change WM_STATE**: A window on a non-visible workspace
  remains in `NormalState` even though it is unmapped. Visibility across workspaces is
  expressed via `_NET_WM_DESKTOP` and physical mapping state, not WM_STATE.
- Remove or set to Withdrawn when unmanaging.

### 4. Client Messages (Handle)

#### WM_CHANGE_STATE
- If `data[0]` is `IconicState`, iconify the window.

### 5. ConfigureRequest Handling

- For **tiled/managed windows**: 
  - Acknowledge with a synthetic `ConfigureNotify` reflecting WM-determined geometry.
  - Apply size hint constraints.
- For **floating windows**:
  - Apply requested changes within size hint constraints.
  - Honor position requests if reasonable.
- For **override-redirect windows**: ignore (not managed).

### 6. MapRequest Handling

- For normal windows: add to management, apply initial state.
- For `override_redirect=True`: ignore entirely.
- Honor `WM_HINTS.initial_state`:
  - `IconicState`: manage but do not map; set `WM_STATE` to Iconic.
  - `NormalState` or unset: map normally.

### 7. Unmap Handling

ICCCM requires distinguishing WM-initiated unmaps from client-initiated unmaps:

- **WM-initiated unmaps** (e.g., hiding windows during workspace switches):
  - The WM tracks pending unmaps with a counter before calling `xcb_unmap_window()`.
  - The counter is only incremented if the window is currently viewable (to avoid leaks).
  - When `UnmapNotify` arrives, if counter > 0, decrement and ignore the event.
  - Window remains managed; WM_STATE stays `NormalState`.

- **Client-initiated unmaps** (withdraw requests):
  - `UnmapNotify` arrives with counter == 0 (no pending WM unmap).
  - Unmanage the window and set `WM_STATE` to `WithdrawnState`.
  - Remove from all internal structures.

- **Event source**: We receive `UnmapNotify` events via root window's `SubstructureNotifyMask`.
  We intentionally do NOT select `STRUCTURE_NOTIFY` on client windows to avoid receiving
  duplicate `UnmapNotify` events (which would break the counting mechanism).

### 8. Destroy Handling

- Remove destroyed windows from all internal structures.
- Update focus if the destroyed window was focused.

### 9. Reparenting

- Reparent managed windows into a frame window (if decorating).
- Save and restore original window position/border on unmanage.
- Handle reparent-back correctly on unmanage or shutdown.

### 10. Focus Management

- Never call `SetInputFocus` on windows with `WM_HINTS.input=False`.
- For `input=True` (or unset): set focus directly with `SetInputFocus`.
- For `input=False` with `WM_TAKE_FOCUS` in protocols: send `WM_TAKE_FOCUS` message.
- For `input=False` without `WM_TAKE_FOCUS`: window cannot receive focus.
- Combine models as needed (Globally Active, Locally Active, Passive, No Input).

### 11. Session Management Integration

- Set `WM_CLIENT_LEADER` if acting as session participant.
- Honor `WM_WINDOW_ROLE` for session restoration.
- Respond to `WM_SAVE_YOURSELF` if supported (legacy).

---

## EWMH Compliance

### 1. Root Window Properties (Write and Maintain)

#### _NET_SUPPORTED
- List all EWMH atoms the WM supports.
- Must be accurate and match actual behavior.
- Include supported `_NET_WM_STATE_*` atoms.
- Include supported `_NET_WM_WINDOW_TYPE_*` atoms.
- Include supported `_NET_WM_ACTION_*` atoms.

#### _NET_SUPPORTING_WM_CHECK
- Set on root window pointing to a child window.
- Set on child window pointing to itself.
- Child window must have `_NET_WM_NAME` set to WM name.

#### _NET_CLIENT_LIST
- List all managed windows in initial mapping order.
- Update on map/unmap.

#### _NET_CLIENT_LIST_STACKING
- List all managed windows in bottom-to-top stacking order.
- Update when stacking changes.

#### _NET_NUMBER_OF_DESKTOPS
- Total number of virtual desktops/workspaces.
- Update if desktops are added/removed dynamically.
- For per-monitor workspaces: count is `monitors * workspaces_per_monitor`.

#### _NET_DESKTOP_GEOMETRY
- Size of the desktop (viewport) in pixels.
- For non-large-desktop WMs: typically screen/monitor size.

#### _NET_DESKTOP_VIEWPORT
- Top-left corner of each desktop's viewport.
- For non-large-desktop WMs: typically (0,0) for each.
- For per-monitor workspaces: repeat each monitor’s origin once per workspace slot.

#### _NET_CURRENT_DESKTOP
- Index of the currently active desktop.
- Update on desktop switch.
- For per-monitor workspaces: track the active monitor’s current workspace.

#### _NET_DESKTOP_NAMES
- UTF-8 names for each desktop.
- Null-separated.

#### _NET_ACTIVE_WINDOW
- XID of the currently focused window, or None.
- Update immediately on focus change.

#### _NET_WORKAREA
- Usable area (x, y, width, height) per desktop.
- Account for struts/panels.
- Update when struts change.

#### _NET_VIRTUAL_ROOTS
- List virtual root windows if using virtual roots.
- Omit if not applicable.

#### _NET_DESKTOP_LAYOUT
- Pager layout hint (rows, columns, orientation).
- Honor if set by pager; may set if WM controls layout.

#### _NET_SHOWING_DESKTOP
- Set to 1 when in "show desktop" mode, 0 otherwise.

### 2. Per-Window Properties (Read)

#### _NET_WM_WINDOW_TYPE
Types to support (in priority order, first match wins):
- `_NET_WM_WINDOW_TYPE_DESKTOP`: below all, no focus, spans workspaces.
- `_NET_WM_WINDOW_TYPE_DOCK`: panel/bar, reserves strut space, no focus typically.
- `_NET_WM_WINDOW_TYPE_TOOLBAR`: floating, no taskbar.
- `_NET_WM_WINDOW_TYPE_MENU`: floating, no taskbar.
- `_NET_WM_WINDOW_TYPE_UTILITY`: floating, no taskbar, above siblings.
- `_NET_WM_WINDOW_TYPE_SPLASH`: floating, centered, no decorations, no taskbar.
- `_NET_WM_WINDOW_TYPE_DIALOG`: floating, centered, transient-like.
- `_NET_WM_WINDOW_TYPE_DROPDOWN_MENU`: popup, no decorations, no focus steal.
- `_NET_WM_WINDOW_TYPE_POPUP_MENU`: popup, no decorations, no focus steal.
- `_NET_WM_WINDOW_TYPE_TOOLTIP`: popup, no decorations, no focus, timeout.
- `_NET_WM_WINDOW_TYPE_NOTIFICATION`: popup, no decorations, no focus.
- `_NET_WM_WINDOW_TYPE_COMBO`: dropdown element, no decorations.
- `_NET_WM_WINDOW_TYPE_DND`: drag-and-drop icon, no decorations.
- `_NET_WM_WINDOW_TYPE_NORMAL`: default, managed normally (tiled or floating per policy).

#### _NET_WM_STATE (Initial)
- Read on map to apply initial states.
- States to support:
  - `_NET_WM_STATE_MODAL`: treat as modal dialog.
  - `_NET_WM_STATE_STICKY`: visible on all workspaces of the owning monitor (see BEHAVIOR.md §1.5).
  - `_NET_WM_STATE_MAXIMIZED_VERT`: maximize vertically (**floating windows only**; tiled windows
    ignore this as they already fill the tiling area).
  - `_NET_WM_STATE_MAXIMIZED_HORZ`: maximize horizontally (**floating windows only**).
  - `_NET_WM_STATE_SHADED`: show only title bar (implemented as iconify due to no decorations).
  - `_NET_WM_STATE_SKIP_TASKBAR`: exclude from taskbar.
  - `_NET_WM_STATE_SKIP_PAGER`: exclude from pager.
  - `_NET_WM_STATE_HIDDEN`: iconified/minimized.
  - `_NET_WM_STATE_FULLSCREEN`: fullscreen mode (works for both tiled and floating).
  - `_NET_WM_STATE_ABOVE`: always on top.
  - `_NET_WM_STATE_BELOW`: always on bottom.
  - `_NET_WM_STATE_DEMANDS_ATTENTION`: urgency indicator (also reflects ICCCM WM_HINTS urgency).
  - `_NET_WM_STATE_FOCUSED`: has input focus (WM sets this).

#### _NET_WM_STRUT / _NET_WM_STRUT_PARTIAL
- Reserve screen edges for panels/docks.
- Reduce workarea accordingly.
- Apply to window placement and tiling.
- **Limitation**: Partial strut ranges (start/end coordinates) are read but only the basic
  extents (left/right/top/bottom) are used. Multi-monitor partial struts may not be
  handled correctly if they span monitors.

#### _NET_WM_ICON
- Read for taskbar/pager/switcher display.

#### _NET_WM_PID
- Read for process identification and management.

#### _NET_WM_USER_TIME
- Compare for focus stealing prevention.
- Newer user time indicates more recent user interaction.

#### _NET_WM_USER_TIME_WINDOW
- Read user time from this window instead of the client window.

#### _NET_WM_FULLSCREEN_MONITORS
- Four monitor indices defining fullscreen span.
- Apply when window enters fullscreen.

#### _NET_WM_BYPASS_COMPOSITOR
- Hint to disable compositing for this window.

### 3. Per-Window Properties (Write)

#### _NET_WM_DESKTOP
- Set to desktop index for each window.
- Set to 0xFFFFFFFF for sticky windows.
- Update when window moves between desktops.

#### _NET_WM_STATE
- Maintain current state atoms.
- Add/remove atoms as state changes.

#### _NET_WM_ALLOWED_ACTIONS
- List actions permitted on the window:
  - `_NET_WM_ACTION_MOVE`
  - `_NET_WM_ACTION_RESIZE`
  - `_NET_WM_ACTION_MINIMIZE`
  - `_NET_WM_ACTION_SHADE`
  - `_NET_WM_ACTION_STICK`
  - `_NET_WM_ACTION_MAXIMIZE_HORZ`
  - `_NET_WM_ACTION_MAXIMIZE_VERT`
  - `_NET_WM_ACTION_FULLSCREEN`
  - `_NET_WM_ACTION_CHANGE_DESKTOP`
  - `_NET_WM_ACTION_CLOSE`
  - `_NET_WM_ACTION_ABOVE`
  - `_NET_WM_ACTION_BELOW`
- Update based on window type and state.

#### _NET_WM_VISIBLE_NAME / _NET_WM_VISIBLE_ICON_NAME
- Set if displayed name differs from `_NET_WM_NAME`.

#### _NET_FRAME_EXTENTS
- Set frame dimensions (left, right, top, bottom) in pixels.
- Set to (0,0,0,0) for undecorated windows.
- Update if decorations change.

### 4. Client Messages (Handle)

#### _NET_CURRENT_DESKTOP
- Switch to requested desktop if valid.
- Update `_NET_CURRENT_DESKTOP` and visibility.

#### _NET_ACTIVE_WINDOW
- Activate the specified window.
- Switch desktops if necessary.
- Apply focus stealing prevention based on source indication and user time.

#### _NET_CLOSE_WINDOW
- Initiate close (send `WM_DELETE_WINDOW` or destroy).

#### _NET_MOVERESIZE_WINDOW
- Apply gravity-adjusted move/resize.
- Honor flags for which fields to apply.
- Respect size hints (minimum dimensions enforced).
- **Limitation**: Only supported for **floating windows**. Requests for tiled windows are ignored
  (tiled window geometry is controlled by the tiling layout).
- **Limitation**: Gravity parameter is currently ignored; position is applied directly.

#### _NET_WM_MOVERESIZE
- Initiate interactive move or resize based on direction.
- Cancel ongoing operation if `_NET_WM_MOVERESIZE_CANCEL`.
- **Limitation**: Only supported for **floating windows**. Tiled windows cannot be interactively
  moved/resized via this mechanism (use keybindings or mouse drag with modifier).

#### _NET_RESTACK_WINDOW
- Restack window relative to sibling.
- **Limitation**: Source indication rules are not fully applied.
- **Limitation**: `_NET_CLIENT_LIST_STACKING` may not be updated immediately after restack.

#### _NET_REQUEST_FRAME_EXTENTS
- Set `_NET_FRAME_EXTENTS` before window is mapped.

#### _NET_WM_STATE
- Handle state change requests with action:
  - `_NET_WM_STATE_REMOVE` (0): remove atom.
  - `_NET_WM_STATE_ADD` (1): add atom.
  - `_NET_WM_STATE_TOGGLE` (2): toggle atom.
- Apply to both `data[1]` and `data[2]` atoms if present.

#### _NET_WM_DESKTOP
- Move window to requested desktop.
- Handle 0xFFFFFFFF as "sticky."

#### _NET_WM_FULLSCREEN_MONITORS
- Store and apply fullscreen monitor bounds.

#### _NET_SHOWING_DESKTOP
- Enter or leave "show desktop" mode.

### 5. Focus Stealing Prevention

- Track `_NET_WM_USER_TIME` for windows.
- Support `_NET_WM_USER_TIME_WINDOW`: if set, read user time from the specified window.
- Compare timestamps when handling `_NET_ACTIVE_WINDOW`:
  - Source indication 1 (application): apply prevention.
  - Source indication 2 (pager/user): allow activation.
- For prevented activations: set `_NET_WM_STATE_DEMANDS_ATTENTION`.
- User time is also updated when windows receive focus.

### 6. WM Protocols (EWMH Extensions)

#### _NET_WM_PING
- Send via `WM_PROTOCOLS` format to check client liveness.
- Track response; consider hung if no reply within timeout.
- Use `_NET_WM_PID` and `WM_CLIENT_MACHINE` to offer kill option.
- **Current behavior**: Ping is sent during `kill_window()` to check if window is responsive.
  If no response within 5 seconds, the window is forcibly killed. This is more aggressive
  than ideal—responsive windows doing slow shutdown may be killed prematurely.

#### _NET_WM_SYNC_REQUEST
- Send before WM-initiated resizes.
- Wait for client to update `_NET_WM_SYNC_REQUEST_COUNTER`.
- Proceed after counter update or timeout.
- **Limitation**: Current implementation uses blocking wait during configure, which can
  introduce latency. A future version should use non-blocking async waiting.

### 7. Compositing Manager Interaction

#### _NET_WM_CM_S{screen}
- Check for compositing manager presence.
- Respect `_NET_WM_BYPASS_COMPOSITOR` hints.

---

## Startup Notification Protocol (Optional)

- Monitor `_NET_STARTUP_ID` on new windows.
- Match to pending startup sequences.
- Complete sequences and remove visual feedback.
- Apply workspace/focus from startup notification.

---

## XDG Specifications (Supplementary)

### Application Identification
- Use `_NET_WM_PID` + `WM_CLIENT_MACHINE` for process identification.
- Match against `.desktop` files via `StartupWMClass` or binary name.

---

## System Tray Protocol (Optional)

If implementing system tray support:
- Claim `_NET_SYSTEM_TRAY_S{screen}` selection.
- Handle `_NET_SYSTEM_TRAY_OPCODE` messages.
- Embed tray icons via reparenting.
- Support `_NET_SYSTEM_TRAY_ORIENTATION`.

---

## Invariants and Consistency Requirements

1. **_NET_SUPPORTED Accuracy**: Every atom in `_NET_SUPPORTED` must have working implementation.

2. **State Consistency**:
   - `WM_STATE=IconicState` ↔ `_NET_WM_STATE` contains `HIDDEN`.
   - `_NET_ACTIVE_WINDOW` = None ↔ no window has input focus.
   - `_NET_CLIENT_LIST` contains exactly all managed, non-override-redirect windows.
   - `_NET_CLIENT_LIST_STACKING` has same windows as `_NET_CLIENT_LIST`.
   - **DOCK windows are NOT included in `_NET_CLIENT_LIST`**: Docks (panels, bars) are tracked
     separately for strut handling but are not considered "managed" for client list purposes.
     They have their own stacking layer (above normal windows) and do not participate in
     workspace membership or normal focus handling.

3. **Desktop Consistency**:
   - `_NET_CURRENT_DESKTOP` < `_NET_NUMBER_OF_DESKTOPS`.
   - All window `_NET_WM_DESKTOP` values are valid or 0xFFFFFFFF.

4. **Workarea Accuracy**:
   - `_NET_WORKAREA` reflects all active struts.
   - Updated immediately when struts change.

5. **Frame Extents**:
   - `_NET_FRAME_EXTENTS` set before and after mapping.
   - Accurate for frame geometry calculations.

---

## Conformance Testing Checklist

### Startup
- [ ] `WM_S0` selection acquired and `MANAGER` message broadcast.
- [ ] Fails gracefully if another WM is running.
- [ ] `SubstructureRedirectMask` selected on root.

### ICCCM Window Properties
- [ ] `WM_NORMAL_HINTS` respected for sizing.
- [ ] `WM_HINTS.input` respected for focus.
- [ ] `WM_HINTS.initial_state` honored on map.
- [ ] `WM_TRANSIENT_FOR` affects stacking and workspace.
- [ ] `WM_PROTOCOLS` checked for DELETE_WINDOW and TAKE_FOCUS.

### ICCCM State
- [ ] `WM_STATE` set to Normal on map.
- [ ] `WM_STATE` set to Iconic on iconify.
- [ ] `WM_STATE` set to Withdrawn on unmanage.
- [ ] `WM_CHANGE_STATE` to Iconic iconifies window.

### EWMH Root Properties
- [ ] `_NET_SUPPORTING_WM_CHECK` valid on root and child.
- [ ] `_NET_SUPPORTED` lists all supported atoms.
- [ ] `_NET_CLIENT_LIST` accurate.
- [ ] `_NET_CLIENT_LIST_STACKING` accurate.
- [ ] `_NET_ACTIVE_WINDOW` updates on focus.
- [ ] `_NET_CURRENT_DESKTOP` updates on switch.
- [ ] `_NET_WORKAREA` reflects struts.

### EWMH Window Types
- [ ] DOCK windows reserve strut space.
- [ ] DIALOG windows float and center.
- [ ] SPLASH windows have no decorations.
- [ ] DESKTOP windows stay below all.

### EWMH States
- [ ] FULLSCREEN covers monitor geometry.
- [ ] ABOVE windows stay on top.
- [ ] HIDDEN corresponds to iconified.
- [ ] DEMANDS_ATTENTION set for urgent windows.
- [ ] MAXIMIZED_* fills workarea dimension.

### EWMH Client Messages
- [ ] `_NET_ACTIVE_WINDOW` activates and switches desktop.
- [ ] `_NET_CLOSE_WINDOW` closes window.
- [ ] `_NET_WM_STATE` add/remove/toggle works.
- [ ] `_NET_CURRENT_DESKTOP` switches desktop.
- [ ] `_NET_WM_MOVERESIZE` initiates interactive operation.

### Focus
- [ ] Focus follows configured model.
- [ ] `WM_TAKE_FOCUS` sent when needed.
- [ ] Focus stealing prevention applied.
- [ ] User time tracked correctly.

### Synchronization
- [ ] `_NET_WM_PING` sent and tracked.
- [ ] `_NET_WM_SYNC_REQUEST` sent before WM resizes.
