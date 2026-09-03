# Architecture

This document defines LWM's internal runtime model and the ownership boundaries
maintainers should preserve. User setup belongs in [README.md](README.md);
external X11 behavior belongs in [X11.md](X11.md); the local wire contract
belongs in [IPC.md](IPC.md).

## Components

| Area | Responsibility |
| --- | --- |
| `src/app/main.cpp`, `cli.*` | process startup, config selection, logging options, exec restart |
| `src/app/lwmctl.cpp` | supported command-line IPC client |
| `src/lwm/config/` | strict TOML parsing and built-in defaults |
| `src/lwm/keybind/` | key binding normalization, grabs, and lookup |
| `src/lwm/layout/` | master-stack and monocle geometry, split ratios, hit testing |
| `src/lwm/core/log.*` | owned logger, secure rotating sink, process-boundary lifecycle |
| `src/lwm/core/types.hpp` | domain state: clients, monitors, workspaces, geometry |
| `src/lwm/core/policy.hpp` | pure visibility, focus, workspace, fullscreen, and hotplug decisions |
| `src/lwm/core/ewmh.*` | EWMH atoms, classification, and property I/O |
| `src/lwm/wm.cpp` | construction, IPC, client lifecycle, rules, visibility, stacking, layout |
| `src/lwm/wm_ewmh.cpp` | root properties, client lists, workareas, EWMH desktop projection |
| `src/lwm/wm_events.cpp` | X event dispatch, client messages, property changes, RANDR |
| `src/lwm/wm_focus.cpp` | focus assignment, fallback, and cycling |
| `src/lwm/wm_workspace.cpp` | workspace and monitor commands |
| `src/lwm/wm_floating.cpp`, `wm_drag.cpp` | floating geometry and pointer-driven move/resize/reorder |
| `src/lwm/wm_restart.cpp`, `wm_scratchpad.cpp` | exec handoff and scratchpad state |

`WindowManager` owns one event loop. It polls the X connection, the SIGHUP
self-pipe, the IPC listener, one pending request, and subscription connections.
State mutation is single-threaded.

Logging is an owned service rather than spdlog global state. A non-null
stderr fallback exists before initialization and after shutdown; the configured
logger is swapped in only after every sink is ready. Exec restart and forked
children flush and return to the fallback before crossing the process boundary,
which prevents log-file descriptor inheritance. Logging policy is fixed by
startup options and is not reloaded from TOML.

## State model

Each RANDR monitor owns a fixed-size vector of workspaces and identifies one as
current. The same configured workspace names are repeated per monitor. EWMH
projects this model into a flat, monitor-major desktop list:

```text
desktop = monitor_index * workspaces_per_monitor + workspace_index
```

`focused_monitor_` is the target for commands. It usually follows the focused
window or pointer but is distinct from X input focus.

`clients_` is the registry for every managed window. `Client::kind` separates:

- `Tiled`: present in exactly one `Workspace::windows` vector.
- `Floating`: independently positioned and absent from tiled membership.
- `Dock`: outside normal focus/layout, contributes a strut, and uses the Above
  stacking tier.
- `Desktop`: outside normal focus/layout and uses the Below stacking tier.

Popup-only window types are mapped directly and never enter `clients_`.

The important authorities are:

- `Client`: placement, classification, geometry restore data, protocol state,
  urgency, and scratchpad membership.
- `Workspace::windows`: tiled membership and layout order.
- `Workspace::focused_window` and `focus_history`: remembered tiled focus.
- `Client::mru_order`: in-memory focus recency for tiled and floating clients;
  only floating order is persisted across an exec restart.
- `active_window_`: the focused managed window.
- `Monitor::fullscreen_owner`: the effective fullscreen owner for one monitor.

Application requests and effective policy are intentionally separate where they
can disagree. For example, `Client::app_prefs` retains requested above/below and
skip states while rules and modal/fullscreen policy determine the effective
state published back to X.

## Visibility

LWM maps a normal client once, then hides it by moving it to
`OFF_SCREEN_X`. Three terms must remain distinct:

- `iconic`: the client is logically minimized; ICCCM `WM_STATE` is
  `IconicState` and EWMH includes `_NET_WM_STATE_HIDDEN`.
- policy-visible: the client is non-iconic and is sticky or belongs to the
  current workspace on its monitor. Showing-desktop hides non-sticky clients.
- `hidden`: LWM has physically moved the client off-screen.

A policy-visible client can still be hidden when another window owns fullscreen.
Normal workspace changes therefore update `hidden`, not `WM_STATE`, and an
incoming `UnmapNotify` means client withdrawal rather than a workspace change.

`reconcile_visibility_for_monitor()` is the authority for fullscreen-owner
selection and physical hide/show state. Its wrappers add only the next required
phase:

