# Architecture

LWM is a pure domain model (`State`) inside an X11 shell (`WindowManager`). The shell
does two things: it **observes** X and IPC input as plain values, and it **applies
effects** (X requests, process launches, grabs, pointer warps, IPC replies). Every
decision about windows, placement, focus, visibility and configuration belongs to
`State`. One completion step per operation projects the model onto the X server.
The event loop is single-threaded; only log delivery runs on a worker.

Configuration syntax belongs in [config.toml.example](config.toml.example).
[X11.md](X11.md) and [IPC.md](IPC.md) own externally observable protocol behavior;
[TESTING.md](TESTING.md) covers validation and the integration harness.

## Code map

Paths are relative to `src/`.

| Area | Owner |
| --- | --- |
| Startup, options, exec/recovery | `app/main.cpp`, `app/cli.*` |
| Command-line IPC client | `app/lwmctl.cpp` |
| Strict TOML input and resolved configuration | `lwm/config/` |
| Domain values: clients, fixtures, monitors, observations, requests | `lwm/core/types.hpp` |
| The model's interface | `lwm/core/state.hpp` |
| Registry, focus, client state, workspaces, configuration, topology | `lwm/core/state.cpp` |
| Roles, admission, initial placement, rules, metadata, restoration | `lwm/core/admission.cpp` |
| Decoded application and pager requests | `lwm/core/requests.cpp` |
| Layout projection and floating geometry | `lwm/core/geometry.cpp`, `floating.*`, `lwm/layout/` |
| Pointer interactions | `lwm/core/drag.cpp` |
| Named scratchpads and the pool | `lwm/core/scratchpad.cpp` |
| Pure derivations used by the model | `classification.*`, `focus.*`, `stacking.*`, `window_rules.*`, `workarea.*` |
| Command grammar, actions, IPC transport, event records | `action.*`, `command.*`, `ipc*`, `events.*` |
| X connection, EWMH resources, property reads | `connection.*`, `ewmh.*`, `xproperty.hpp` |
| Signals, logging, snapshot codec, invariants | `signals.*`, `log.*`, `restart.*`, `invariants.hpp` |
| Event loop, setup, topology discovery, reload, processes | `lwm/wm.cpp` |
| Window observation (pipelined X reads) | `lwm/wm_observe.cpp` |
| Admission adapters: scan and X resources per role | `lwm/wm_manage.cpp` |
| X event translation | `lwm/wm_events.cpp` |
| Action execution: dispatch, effects, replies | `lwm/wm_actions.cpp` |
| Completion and publication | `lwm/wm_transition.cpp` |
| IPC queries, exec handoff | `lwm/wm_ipc.cpp`, `wm_restart.cpp` |

## The boundary

The shell never decides; it translates. Each X input becomes a call that carries
everything the model needs:

- Admission passes a `WindowObservation`: type, transient parent, class, title,
  desktop hint, `_NET_WM_STATE`, `WM_HINTS`, size hints, user time, dock reservation
  and, for parents LWM does not manage, their server rectangle.
- Protocol requests arrive decoded: `_NET_WM_STATE` as a `StateChange` and a set of
  `WindowState` values, ConfigureRequest and `_NET_MOVERESIZE_WINDOW` as a
  `GeometryRequest`, activation as its source and timestamp.
- Property changes arrive as new values (`title`, `window_class`, `window_type`,
  `transient`, `size_hints`, `focus_hints`, `reserve`, `user_time`).
- Topology arrives as a `Topology`: output names, rectangles and the screen extent.
- Bindings and IPC commands share one `Action` type. `execute()` maps each action to
  a named model operation and performs only the effects the model asks for: closing
  a window, launching a process (including a scratchpad launch the model requested),
  warping the pointer, restarting, or reloading the file.

Observation is pipelined. `observe()` sends identity requests for a batch of windows,
lets `State::role()` choose what each role needs, then sends only those requests, so a
startup scan costs a fixed number of round trips. Property notifications read only
the changed property with the same decoders. No X-reading callback enters the model.

The pointer grab is an effect derived from the model: the shell acquires it before a
drag starts and completion releases it once the model no longer has a drag, whether
the drag ended by release, cancellation, invalidation, reload or topology change.

## Model and ownership

`State` owns the installed `Config`. `configure()` validates workspace-count
compatibility, then installs the configuration, reconciles scratchpad slots and
reapplies every matching rule; a rejected candidate changes nothing.

