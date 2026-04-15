# ICCCM / EWMH Compliance

This document is normative for protocol behavior. It describes what LWM exposes to clients and desktop tooling. For the internal model and transition rules, see [`ARCHITECTURE.md`](ARCHITECTURE.md).

## 1. ICCCM

### 1.1 WM Ownership and Startup

LWM:

- acquires `WM_S0`
- selects `SubstructureRedirectMask` on the root window
- refuses startup if another WM already owns the role
- broadcasts `MANAGER` after ownership is acquired

### 1.2 Client Properties Read by LWM

LWM reads and uses:

- `WM_NAME` / `_NET_WM_NAME`
- `WM_CLASS`
- `WM_HINTS` (`input`, `initial_state`, urgency)
- `WM_NORMAL_HINTS` (initial and runtime size/position hints)
- `WM_TRANSIENT_FOR`
- `_NET_WM_USER_TIME_WINDOW`
- `WM_PROTOCOLS` (`WM_DELETE_WINDOW`, `WM_TAKE_FOCUS`, `_NET_WM_PING`, `_NET_WM_SYNC_REQUEST` when present)

Intentional limits:

- `WM_NORMAL_HINTS.min_width` / `min_height` are not enforced
- `WM_NORMAL_HINTS.width_inc` / `height_inc` are not enforced
- `WM_NORMAL_HINTS` aspect/gravity hints do not override layout policy
- runtime changes to `WM_HINTS`, `WM_NORMAL_HINTS`, and `WM_TRANSIENT_FOR` are re-evaluated for managed tiled/floating windows
- `_NET_WM_USER_TIME_WINDOW` indirection is respected, and changes after manage are re-read so activation time can follow the helper window

### 1.3 `WM_STATE`

LWM writes `WM_STATE` for managed windows:

- `NormalState` for normal managed visibility
- `IconicState` for explicit iconify/minimize
- `WithdrawnState` on unmanage

Workspace switching does not change `WM_STATE` on its own.

### 1.4 ICCCM Requests

- `WM_CHANGE_STATE(IconicState)` iconifies the window
- `ConfigureRequest`:
  - tiled windows receive a synthetic `ConfigureNotify`; the WM keeps geometry authority
  - floating windows may have geometry applied within WM policy constraints

## 2. EWMH Root Properties

LWM maintains:

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

Protocol notes:

- `_NET_CLIENT_LIST` tracks managed windows, including dock and desktop windows
- `_NET_CLIENT_LIST` changes on manage/unmanage, not on workspace show/hide
- desktop indices use the per-monitor mapping described in [`ARCHITECTURE.md`](ARCHITECTURE.md)

## 3. EWMH Window Types

LWM classifies windows primarily by `_NET_WM_WINDOW_TYPE`:

- `DESKTOP`: managed, bottom layer, not focus-eligible
- `DOCK`: managed, strut-reserving, not normal-focus eligible
- `DIALOG`, `UTILITY`, `TOOLBAR`, `MENU`, `SPLASH`: managed floating
- `NORMAL`: managed tiled by default
- popup/ephemeral types (`TOOLTIP`, `NOTIFICATION`, `POPUP_MENU`, `DROPDOWN_MENU`, `COMBO`, `DND`): mapped directly, not fully managed. `_NET_WM_WINDOW_TYPE_NOTIFICATION` windows are never themselves urgency carriers; notification-derived urgency is bridged to managed app windows via IPC.

Runtime reclassification:

- managed tiled/floating windows are re-evaluated when `_NET_WM_WINDOW_TYPE` or `WM_TRANSIENT_FOR` changes
- runtime conversion into `DOCK`, `DESKTOP`, or popup-only behavior is intentionally ignored after manage

## 4. EWMH Window State Atoms

Supported state atoms:

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

State semantics implemented by LWM:

- sticky scope is per owning monitor
- fullscreen supersedes maximize
- above and below are mutually exclusive
- visible fullscreen is exclusive per monitor visible scope; conflicting visible fullscreen clients are suppressed
- `_NET_WM_STATE_FOCUSED` is WM-managed (read-only); client requests to change it are ignored

