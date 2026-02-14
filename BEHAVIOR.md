# LWM Behavior

This file defines user-visible behavior. If implementation changes but behavior stays the same, this file should not change.

## 1. Model

- LWM manages one or more monitors.
- Each monitor has its own workspace set.
- Exactly one workspace per monitor is visible at a time.
- Exactly one monitor is the command target (`active monitor`) at any moment.

### EWMH Desktop Mapping

LWM maps per-monitor workspaces to EWMH desktop indices:

`desktop = monitor_index * workspaces_per_monitor + workspace_index`

Consequences:

- `_NET_NUMBER_OF_DESKTOPS = monitors * workspaces_per_monitor`
- `_NET_CURRENT_DESKTOP` reflects the active monitor's workspace

## 2. Window Classes

- `Tiled`: participates in workspace layout.
- `Floating`: independent geometry, still tied to monitor/workspace.
- `Dock`: managed, reserves struts, always visible, not focus-eligible.
- `Desktop`: managed, stacked below others, not focus-eligible.
- `Popup/Ephemeral`: mapped directly, not fully managed (no workspace/layout membership).

## 3. Visibility Rules

A managed window is visible when all are true:

- Not hidden
- Not iconified
- Not blocked by "show desktop" mode
- On the monitor's current workspace, unless sticky

Sticky behavior:

- Sticky windows appear on all workspaces of their owning monitor.
- Sticky is not global across monitors.

## 4. Focus Rules

Global focus invariant:

- At most one managed window is focused at a time.
- Focusing/unfocusing a fullscreen window must not reapply geometry or reintroduce borders.

### Focus Eligibility

A window may receive focus only if it is:

- Visible
- Not Dock/Desktop class
- Able to accept focus (`WM_HINTS` / `WM_TAKE_FOCUS` respected)

### Focus-Follows-Mouse

- Entering a focus-eligible window focuses it.
- Motion events can re-establish focus if another action stole focus.
- Clicking a window focuses it before processing drag/mouse actions.

### Empty-Space Semantics

- Pointer over empty space on the same monitor: keep current focus.
- Pointer crosses into a different monitor's empty space: switch active monitor and clear focused window.
- Entering a window on that monitor restores focus normally.

### Focus Restoration

When focus becomes invalid (window closes/hides/workspace change), LWM tries, in order:

1. Workspace remembered focus (if still valid)
2. Another eligible tiled window on that workspace
3. Eligible floating window (MRU order)
4. No focus

## 5. Workspace and Monitor Operations

### Switching Workspace

On the active monitor:

- Update current workspace
- Hide old workspace windows
- Show/rearrange new workspace windows
- Restore focus using policy above

No-op if requesting the already active workspace.

### Moving a Window to Another Workspace

- Reassign window workspace
- Recompute affected layouts/visibility
- Restore focus at source workspace if needed

### Switching Active Monitor

- Change command target monitor
- Restore focus on that monitor's current workspace
- Optional cursor warp (configurable)

### Moving a Window to Another Monitor

- Reassign window to destination monitor's current workspace
- Recompute source and destination layouts
- Focus moved window if visible

## 6. New Window Policy

- New tiled windows open on the active monitor's current workspace.
- Floating/transient windows prefer parent window context when available.
- Focus is granted if window is visible and eligible, except where protocol rules block it.

## 7. Tiling Layout + Drag Reorder

Default tiling behavior:

- 1 tiled window: fills the usable monitor area.
- 2 tiled windows: split usable area into two regions.
- 3+ tiled windows: master + stack arrangement.
- Usable area excludes reserved struts.

Drag reorder:

- Dragging a tiled window enters reorder mode.
- During drag, the window follows pointer visually.
- Drop chooses target monitor/workspace by pointer position.
- On drop, window is inserted into target layout and remains focused if eligible.

## 8. Window Rules

Rules are evaluated in order; first match wins.

Typical match keys:

- class / instance / title (regex)
- type
- transient

Typical actions:

- force tiled/floating
- assign workspace or monitor
- set sticky/fullscreen/above/below
- set floating geometry or center placement

Rules cannot override core class behavior for Dock/Desktop/Popup windows.