Each monitor has an output name, geometry, derived workarea reservation, and a fixed
number of workspaces. One workspace is current; the previous index supports toggling.
`focused_monitor` selects the target for commands and is distinct from X input focus.

The registry has two kinds of managed window:

- `Client`: a normal window with a valid monitor/workspace placement and a `ClientMode`
  variant. `TiledMode` holds an optional remembered floating rectangle; `FloatingMode`
  holds its normal rectangle and an optional tile-return slot.
- `Fixture`: a dock or desktop with no placement, layout participation, or focus. A
  dock carries its observed reservation; every monitor's workarea is derived from the
  current docks and the screen extent whenever docks or outputs change. Popup-only
  types are mapped by the shell and never registered.

Authoritative state:

| Record | Meaning |
| --- | --- |
| Client placement, mode, requested flags, preferences | User, application and rule intent; effective classification is derived |
| `Workspace::windows` | Tiled membership and layout order; each tiled client occurs exactly once |
| `Workspace::preferred_tile` | Destination intent after relocation; actual tiled focus clears it |
| `Client::order` / `Fixture::order` | Registration order across both registries |
| `Client::mru_order` | Completed focus recency; zero means never focused |
| `fullscreen_claims_` | Fullscreen clients in interaction order, oldest to newest |
| `active_window` | Final selected managed window, or none |
| Named scratchpad slots and pool | The only scratchpad membership records |
| `drag_` | The current pointer interaction, if any |

Use `find()` where a window may be unmanaged; use `require()` once ownership is
established. Mutations maintain membership, claims and workareas, and advance a
revision counter only when a value actually changes. User-time bookkeeping is not
published and does not advance it.

Visibility, fullscreen ownership, effective layer/skip values, published
`_NET_WM_STATE` values and tiled rectangles are derived. Completion writes every
published window and root property through one cache of the bytes last written per
window and property, so unchanged values cost nothing and a fresh WM rewrites stale
ones. Per-window `Output` records cover what is not a plain property: geometry and
mapping, border, `_NET_WM_STATE` merged with other parties' atoms, and urgency
mirrored into the application's `WM_HINTS`. Caches never drive domain decisions.

## Operations and completion

An operation is one dispatched X event, IPC request, signal reload, timeout or
topology pass, or the startup scan. Handlers call `State` and retain only obligations
the model cannot express: ConfigureRequest replies, queued event facts, forwarded
restacks, crossing-event drains after a pointer release, and outputs forgotten after
an external write.

`complete_transition()` orders completion:

1. Return if neither the revision nor any obligation changed.
2. `State::settle()` ends a drag whose context changed and resolves final focus,
   recency, user time and urgency. Release the pointer if no drag remains.
3. Freeze `State`. Derive fullscreen visibility and one projection per client,
   including a tiled drag preview.
4. Hide, configure and map clients; acknowledge ConfigureRequests; commit explicit
   focus; publish client, fixture and root properties and stacking; withdraw removed
   windows.
5. Drain stale crossing events after visible movement (outside drags) or a pointer
   release, flush X, emit events, thaw, and check Debug invariants.

This orders effects; it is not an atomic X transaction. Publication must not mutate
the frozen model. A ConfigureNotify mismatch forgets cached geometry without forcing
completion; a conflicting urgency hint forces reconciliation. Explicit focus reasserts
X focus, focused state and stacking even when the selected window is unchanged.

The event loop polls X, the signal pipe and IPC. X batches stop after 64 events or
2 ms; drag motion is coalesced up to the next non-motion event. Crossing drains only
consume events already queued and defer unrelated ones; handlers never re-enter.

## Admission and topology

Roles follow identity first: a predecessor's saved fixtures keep their role and saved
clients stay clients; only newcomers choose a role from their window type.
`classify()` registers fixtures directly and turns client observations into
candidates with the admitted rule, initial mode, desktop hint and initial state.

- `admit()` handles a live map: register, place, focus when on the focused monitor
  and eligible, then claim a matching named scratchpad. An unrequested scratchpad
  match starts hidden and floating, so it neither tiles nor takes focus first.
- `adopt()` handles startup: register the whole scene (so every dock reservation
  shapes the workareas), restore a valid predecessor graph, place tiled newcomers,
  then floating newcomers with parents before children (each visited once, even with
  malformed cycles), select the pointer's monitor on a cold start, choose focus, and
  let pending launches claim remaining clients after a handoff.

