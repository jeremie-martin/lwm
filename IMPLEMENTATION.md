# LWM Implementation Notes

This file is for maintainers. It records non-obvious architecture and behavior constraints that are easy to violate.

## 1. Source Map

- `src/lwm/wm.cpp`: main state transitions, window management, layout triggers
- `src/lwm/wm_events.cpp`: X11 event dispatch, key/mouse handling, client-message routing
- `src/lwm/wm_focus.cpp`: focus policy execution and fallback logic
- `src/lwm/wm_workspace.cpp`: workspace switching and cross-workspace moves
- `src/lwm/wm_floating.cpp`: floating geometry, visibility, monitor reassignment
- `src/lwm/wm_drag.cpp`: drag state machine for floating move/resize and tiled reorder
- `src/lwm/wm_ewmh.cpp`: EWMH desktop/workarea/client-list updates and desktop index translation
- `src/lwm/core/types.hpp`: `Client`, `Workspace`, `Monitor` data model

## 2. Core Data Model

### Client

`Client` is the authoritative per-window state. Treat cached X11 properties as inputs, not truth.

Important fields:

- `kind`: tiled/floating/dock/desktop behavior branch
- `monitor`, `workspace`: location for tiled/floating windows
- `hidden`: physical off-screen state
- `iconic`: logical minimized state
- `floating_geometry`: persistent floating placement
- ordering/state flags mirrored to EWMH where applicable

### Containers

Managed windows live in exactly one class container plus `clients_`:

- tiled -> one `Workspace::windows`
- floating -> `floating_windows_`
- dock -> `dock_windows_`
- desktop -> `desktop_windows_`

## 3. Invariants That Matter

- `hidden` and `iconic` are different concepts. Workspace switches set `hidden` without iconifying.
- `iconic` implies off-screen for non-sticky windows.
- fullscreen windows are excluded from normal tiling layout.
- `above` and `below` are mutually exclusive.
- modal implies above (one-way coupling).
- `_NET_CLIENT_LIST` tracks managed windows, not currently visible windows.

## 4. Event Flow (High Level)

`run()` loop:

1. poll for X events / timeout
2. dispatch via `handle_event()`
3. process ping/kill timeouts
4. check connection health

Main handlers live in `wm_events.cpp`:

- `MapRequest`: classify, manage, map, apply initial state
- `UnmapNotify` / `DestroyNotify`: unmanage path
- `EnterNotify` / `MotionNotify` / `ButtonPress`: focus + drag interactions
- `ClientMessage`: EWMH/ICCCM control path
- `ConfigureRequest`: tiled ack vs floating apply
- `PropertyNotify`: title/hints/struts/state/user-time updates
- RANDR screen change: hotplug reconciliation

Input nuance:

- `toggle_workspace` has explicit auto-repeat protection (`KeyRelease`/`KeyPress` same timestamp).

## 5. Window Lifecycle

Nominal path:

1. map request arrives
2. ignore override-redirect
3. classify (popup types bypass full management)
4. apply rules
5. create `Client`
6. read initial state (`_NET_WM_STATE`, `WM_HINTS.initial_state`)
7. compute geometry
8. `xcb_map_window()`
9. if not currently visible/iconic, move off-screen
10. apply post-map non-geometry states
11. update EWMH lists

Unmanage path (Unmap/Destroy):

- set `WM_STATE=Withdrawn`
- remove from containers and `clients_`
- reconcile focus
- update EWMH lists

## 6. Focus Pipeline

`focus_any_window()` is the single entry point.

Key rules:

- no focus assignment during showing-desktop mode
- no focus for ineligible windows
- hidden/off-workspace targets are shown before final focus assignment
- deiconify before final focus assignment
- workspace switch may occur as part of focus target resolution
- fullscreen focus transitions must not reapply fullscreen geometry or reintroduce borders

Fallback sequence (`focus_or_fallback()`):

1. workspace remembered focus
2. other eligible tiled windows (MRU order)
3. eligible sticky tiled windows from other workspaces on the same monitor
4. eligible floating windows (MRU)
5. clear focus

## 7. Workspace Switch Contract

All switch flows should converge through `perform_workspace_switch(...)`.

Required order:

1. update monitor current/previous workspace
2. hide old workspace windows
3. flush X connection
4. update `_NET_CURRENT_DESKTOP`
5. rearrange/show target workspace
6. refresh floating visibility
7. restore focus in caller (`focus_or_fallback()` or explicit target focus)

The flush between hide and show is intentional to avoid visual artifacts and stale geometry timing.

## 8. Visibility Model (Intentional)

LWM hides managed windows by moving them off-screen (`OFF_SCREEN_X`), not by unmapping them.

Why:

- avoids redraw bugs seen with some GPU-heavy apps on unmap/remap
- keeps workspace switching immediate

Consequences:

- WM-initiated unmaps are not part of visibility control
- treat received `UnmapNotify` as client-initiated withdraw/unmanage
- hidden windows can still emit some events; handlers must gate on `client.hidden`

## 9. Hotplug Contract

RANDR changes:

- exit fullscreen on managed windows before monitor graph rebuild (prevents stale fullscreen geometry)
- remap windows to monitors by monitor name, not old monitor index
- fallback to monitor 0 when old name disappears
- recompute struts/workareas
- restore focus target and rearrange all monitors

Using names avoids index-churn regressions during unplug/replug.

## 10. Common Regression Traps

- Updating `_NET_CLIENT_LIST` on workspace switch (wrong: update on manage/unmanage).
- Treating sticky as global across all monitors (LWM sticky is per-monitor).
- Applying ConfigureRequest geometry to tiled windows (should send synthetic configure only).
- Reintroducing WM-driven unmap/map for normal workspace visibility.
