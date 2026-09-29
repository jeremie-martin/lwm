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
| `src/lwm/layout/` | pure master-stack and monocle geometry, split ratios, hit testing |
| `src/lwm/core/log.*` | owned logger, secure rotating sink, process-boundary lifecycle |
| `src/lwm/core/types.hpp` | domain state: clients, monitors, workspaces, geometry |
| `src/lwm/core/policy.hpp` | pure visibility, focus, workspace, fullscreen, and hotplug decisions |
| `src/lwm/core/ewmh.*` | EWMH atoms, classification, and property I/O |
| `src/lwm/core/restart.*` | bounded restart record encoding/decoding without X or live-state mutation |
| `src/lwm/core/ipc_server.*` | socket ownership, bounded request/reply transport, subscriptions |
| `src/lwm/wm_ipc.cpp` | command handling and IPC query results |
| `src/lwm/wm.cpp` | construction, client lifecycle, visibility, stacking, geometry application |
| `src/lwm/wm_rules.cpp` | classification, rule application, and runtime reevaluation |
| `src/lwm/wm_ewmh.cpp` | root properties, client lists, workareas, EWMH desktop projection |
| `src/lwm/wm_events.cpp` | X event dispatch, client messages, property changes, RANDR |
| `src/lwm/wm_focus.cpp` | focus assignment, fallback, and cycling |
| `src/lwm/wm_workspace.cpp` | workspace and monitor commands |
| `src/lwm/wm_floating.cpp`, `wm_drag.cpp` | floating geometry and pointer-driven move/resize/reorder |
| `src/lwm/wm_restart.cpp`, `wm_scratchpad.cpp` | exec handoff and scratchpad state |

`WindowManager` owns one event loop. It polls the X connection, the SIGHUP
self-pipe, the IPC listener, one pending request or reply, and subscription
connections. State mutation is single-threaded. Socket readiness feeds the
transport server; its command callback runs on this same thread. X events,
config reloads, and timeouts use the same state-transition helpers.

Logging is an owned service rather than spdlog global state. A non-null
stderr fallback exists before initialization and after shutdown; the configured
logger is swapped in only after every sink is ready. Exec restart flushes and
returns to the fallback before replacing the process.
Application launches use `posix_spawnp`; owned descriptors are close-on-exec,
so children inherit stderr but not the private log sink. Logging policy is
fixed by startup options and is not reloaded from TOML.

## Layout and geometry

`Layout` computes rectangles and resize boundaries without X calls. Master-stack
is evaluated as an iterative sequence of cuts: split 0 divides master from
stack, and split i divides stack slot i from the remaining slots. Monocle gives
every window the content rectangle and has no resize boundaries. Arrangement,
drop selection, and resize hit testing share the same calculation; they do not
allocate an intermediate tree.

Split identities are stable slot indices. Restart serialization retains the
version-3 depth/path encoding of the former right-leaning tree for existing
splits. Beyond depth 32, the path field is saturated and the full index remains
in the depth field. Non-chain legacy addresses have no meaning in the supported
layouts and are ignored on restore.

`WindowManager::apply_geometry()` owns WM-driven configure requests, sync
notifications, and synthetic ConfigureNotify events for tiled, floating, and
fullscreen geometry. It reports the actual policy-selected border width.
Geometry-only tiled updates compare against the client cache without allocating
a second vector of old rectangles.

## State model