Placement shares one implementation for initial placement and later size-hint
updates. A transient with a managed parent joins the parent's workspace in either
mode, as a later `WM_TRANSIENT_FOR` change would. Floating initial placement centers
on the parent or workarea unless an accepted position hint places it; later size-only
updates keep the chosen origin. Rules then apply.

Metadata updates resolve classification defaults, parent placement, changed rule
actions and pending scratchpad claims. A rule applies on metadata only when its
resulting actions change; losing a match leaves earlier actions. Pending claims take
precedence. Reload reapplies every matching rule without claiming pending launches.

Topology notifications are coalesced. `replace_topology()` rebinds workspace graphs by
output name: survivors keep complete graphs, removed outputs' clients move to output 0
after surviving tiles, and returning outputs start fresh. A changed topology fits
floating rectangles (clamping survivors, centering displaced clients) and clears
index-based fullscreen-monitor hints; a refresh reporting the same outputs changes
nothing. Focus is repaired only if it became ineligible. Restart rebinding uses the
same code.

## Placement and geometry

`insert()`, `erase()`, `relocate()` and `floating()` own membership and focus
consequences. Relocating the active client follows a shown destination or chooses
replacement focus on the source monitor; hidden destinations remember a tile
preference. A floating rectangle may be preserved, centered or translated when the
monitor changes. Application and user floating geometry joins the monitor under its
center.

Tile-to-float conversion restores a remembered floating rectangle or derives the tile
slot; wholly off-workarea rectangles are centered. Float-to-tile conversion restores
the tile slot only on its original output name and workspace; losing either
invalidates the slot permanently.

Every rectangle in the model is a frame: the window plus the border drawn inside it.
Application and rule geometry gains the client's border on entry; `presentation()`
removes it again and decides the border width and color, so layout, maximize,
fullscreen and placement never account for borders separately. `project()` returns
every client in registration order with an optional presentation: absent means hidden.
`normal_geometry()` derives a tiled client's slot even when minimized, off-workspace or
fullscreen. Fullscreen, maximize and drag previews affect presentation, never the saved
normal rectangle.

Layout arrangement, split hit-testing and drop targeting share one subdivision of the
workarea into frames separated by padding.
Master-stack is a sequence of cuts: split 0 divides master from stack, later splits
divide stack slots from the remainder. Monocle has no resize boundaries. Participants
are eligible tiles of the current workspace, then sticky tiles of others; fullscreen
clients take no slot.

`write_geometry()` owns WM-driven configure requests, sync messages and synthetic
ConfigureNotify. An unchanged rectangle is skipped, but an outstanding ConfigureRequest
still receives its acknowledgement.

## Visibility, focus and stacking

A client is in view when it is not iconic and is sticky or on its monitor's current
workspace; showing the desktop leaves only sticky clients in view. Outside show-desktop
mode, the newest fullscreen claim in view owns its monitor and suppresses other normal
clients except its managed transient descendants. `FullscreenVisibility` computes
owners and exemptions once per pass, bounding cyclic parent hints.

Keep three concepts distinct: `iconic` is explicit minimization, visibility is
derived, and `Output::hidden` means LWM moved the window off-screen. Normal clients
are mapped once; workspace hiding does not unmap them.

`fullscreen()` is idempotent and used by rules; `request_fullscreen()` renews the
claim and is used by interactions and restoration. Entering fullscreen clears
maximize. Focus eligibility requires visibility, an input hint or `WM_TAKE_FOCUS`,
and no show-desktop mode.

`focus()` deiconifies and selects the placement, then falls back if the target is
suppressed. Operations that choose a window as policy (workspace and monitor
navigation) fall back explicitly; nothing else repairs focus as a side effect.
`settle()` is the one repair: a selection that lost its client or eligibility, or an
empty selection, takes the focused monitor's fallback, and it records only the final
choice. Focus cleared on purpose (`focus(XCB_NONE)`, used when the pointer moves to an
empty part of another monitor) stays empty until a later choice; `hover()` implements
focus-follows-pointer and pointer monitor selection. Fallback
prefers the destination tile, then the most recent eligible tile of the current
workspace, then sticky tiles, then floating clients by recency. Cycling keeps one MRU
order across consecutive steps and rechecks eligibility at each step.