- `sync_visibility_for_monitor()`: reconciliation only.
- `finalize_visibility_on_monitor()`: reconciliation, then layout.
- `finalize_move_visibility()`: reconciliation and layout for source and
  destination monitors.
- `rearrange_all_monitors()`: reconcile every monitor before arranging any.

`rearrange_monitor()` consumes reconciled state. It lays out visible tiled
clients, applies fullscreen geometry, then normally delegates global ordering to
`apply_stacking()`.

## Fullscreen and stacking

At most one non-iconic, policy-visible tiled or floating client owns fullscreen
on a monitor. Reconciliation keeps the existing owner when still valid, honors
an explicitly preferred new owner, otherwise chooses the newest eligible
managed client. Other policy-visible tiled/floating clients on that monitor are
suppressed and hidden. A managed transient whose `transient_for` is the owner
is exempt.

Fullscreen state may remain set on iconified or off-workspace clients; ownership
is effective only when they return to visible scope. Showing-desktop removes
fullscreen ownership while active. `_NET_WM_FULLSCREEN_MONITORS` changes
geometry only and does not create cross-monitor ownership.

`apply_stacking()` is the single global stacking authority. It computes the X
order and `_NET_CLIENT_LIST_STACKING` together. Physically hidden clients sort
before visible clients. Desktop clients use the Below tier and docks use the
Above tier; tiled and floating clients use Below, Normal, Above, or Fullscreen
according to effective state. Within a tier, floating clients are above
non-floating clients, active preference is applied within each kind, and
visible transients are placed above visible parents.

## Focus

`focus_any_window()` is the normal focus funnel. It validates eligibility,
deiconifies when necessary, switches the target monitor's workspace when
necessary, updates focus memory and recency, sends `WM_TAKE_FOCUS` when
advertised, sets X input focus, restacks, updates EWMH focus state, clears
urgency, and emits the IPC event.

Docks, desktops, iconic clients, and fullscreen-suppressed clients are not
focus candidates. A managed client accepts focus when `WM_HINTS.input` is true
or it advertises `WM_TAKE_FOCUS`.

Fallback selection prefers the workspace's remembered focus, its bounded focus
history, reverse tiled order, sticky tiled clients on the monitor, then visible
floating clients by recency. Focus cycling instead builds one recency-ranked
list of eligible tiled and floating clients.

Visibility-changing transitions finish with `flush_and_drain_crossing()`
before relying on programmatic focus. This round-trip discards stale crossing
and motion events that could otherwise overwrite the intended focus under
focus-follows-mouse.

## Lifecycle and transitions

The normal map path classifies the X window, matches the first applicable rule,
creates the client record, reads initial hints/state, establishes placement and
protocol properties, maps once, then reconciles visibility, geometry, stacking,
and focus. Docks and desktops use dedicated registration paths; popup-only
types are only mapped.

Client removal writes `WM_STATE=WithdrawnState`, removes every authoritative
membership, repairs visibility/focus, and refreshes EWMH lists. Movement helpers
update both client placement and tiled membership before reconciling the source
and destination monitors. Direct monitor/workspace writes belong only in
manage, movement, restart, and hotplug paths.

Config loading is strict and atomic. Reload replaces the parsed configuration,
rebuilds input and scratchpad state, updates EWMH workspace metadata, and
reapplies currently matching rules. The user-visible reload limits are recorded
in [README.md](README.md).

Graceful restart serializes global, workspace, client, ordering, ratio, and
scratchpad state into private X properties, execs the selected binary, restores
that state during the next scan, then removes the handoff properties. Autostart
is skipped during this handoff. These properties are private implementation
details, not a compatibility API.

RANDR changes rebuild the monitor graph. Tiled and floating clients are rebound
by monitor name, missing names fall back to monitor 0, workspace indices are
clamped, dock/desktop clients are rebound separately, workareas are recomputed,
and visibility, fullscreen geometry, layout, and focus are restored. Fullscreen
monitor-index hints are cleared because their indices may no longer be valid.

Named and generic scratchpads remain ordinary managed clients. Hidden
scratchpads are iconic and off-screen. Showing one rehosts it on the focused
monitor's current workspace and restores its tiled membership or floating
geometry through the normal visibility and focus funnels.

## Invariants

Every completed transition must preserve:

- each tiled client appears in exactly one workspace and each workspace entry
  resolves to a tiled client;
- tiled/floating client monitor and workspace indices are valid;
- kind-specific state matches `Client::kind`;
- active and remembered focus never point at an iconic or absent client;
- above and below are mutually exclusive;
- fullscreen ownership and `hidden` are written by visibility reconciliation;
- managed X stacking and `_NET_CLIENT_LIST_STACKING` come from the same order.

`LWM_ASSERT_INVARIANTS` checks the in-memory subset in debug builds. X
properties and observable ordering require integration tests.
