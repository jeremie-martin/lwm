# ICCCM / EWMH Compliance

This document is normative for protocol behavior.

- `MUST`: required behavior in LWM
- `MAY`: optional behavior currently implemented
- `NOT IMPLEMENTED`: explicitly unsupported

## 1. ICCCM

### 1.1 WM Ownership and Startup

LWM MUST:

- Acquire `WM_S0` (or `WM_Sn`) selection.
- Select `SubstructureRedirectMask` on the root window.
- Refuse startup if another WM already owns the role.
- Broadcast `MANAGER` after ownership is acquired.

### 1.2 Read Client Properties

LWM reads and uses:

- `WM_NAME` / `_NET_WM_NAME`
- `WM_CLASS`
- `WM_HINTS` (`input`, `initial_state`, urgency)
- `WM_NORMAL_HINTS` (minimum size, initial user size/position where relevant)
- `WM_TRANSIENT_FOR`
- `WM_PROTOCOLS` (`WM_DELETE_WINDOW`, `WM_TAKE_FOCUS`, `_NET_WM_PING`, `_NET_WM_SYNC_REQUEST` when present)

Intentional behavior:

- `WM_NORMAL_HINTS.min_width` / `min_height` are enforced.
- `WM_NORMAL_HINTS.width_inc` / `height_inc` are intentionally not enforced.
- `WM_NORMAL_HINTS` aspect/gravity hints are intentionally not enforced for tiled behavior.
- Transients are treated as floating and inherit parent context where possible.

### 1.3 WM_STATE Semantics

LWM writes `WM_STATE` for managed windows:

- `NormalState` for managed normal visibility
- `IconicState` for explicit iconify/minimize
- `WithdrawnState` on unmanage

Important: workspace visibility changes do not imply `WM_STATE` changes.

### 1.4 ICCCM Messages and Requests

- `WM_CHANGE_STATE(IconicState)` -> iconify.
- `ConfigureRequest`:
  - tiled windows: synthetic `ConfigureNotify` only (WM keeps geometry authority)
  - floating windows: apply request within policy constraints

## 2. EWMH Root Properties

LWM maintains these root properties:

- `_NET_SUPPORTED`
- `_NET_SUPPORTING_WM_CHECK`
- `_NET_CLIENT_LIST`
- `_NET_CLIENT_LIST_STACKING`
- `_NET_NUMBER_OF_DESKTOPS`
- `_NET_DESKTOP_GEOMETRY`
- `_NET_DESKTOP_VIEWPORT`
- `_NET_CURRENT_DESKTOP`
- `_NET_DESKTOP_NAMES`
- `_NET_ACTIVE_WINDOW`
- `_NET_WORKAREA`
- `_NET_SHOWING_DESKTOP`

Notes:

- `_NET_CLIENT_LIST` tracks managed windows, including dock/desktop windows.
- `_NET_CLIENT_LIST` updates on manage/unmanage, not workspace show/hide.
- Per-monitor workspace mapping is used for desktop indices.

## 3. EWMH Window Types

LWM classifies windows primarily by `_NET_WM_WINDOW_TYPE`:

- `DESKTOP`: managed, bottom layer, no focus
- `DOCK`: managed, strut-reserving, no normal focus
- `DIALOG/UTILITY/TOOLBAR/SPLASH`: managed floating
- popup/ephemeral types (`TOOLTIP`, `NOTIFICATION`, `POPUP_MENU`, `DROPDOWN_MENU`, `COMBO`, `DND`): mapped but not fully managed
- `NORMAL`: managed tiled by default

## 4. EWMH Window State Atoms

Supported state atoms include:

- `_NET_WM_STATE_FULLSCREEN`
- `_NET_WM_STATE_MAXIMIZED_VERT`
- `_NET_WM_STATE_MAXIMIZED_HORZ`
- `_NET_WM_STATE_HIDDEN`
- `_NET_WM_STATE_STICKY`
- `_NET_WM_STATE_ABOVE`
- `_NET_WM_STATE_BELOW`
- `_NET_WM_STATE_MODAL`
- `_NET_WM_STATE_SKIP_TASKBAR`
- `_NET_WM_STATE_SKIP_PAGER`
- `_NET_WM_STATE_DEMANDS_ATTENTION`
- `_NET_WM_STATE_FOCUSED`