Each RANDR monitor owns a fixed-size vector of workspaces and identifies one as
current. The same configured workspace names are repeated per monitor.
[X11.md](X11.md#desktops-and-root-properties) defines the external EWMH desktop
numbering; internal placement uses separate monitor and workspace indices.

`focused_monitor_` is the target for commands. It usually follows the focused
window or pointer but is distinct from X input focus.

`clients_` is the registry for every managed window. `Client::state` holds one
of four alternatives; `Client::kind()` derives its classification from that
state:

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

- `finalize_visibility_on_monitor()`: reconciliation, then layout.
- `finalize_move_visibility()`: reconciliation and layout for source and
  destination monitors.
- `rearrange_all_monitors()`: reconcile every monitor before arranging any.

`rearrange_monitor()` consumes reconciled state. It lays out visible tiled
clients, applies fullscreen geometry, then marks global stacking dirty unless
the operation changes geometry only.
Multi-monitor arrangement therefore does not restack once per monitor.

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

`apply_stacking()` is the single global stacking authority. Transitions mark
stacking dirty; it is reconciled at startup, at the end of an event-loop
iteration, before IPC replies/subscription events, and before a crossing-event
drain. The drain includes the restack because it can generate crossing events.
Each reconciliation compares the desired visible order with a fresh root
`QueryTree` reply. A longest increasing subsequence identifies the windows
already in order; each remaining window needs one sibling move. This repairs
external restacks without maintaining a second cached authority for X order.
Unrelated root children are not themselves moved. It computes the X order and
`_NET_CLIENT_LIST_STACKING` together. Physically hidden clients sort before
visible clients. Desktop clients use the Below tier and docks use the
Above tier; tiled and floating clients use Below, Normal, Above, or Fullscreen
according to effective state. Within a tier, floating clients are above
non-floating clients, active preference is applied within each kind, and
visible transients are placed above visible parents.

## Focus

`focus_any_window()` is the normal focus funnel. It validates eligibility,
deiconifies when necessary, switches the target monitor's workspace when
necessary, checks the resulting fullscreen suppression, then commits active
focus, focus memory and recency. It sends `WM_TAKE_FOCUS` when
advertised, sets X input focus, marks stacking dirty, updates EWMH focus state,
clears urgency, and emits the IPC event. Reads of the old and new windows'
`_NET_WM_STATE` are issued together; updates preserve unrelated atoms and avoid
rewriting an already-correct state. Explicit same-window focus still reasserts
input focus, protocol notifications, and server stacking.

Docks, desktops, iconic clients, and fullscreen-suppressed clients are excluded
from fallback and cycling. Explicit activation can deiconify a client before
the final eligibility check. A managed client accepts focus when `WM_HINTS.input`
is true or it advertises `WM_TAKE_FOCUS`.

Fallback selection prefers the workspace's remembered focus, its bounded focus
history, reverse tiled order, sticky tiled clients on the monitor, then visible
floating clients by recency. Focus cycling instead builds one recency-ranked
list of eligible tiled and floating clients.

Visibility-changing transitions finish with `flush_and_drain_crossing()`
before accepting subsequent pointer-driven focus. This round-trip discards stale
crossing and motion events that could otherwise overwrite the intended focus
under focus-follows-mouse.

## Lifecycle and transitions

The normal map path classifies the X window, matches the first applicable rule,
creates the client record, reads initial hints/state, establishes placement and
protocol properties, maps once, then reconciles visibility, geometry, stacking,
and focus. Docks and desktops use dedicated registration paths; popup-only
types are only mapped.

Mapping, property reevaluation, and config reload apply rule actions through
`apply_rule_result_to_window()`. Classification and application preferences feed
the shared desired-state policy on mapping and property changes. Classification
and rule/scratchpad matching use the same captured properties; initial client
registration reuses these values. This is consistency within one update, not
an atomic snapshot of independently changing X properties. Managed
class/title/type data are refreshed at their property-notification boundaries.
Title and class updates compare rule results before and after updating metadata.
Unchanged actions leave placement and user state intact; changed results enter
the normal classification transition. Pending scratchpad claims are checked even
when the rule result is unchanged. Type/transient changes still reclassify, and
config reload explicitly reapplies matching rules.
Reload uses the current effective state as its baseline, preserving unspecified
state and the existing behavior when no rule matches. Placement and fullscreen overrides use
the same transition helpers in all three paths.

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
details, not a compatibility API. The X envelope (type, format, and completeness)
is checked before decoding. Client records validate before mutation; layout
restore retains complete workspace records and discards an incomplete tail.
One invalid client record does not discard valid peers.

RandR screen, output, and CRTC notifications mark topology dirty. The event loop
coalesces each batch before reconciliation. Discovery supplies fresh output
geometry; surviving monitor names retain complete workspace state, including
tiled order, focus history, split ratios, and layout strategy. Removed outputs'
clients move to monitor 0, with surviving workspace order/focus taking precedence.
Returning outputs start fresh and do not reclaim relocated clients. All client
kinds use the same old-to-new monitor mapping. Workareas, floating geometry,
visibility, and focus are then reconciled. Fullscreen monitor-index hints are
cleared because the indices may have changed.

Named and generic scratchpads remain ordinary managed clients. Hidden
scratchpads are iconic and off-screen. Showing one rehosts it on the focused
monitor's current workspace and restores its tiled membership or floating
geometry through the normal visibility and focus funnels.

A named scratchpad enters launch-pending state only after successful process
creation and exec. Process exit is not used as a window-creation signal: a
launcher may delegate to another process. Pending launches suppress duplicate
toggles until a matching window arrives or the user explicitly cancels the
pending launch through IPC. Cancellation neither kills the program nor prevents
a late matching window from being claimed.

## Invariants

Every completed transition must preserve:

- each tiled client appears in exactly one workspace and each workspace entry
  resolves to a tiled client;
- tiled/floating client monitor and workspace indices are valid;
- kind and kind-specific storage agree by construction through `ClientState`;
- active and remembered focus never point at an iconic or absent client;
- above and below are mutually exclusive;
- fullscreen ownership and `hidden` are written by visibility reconciliation;
- managed X stacking and `_NET_CLIENT_LIST_STACKING` come from the same order.

`invariants::validate()` checks registry identity, placement, tiled membership,
workspace focus, effective fullscreen ownership, and active focus without X
calls. Debug builds run it on entry to the event loop and after each completed
iteration, covering X events, IPC, signal reloads, and timeouts after their
transitions settle. A violation logs the reason and aborts at that boundary;
Release builds omit these checks. X properties and observable ordering require
integration tests.