`stacking::compute_order()` derives one bottom-to-top order for clients and fixtures;
its tiers and transient constraints are specified in
[X11.md](X11.md#desktops-and-root-properties). Completion reconciles a changed order,
a forwarded restack or an explicit focus against a fresh QueryTree, moving the fewest
siblings.

## Pointer interactions and scratchpads

A drag is either a window move/resize or a split resize. Domain changes (focus,
floating conversion, leaving maximize) start only after the shell holds the grab.
Floating drags change normal geometry; a tiled move is a preview until release, when
the drop slot is translated into a membership index without reordering hidden members
or sticky guests. A drag ends when its client's eligibility or the captured split
context changes, and on reload, topology change and restart.

Named scratchpad slots are empty, launch-pending or claimed; the pool is an ordered
list whose last entry is the recall target. Both hold ordinary clients and hide them
with iconic state. Recall relocates to the focused monitor's current workspace; named
clients float at their configured size and pooled clients keep their mode and offset.
Cycling recalls an invisible or remote pool target and focuses an inactive local one;
an active local target is hidden and rotated to the front before the next is
recalled, so a one-window pool toggles. Removing members preserves the remaining
order. A toggle can ask the shell to launch; the slot becomes pending only after process
creation succeeds. Claims and pending launches survive reload and restart while their
names survive.

## Configuration and events

A private reflect-cpp schema validates TOML over toml++; the loader resolves names,
bounds, command references and compiled regexes into plain runtime values. Bindings'
`action` strings and IPC requests share one command parser; bindings reject queries
and subscriptions. Execution copies a binding's action because reload can replace the
configuration that owns it. Startup options own logging policy; reload does not.

Event records are the subscription wire schema; their C++ type names are the public
event names. IPC query results are typed view records serialized by the same writer.
Completion emits workspace switches and the final focus change first, then queued
map/unmap facts, then action and reload outcomes, then `state_change` when the exposed
state differs from the last emitted snapshot.
[IPC.md](IPC.md#subscriptions) owns ordering and synchronization guarantees.

## Restart and process lifetime

`SignalPipe` owns SIGHUP/SIGCHLD handling through a close-on-exec self-pipe. Main
creates it before logging and destroys it after logging shutdown; a failed exec
reconstructs the WM with both process-level owners.

Restart retains the predecessor's X resources so closing its connection cannot reset
an otherwise empty server. `_LWM_RESTART_OWNER` marks the retained window; the
successor validates the marker and kills that client after claiming the screen.

`State::snapshot()` stores private intent in `_LWM_RESTART`: placements, modes,
preferences, urgency, focus ranks, workspace graphs, scratchpad claims, pending
launches, pool order, fullscreen claims and fixture roles with registration ranks.
Application properties are observed afresh; derived rectangles, dock reservations
and publication caches are not saved. The live value types are the snapshot schema,
encoded with reflect-cpp over yyjson inside a CARDINAL envelope (format word, byte
length, zero padding). Decoding rejects duplicate or extra fields, ambiguous variant
tags and narrowing, then validates the persistent graph with the same validator Debug
builds use. Any malformed or incompatible snapshot is rejected whole and windows are
adopted afresh. The current format is 14; schema changes require a bump and there is
no migration.

Restoration overlays saved intent on surviving observations, installs the saved
graph, rebinds it to discovered outputs through the live topology code (folding fewer
workspaces into the last), filters vanished clients, admits newcomers on the restored
current workspace unless a desktop hint places them, keeps surviving fullscreen claims
before new ones, and restores scratchpad state. Autostart is suppressed whenever a
predecessor handed over.

## Logging

`core/log` owns Quill's journal/stderr sinks, one worker and a bounded dropping queue.
The WM thread submits and never waits for delivery; normal shutdown joins the worker.
Invariant failures abort without draining. Exec neither flushes nor stops logging.
INFO reports lifecycle, configuration and topology outcomes; DEBUG explains domain
decisions; TRACE records bindings and submitted geometry. Launch failures name the
executable and failing stage, never arguments. Asynchronous X errors are rate limited;
teardown-related BadWindow/BadDrawable errors are DEBUG.

## Invariants

Types prevent mode/storage disagreement, fixture placement and simultaneous
Above/Below preferences. `invariants::validate()` checks the persistent graph (valid
placement, exact tiled membership, unique identities and ranks, consistent fullscreen
claims, exclusive scratchpad ownership, a managed active window), then live facts:
registry bounds, fullscreen flags, iconic preferences and focus eligibility. Debug
builds check on event-loop entry and after every completed operation, aborting on
violation. Model invariants do not establish X delivery or ordering; integration tests
cover those.