State policy details:

- Sticky scope is per owning monitor.
- Fullscreen supersedes maximize behavior.
- Above and below are mutually exclusive.

## 5. EWMH Per-Window Properties Written by LWM

LWM writes:

- `_NET_WM_DESKTOP` (or `0xFFFFFFFF` for sticky)
- `_NET_WM_STATE`
- `_NET_WM_ALLOWED_ACTIONS`
- `_NET_FRAME_EXTENTS` (undecorated -> zero extents)

## 6. EWMH Client Messages

Handled messages:

- `_NET_CURRENT_DESKTOP`
- `_NET_ACTIVE_WINDOW`
- `_NET_CLOSE_WINDOW`
- `_NET_WM_STATE`
- `_NET_WM_DESKTOP`
- `_NET_SHOWING_DESKTOP`
- `_NET_MOVERESIZE_WINDOW`
- `_NET_WM_MOVERESIZE`
- `_NET_RESTACK_WINDOW`
- `_NET_REQUEST_FRAME_EXTENTS`
- `_NET_WM_FULLSCREEN_MONITORS`

Known limits:

- moveresize actions are effectively floating-window oriented; tiled geometry remains layout-controlled.
- gravity/source-indication edge semantics are simplified.
- partial-strut coordinate ranges are not fully modeled across monitors (basic edge extents are used).

### 6.1 WM Protocol Extension Behavior

- `_NET_WM_PING`: LWM tracks ping replies and may force-kill unresponsive clients after timeout when close was requested.
- `_NET_WM_SYNC_REQUEST`: sync requests are sent before WM-driven resize operations, without blocking the event loop for counter completion.

## 7. Focus Stealing Prevention

For `_NET_ACTIVE_WINDOW`:

- source `1` (application): user-time checks apply
- source `2` (pager/user): activation is allowed
- rejected steals mark window with `DEMANDS_ATTENTION`

`_NET_WM_USER_TIME_WINDOW` indirection is respected when available (user time may come from a child helper window).

## 8. Intentional Omissions / Partial Support

- `_NET_VIRTUAL_ROOTS`: not implemented
- `_NET_WM_ICON`: not implemented
- `_NET_WM_VISIBLE_NAME` / `_NET_WM_VISIBLE_ICON_NAME`: not implemented
- `WM_COLORMAP_WINDOWS`: not implemented
- Session-management protocols: not implemented
- System tray protocol ownership: not implemented

These are deliberate scope cuts for a minimal WM.

## 9. Non-Obvious Compliance Decisions

- Off-screen hiding is used instead of WM-driven unmap/remap for workspace visibility.
- Because of that model, received `UnmapNotify` is treated as client withdrawal.
- Dock and desktop windows remain in `_NET_CLIENT_LIST`; taskbar/pager filtering should rely on skip flags.

## 10. Quick Conformance Checklist

- [ ] WM selection ownership and `MANAGER` message work on startup.
- [ ] startup fails cleanly when another WM is active.
- [ ] `WM_STATE` transitions match manage/iconify/unmanage behavior.
- [ ] `_NET_SUPPORTED` matches actual implemented atoms.
- [ ] `_NET_CLIENT_LIST` and `_NET_CLIENT_LIST_STACKING` are accurate.
- [ ] `_NET_ACTIVE_WINDOW` tracks focus changes.
- [ ] `_NET_CURRENT_DESKTOP` tracks active monitor workspace mapping.
- [ ] `_NET_WORKAREA` updates with strut changes.
- [ ] `_NET_WM_STATE` client messages add/remove/toggle correctly.
- [ ] focus-stealing prevention honors source indication and user time.