## 5. Per-Window Properties Written by LWM

LWM writes:

- `_NET_WM_DESKTOP` (or `0xFFFFFFFF` for sticky)
- `_NET_WM_STATE`
- `_NET_WM_ALLOWED_ACTIONS`
- `_NET_FRAME_EXTENTS` (undecorated windows use zero extents)

## 6. EWMH Client Messages

Handled client messages:

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

### 6.1 Message-Specific Semantics

`_NET_ACTIVE_WINDOW`

- source `1` (application) is subject to user-time focus-stealing checks
- source `2` (pager/user) is normally allowed
- rejected steals set `DEMANDS_ATTENTION`
- suppressed windows are not surfaced while a fullscreen owner remains visible
- `_NET_WM_USER_TIME_WINDOW` may redirect activation-time lookups to a helper window, and runtime updates to that property are honored

`_NET_WM_DESKTOP`

- desktop indices are translated through the monitor/workspace mapping
- `0xFFFFFFFF` maps to sticky

`_NET_SHOWING_DESKTOP`

- hides normal managed tiled/floating windows and clears focus
- leaving showing-desktop restores visibility, layout, fullscreen ownership, and fallback focus

`_NET_RESTACK_WINDOW`

- for managed tiled/floating windows, LWM reapplies WM stacking policy instead of blindly honoring the requested sibling/stack mode
- for other windows, including unmanaged and managed dock/desktop windows, the request is forwarded as a raw X restack

`_NET_WM_FULLSCREEN_MONITORS`

- currently affects fullscreen geometry only
- ownership, suppression, and restacking remain tied to the client's monitor

### 6.2 WM Protocol Extensions

- `_NET_WM_PING`: LWM tracks replies and may force-kill an unresponsive client after timeout when close was requested
- `_NET_WM_SYNC_REQUEST`: sync requests are sent before WM-driven resize operations without blocking the event loop

## 7. Known Limits and Intentional Simplifications

- moveresize behavior is effectively floating-window oriented; tiled geometry remains layout-controlled
- gravity and source-indication edge semantics are simplified
- partial-strut coordinate ranges are not fully modeled across monitors; basic edge extents are used
- popup/ephemeral window types are mapped directly rather than fully integrated into workspace/layout/focus policy
- `_NET_WM_FULLSCREEN_MONITORS` is geometry-only, not full multi-monitor fullscreen arbitration

## 8. Intentional Scope Cuts

NOT IMPLEMENTED:

- `_NET_WM_STATE_SHADED`
- `_NET_VIRTUAL_ROOTS`
- `_NET_WM_ICON`
- `_NET_WM_VISIBLE_NAME`
- `_NET_WM_VISIBLE_ICON_NAME`
- `WM_COLORMAP_WINDOWS`
- session-management protocols
- system tray protocol ownership

These are deliberate scope cuts for a small WM.

## 9. Protocol-Level Design Choices

- workspace visibility uses off-screen hiding instead of WM-driven unmap/remap
- because of that model, received `UnmapNotify` is treated as client withdrawal
- dock and desktop windows remain in `_NET_CLIENT_LIST`; taskbars and pagers should filter via skip flags, not list membership

## 10. Release Spot-Checks

- [ ] startup ownership and `MANAGER` broadcast work correctly
- [ ] startup fails cleanly when another WM is active
- [ ] `WM_STATE` transitions match manage/iconify/unmanage behavior
- [ ] `_NET_SUPPORTED` matches actual atoms
- [ ] `_NET_CLIENT_LIST` and `_NET_CLIENT_LIST_STACKING` are accurate
- [ ] `_NET_ACTIVE_WINDOW` tracks real focus changes
- [ ] `_NET_CURRENT_DESKTOP` matches the focused monitor's workspace mapping
- [ ] `_NET_WORKAREA` updates with strut changes
- [ ] `_NET_WM_STATE` client messages add/remove/toggle correctly
- [ ] focus-stealing prevention honors source indication and user time
