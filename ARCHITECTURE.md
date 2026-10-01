# Architecture

This document defines LWM's internal runtime model and the ownership boundaries
maintainers should preserve. User setup belongs in [README.md](README.md); external X11
behavior belongs in [X11.md](X11.md); the local wire contract belongs in
[IPC.md](IPC.md).

## Components

| Area | Responsibility |
| --- | --- |
| `src/app/main.cpp`, `cli.*` | process startup, config selection, logging options, exec restart |
| `src/app/lwmctl.cpp` | supported command-line IPC client |
| `src/lwm/config/` | strict TOML parsing into typed values and built-in defaults |
| `src/lwm/layout/` | pure master-stack and monocle geometry, split ratios, hit testing |
| `src/lwm/core/types.hpp` | domain records: clients, fixtures, monitors, workspaces, geometry |
| `src/lwm/core/state.*` | the domain model: registry, placement, mode, focus memory, scratchpads, derived visibility |
| `src/lwm/core/classification.*` | window-type defaults and effective layer and skip values |
| `src/lwm/core/action.*` | every user-triggerable WM operation, shared by bindings and IPC |
| `src/lwm/core/focus.*`, `stacking.*`, `floating.*`, `policy.hpp` | pure focus selection, stacking order, floating geometry, desktop numbering |
| `src/lwm/core/restart.*` | versioned exec-handoff snapshot codec |
| `src/lwm/core/ewmh.*`, `xproperty.hpp`, `workarea.*` | EWMH atoms, validated property reads, strut projection |
| `src/lwm/core/ipc_command.*`, `ipc_server.*`, `events.*` | IPC grammar, bounded connections, typed subscription events |
| `src/lwm/core/log.*` | Quill configuration, standard sinks, process-boundary lifecycle |
| `src/lwm/wm.cpp` | X setup, event loop, configuration reload, X reads and protocol messages |
| `src/lwm/wm_manage.cpp` | classification, registration, rules, restart adoption |
| `src/lwm/wm_events.cpp` | X event and client-message handlers |
| `src/lwm/wm_actions.cpp` | the executor for `Action`s and window, workspace and monitor operations |
| `src/lwm/wm_focus.cpp`, `wm_drag.cpp`, `wm_scratchpad.cpp` | focus assignment, pointer interactions, scratchpads |
| `src/lwm/wm_transition.cpp` | operation completion: projection of state onto X and subscribers |
| `src/lwm/wm_ipc.cpp`, `wm_restart.cpp` | IPC queries and JSON, exec handoff |

`WindowManager` owns one event loop. It polls the X connection, the SIGHUP self-pipe,
and IPC connections. All domain mutation is single-threaded; only log delivery runs on a
worker. X events, key bindings, IPC requests, reloads and timeouts all end in the same
completion step.

The design has two halves. `State` is a pure domain model with no X calls: it holds
authoritative records, performs named mutations, and derives visibility, fullscreen
ownership and focus eligibility on demand. `WindowManager` is the imperative shell: it
reads X, calls `State`, and at the end of every operation projects the model onto the X
server. Nothing records which outputs a mutation affected.

## State model

Each RANDR monitor owns a fixed-size vector of workspaces and identifies one as current.
The same configured workspace names are repeated per monitor.
[X11.md](X11.md#desktops-and-root-properties) defines the external EWMH desktop
numbering; internal placement uses separate monitor and workspace indices.

`focused_monitor` is the target for commands. It usually follows the focused window or
pointer but is distinct from X input focus.

The registry holds two kinds of records:

- A `Client` is a managed normal window. Its monitor and workspace are always a valid
  placement. `Client::mode` is either `TiledMode` or `FloatingMode`, and `kind()` is
  derived from it. A tiled client appears in exactly one `Workspace::windows` vector.
- A `Fixture` is a dock or desktop window. Fixtures are listed, stacked (desktops in the
  Below tier, docks in the Above tier) and classified, but they have no placement and
  never take part in layout, focus, or `_NET_WM_STATE` requests. Docks contribute
  struts. Popup-only window types are mapped directly and never registered.

The important authorities are:

- `Client`: placement, mode, requested state (fullscreen, iconic, sticky, maximize,
  modal), explicit preferences, urgency provenance, protocol hints, and the actions of
  the rule matched last.
- `TiledMode`: the layout target and the floating rectangle to restore when floated
  again. `FloatingMode`: the one normal rectangle and the tile slot to restore when
  tiled again on the same workspace.
- `Workspace::windows`: tiled membership and layout order; `focused_window` and
  `focus_history`: remembered tiled focus, never an iconic client.
- `Client::mru_order`: focus recency. `Client::fullscreen_claim`: fullscreen claim
  recency, zero exactly when fullscreen is disabled.
- `active_window`: the focused managed window.
- The named scratchpad slots and the pool: the only scratchpad membership records.

Derived values are functions, not stored fields:

- Effective layer and skip-taskbar/pager values combine classification defaults,
  explicit preferences, modal and fullscreen state (`core/classification`).
  [X11.md](X11.md#classification) defines precedence between defaults, application
  requests, user actions, and rules.
- A client is in view when it is not iconic and is sticky or on its monitor's current
  workspace; showing the desktop leaves only sticky clients in view.
- The fullscreen owner of a monitor is the fullscreen client in view with the most
  recent claim. There is no stored owner and no pending owner.
- A client is visible when it is in view and not suppressed by another monitor owner
  (its managed transient descendants are exempt). It can hold focus when it is visible,
  accepts input or `WM_TAKE_FOCUS`, and the desktop is not shown.

`State` exposes const views only. Every mutation is a named operation that keeps
membership, focus memory and scratchpad claims consistent, and increments a revision
counter. Layout targets are the exception: `place_tile()` stores recomputed geometry
without counting as a change.

## Lifecycle and transitions

An operation is one dispatched X event, IPC request, signal reload, timeout and
coalesced topology pass, or startup scan. Handlers mutate `State` and record only
obligations that are not projections of state: ConfigureRequest acknowledgements,
subscription facts, a forwarded restack, a workarea refresh after a dock change, and an
explicit crossing-event drain. `complete_transition()` then runs in this order:

1. Skip everything when the revision is unchanged and nothing is pending.
2. Refresh dock workareas if requested. Validate an active drag, repair focus that can
   no longer hold, and arrange the tiles of every monitor's current workspace. Clear
   urgency on an explicitly focused window.
3. Freeze the model. Project each client: hide it, write geometry (skipping an
   unchanged rectangle and border), acknowledge remaining ConfigureRequests, map new
   windows, commit an explicit focus request, and publish border color, `WM_HINTS`
   urgency, `_NET_WM_DESKTOP`, `WM_STATE`, allowed actions and `_LWM_WINDOW_CLASS`,
   `_NET_WM_FULLSCREEN_MONITORS`, and the owned `_NET_WM_STATE` atoms.
4. Publish fixtures and root properties, reconcile stacking, and withdraw removed
   windows.
5. Drain crossing events after a visible change, flush X, and emit subscription events.
   Debug builds then check invariants.

Publication compares the projection with `Output`, the record of what LWM last wrote
for each window, and with `RootOutput` for the root window. Only differences are sent.
An external change is handled by forgetting the affected field: a conflicting
ConfigureNotify forgets geometry so the next publication rewrites it (a mismatch is
usually LWM's own superseded configure, so it does not force one). Observed `WM_HINTS`
urgency that differs from the cache forgets the published urgency and forces completion,
independently of urgency provenance or focus. Missing hints count as a cleared hint;
publication reconciles it with the remaining urgency sources. Root lists are written on
the first publication even when empty, replacing lists left by a previous window manager.
Inputs to the projection that are not
in `State` (a drag preview, reloaded appearance, a forgotten field) mark the
presentation dirty. `_NET_WM_STATE` updates read the property once, replace only the
atoms LWM owns, and preserve the rest. An explicit focus request reasserts X input
focus, `WM_TAKE_FOCUS`, the focused state and server stacking even when nothing
changed.

Workspace switches and focus changes for subscribers are derived the same way: the
current workspace per output name and an explicit focus request are compared with the
last completion. Other events are typed facts recorded where they happen and serialized
in one place. A new subscription records the exposed state as its baseline, and
`state_change` is emitted when the exposed state snapshot differs from the
last one sent.

The outer loop flushes direct protocol replies after each bounded event batch. A batch
consumes at most 64 X events or 2 ms between events, counting coalesced motions. IPC,
signals, and deadlines are serviced between batches. This prevents drain-until-empty
starvation; an individual handler can still take longer than 2 ms. After a crossing
barrier, only events already in XCB's queue are drained, so fresh socket input cannot
extend it forever.

This is ordered completion, not rollback or an atomic X-server transaction. Handlers may
read X and register protocol resources while mutating state. Visibility, geometry,
focus publication, stacking, and event delivery belong to completion. Drag motion
compression happens in outer dispatch and stops at the first non-motion event. A
handler never recursively dispatches another event.

## Client registration and placement

`manage_window()` classifies a new or adopted window and registers a client or fixture.
For a client it captures metadata and hints, chooses placement from `_NET_WM_DESKTOP`
or the focused monitor, computes floating placement, and records the matched rule. New
clients are mapped at completion; adopted clients are not remapped. A window matched by
an unclaimed named scratchpad starts floating and iconic.

Metadata handlers update the property through `State` and match rules once. A rule is
applied only when its actions differ from the ones remembered on the client; losing a
match leaves previous actions in place. Reload reapplies every matching rule. Pending
scratchpad claims are checked independently of rule changes. Precedence and replay
semantics are specified in [X11.md](X11.md#classification).

`unmanage_window()` removes a client or fixture and repairs focus. Completion then
writes `WM_STATE=WithdrawnState` and removes the focused state.

`State::relocate()` moves a client and chooses what happens to a floating rectangle
when the monitor changes: preserve it, center it, or translate it within the workarea. A
tiled reorder within one workspace keeps membership and focus history.
`State::floating()` records explicit mode intent. Tile-to-float conversion restores the
remembered floating rectangle, otherwise the layout target; a rectangle lying entirely
outside the monitor's workarea is centered on it, because an unarranged tile can still
hold an off-monitor initial or hotplug rectangle. Float-to-tile conversion remembers the
floating rectangle and restores the tile slot on the same workspace. A slot identifies
its original output by name and its workspace by index, so monitor reordering cannot
retarget it. Visiting another workspace while floating keeps that identity; tiling
elsewhere uses normal insertion. Topology replacement and client admission discard a
slot if its original output or workspace no longer exists. Reconnecting an output
does not revive a discarded slot. Type and transient
updates apply the default mode unless the user chose one or a scratchpad owns the
window.

Hotplug is a single `State::replace_monitors()` operation: discovery supplies output
geometry and freshly read dock struts, surviving outputs keep their workspace graphs,
clients are reassigned, and floating rectangles are clamped (survivors) or centered
(displaced clients).

## Layout and geometry

`Layout` computes rectangles and resize boundaries without X calls. Master-stack is
evaluated as an iterative sequence of cuts: split 0 divides master from stack, and split
i divides stack slot i from the remaining slots. Monocle gives every window the content
rectangle and has no resize boundaries. Arrangement, drop selection, and resize hit
testing share the same calculation; they do not allocate an intermediate tree.

`tiled_participants()` supplies the same ordered windows to arrangement, split
hit-testing, and drop targeting: visible members of the current workspace, followed by
visible sticky members of other workspaces. Fullscreen windows get fullscreen geometry
rather than layout slots.

Floating clients keep one normal rectangle. Maximize projects selected axes onto the
workarea and fullscreen projects onto its target monitors; neither overwrites the normal
rectangle. Tiled clients keep maximize flags as a preference without presenting them.
Managed transient placement uses the parent's intended presentation; server geometry
reads are for initial capture and unmanaged parents.

`write_geometry()` owns WM-driven configure requests, sync notifications, and synthetic
ConfigureNotify events. A client's ConfigureRequest creates a separate reply obligation,
so skipping an unchanged rectangle never skips a required acknowledgement.

## Visibility

LWM maps a normal client once, then hides it by moving it to `OFF_SCREEN_X`. Three terms
must remain distinct:

- `iconic`: the client is logically minimized; ICCCM `WM_STATE` is
  `IconicState` and EWMH includes `_NET_WM_STATE_HIDDEN`.
- visible: derived from state as described above.
- hidden: LWM has physically moved the client off-screen (`Output::hidden`).

Normal workspace changes therefore change hidden, not `WM_STATE`, and an incoming
`UnmapNotify` means client withdrawal rather than a workspace change. Layout, focus
eligibility, and drag validity use derived visibility, never the output record.

## Fullscreen and stacking

At most one client in view owns fullscreen on a monitor: the one with the most recent
claim. `State::fullscreen()` assigns the setting idempotently: entering fullscreen
establishes a claim, leaving it clears the claim, and repeating the setting changes
nothing. Rules use this assignment. `State::request_fullscreen()` enables fullscreen
and renews its claim even when already enabled; explicit fullscreen requests use it.
Admission of an initially fullscreen client and restoration from minimized use the same
claim operation. It also clears maximize and requests focus repair. Restart restores
the saved claim order instead of generating interactions from saved settings.
Other visible clients on that monitor are suppressed and hidden, except managed
transient descendants of the owner (see [X11.md](X11.md#state-and-visibility)).
Fullscreen state may remain set on iconified or off-workspace clients; ownership is
effective only when they return to view. Showing the desktop removes fullscreen
ownership. `_NET_WM_FULLSCREEN_MONITORS` changes geometry only and
does not create cross-monitor ownership.

`State::fullscreen_visibility()` derives owners and descendant exemptions for a
read pass. It reverses parent links once and walks outward from each owner with a
visited set, without recursion. Work is O(n) per owner, including cyclic hints;
with no owner it skips ancestry construction. Completion shares this temporary
projection between layout, client visibility, and stacking. It is discarded after
the pass and needs no persistent invalidation machinery.

`stacking::compute_order()` computes one bottom-to-top order for clients and fixtures.
Its hidden prefix includes iconic, off-workspace, and fullscreen-suppressed clients.
Desktop fixtures use the Below tier and docks use Above. Clients use Below, Normal,
Above, or Fullscreen according to their effective layer. Within a tier, floating clients
are above tiled clients, the active window is raised within its kind, then registration
order and window ID break ties.

Visible floating transients must follow their visible managed parents, even across
tiers. Among windows whose parent constraint is satisfied, the lowest base-ranked window
is emitted next. Missing or hidden parents impose no constraint. Each transient has at
most one parent: an iterative parent walk identifies cycles and ignores the parent edge
of the lowest base-ranked member of each cycle (including self-links). These decisions
are local to ordering; client hints are not rewritten. Sorting and dependency ordering
take O(n log n) time and O(n) temporary space, without recursion.

Server order changes only through LWM's own restacks, so completion reconciles stacking
when the desired order differs from the last published one, when a restack of an
unmanaged window or fixture was forwarded, or when focus was explicitly requested. It
then compares the desired order with a fresh root `QueryTree` reply. A longest increasing
subsequence identifies windows already in order; each remaining window needs one
sibling move, so external restacks are repaired too. The same order is published as
`_NET_CLIENT_LIST_STACKING`.

## Focus

`focus_window()` records focus intent. It deiconifies when necessary, switches the
target monitor's workspace, falls back when the target is suppressed by fullscreen, and
updates active focus, memory and recency. Completion publishes the final choice once:
it sends `WM_TAKE_FOCUS` when advertised and sets X input focus. Intermediate choices
within one operation do not produce focus events. A managed client accepts focus when
`WM_HINTS.input` is true or it advertises `WM_TAKE_FOCUS`.

Completion repairs focus when the active window can no longer hold it, and falls back
when a fullscreen or input-hint change asked for it and nothing is focused. Showing the
desktop deliberately leaves focus cleared.

Fallback selection prefers the workspace's remembered focus, its bounded focus history,
reverse tiled order, sticky tiled clients on the monitor, then visible floating clients
by recency. `core/focus` reads `State` directly and derives fullscreen visibility once per
selection, so candidate eligibility checks are constant-time and allocate no candidate
lists.

Cycling retains only window IDs in descending recency order; consecutive steps keep that
order while recording actual focus recency. Each step checks current eligibility, so
removed or newly ineligible windows are skipped, and windows that become eligible can
join the traversal. Ordinary activation (including same-window activation), a changed
monitor/workspace or active window, or a new registration starts a fresh traversal,
detected from `State`'s recency and registration counters. Equal recency is ordered by
newest registration, then window ID.

Visibility-changing transitions finish with `flush_and_drain_crossing()` before
accepting subsequent pointer-driven focus. This round trip discards stale crossing and
motion events that could otherwise overwrite the intended focus under
focus-follows-mouse.

## Commands and events

`Action` is the one representation of a user-triggerable operation. Configuration
bindings and the IPC grammar both parse into it, and `execute()` performs it for either
source, returning the IPC reply text. An action's name is its configuration key and its
`key_action` event value. Layout actions record their `layout_change` event in the
executor, so bindings and IPC report identically. Read-only IPC queries are answered
from state without executing anything.

## Pointer interactions

One optional `Drag` owns an acquired pointer grab, the initiating button, and
start/latest pointer coordinates. Its operation is either a window move/resize or a
tiled split resize. Failed acquisition leaves domain state unchanged; tiled-to-floating
conversion and exiting maximize happen only after acquisition. Window drags retain their
expected kind and placement. Completion cancels them when the client disappears, becomes
hidden or fullscreen, presents maximized (floating only), changes kind, or is moved
elsewhere by another operation. Split drags retain the participants, workarea,
workspace, and strategy defining their split. Config reload, topology reconciliation,
and restart cancel the interaction before replacing its context.

Drops translate a visible slot back to a membership insertion point, excluding the
dragged window. Hidden members and sticky guests are not reordered. Dropping on the guest
suffix appends to the destination's current workspace.

Window movement and edge-aware resize share pure, saturating geometry arithmetic.
Floating drags update the normal rectangle. Tiled movement is a presentation override
applied during completion; it does not change membership until release. Cancellation
removes the override without committing a reorder. The initiating button's release
applies its final coordinates before ending the drag; unrelated releases do not end it.
All endings release the pointer and drain crossing events.

## Scratchpads

Named and pooled scratchpads remain ordinary managed clients. `State`'s named slots
(empty, launch pending, or claimed) and the pool are the only membership records. An
explicitly hidden scratchpad is iconic; workspace and fullscreen visibility remain
derived. Showing one relocates it to the focused monitor's current workspace and
deiconifies it: named scratchpads float at their configured size, while pooled windows
keep their mode (a pooled tile rejoins the tiled order, a floating window keeps its
offset within the workarea).

The pool order also owns recall selection: the last entry is the target. Stashing
appends a new target. Like named toggles, cycling recalls a target that is invisible or
on another monitor, and focuses a visible inactive target here. Cycling an active
target here hides it, moves it to the front, and recalls the next target. A one-window
pool simply toggles. Selection does not depend on why a window is invisible, and every
member participates in rotation. Removing a member preserves the
remaining order; restart preserves the order and therefore the next recall target.

A named scratchpad enters launch-pending state only after successful process creation
and exec. Process exit is not used as a window-creation signal: a launcher may delegate
to another process. Pending launches suppress duplicate toggles until a matching window
arrives or the user explicitly cancels the pending launch through IPC. Pending requests
survive exec restart and failed-exec recovery while their configured names survive.
Cancellation neither kills the program nor prevents a late matching window from being
claimed.

## Configuration and reload

Configuration loading validates a candidate and converts it to typed values before
changing the WM: layout strategies, rule actions (with workspace names resolved to
indices), bindings as `Action`s, and compiled regexes. Monitor names in rules resolve
against current outputs when applied. Runtime matching and input handling use this data
directly. Key bindings are grabbed on the root window only; root grabs take precedence
over any client grab. Resolving a key copies its action, because executing it may reload
and replace the configuration that contained it.

Reload validates workspace-count compatibility before replacing `Config`. The state
owner reconciles scratchpad names with the accepted configuration. Surviving scratchpad
names retain both claimed windows and pending launches, even when their matchers change.
Removed names release their windows. The WM then regrabs input and reapplies matching
rules. Invalid configuration leaves the active configuration and runtime claims
unchanged; this does not promise rollback of X-server errors during application. The
user-visible reload limits are recorded in [README.md](README.md).

## Monitor topology

RandR screen, output, and CRTC notifications mark topology dirty. The event loop
coalesces each batch before reconciliation. Discovery supplies fresh output geometry;
surviving monitor names retain complete workspace state, including tiled order, focus
history, split ratios, and layout strategy. Removed outputs' clients move to monitor 0,
with surviving workspace order/focus taking precedence. Returning outputs start fresh
and do not reclaim relocated clients. Fullscreen monitor-index hints are cleared because
the indices may have changed.

## Process resources

`SignalPipe` owns the SIGHUP/SIGCHLD handlers and nonblocking, close-on-exec self-pipe
for the process lifetime. Main creates it before starting the logging worker and
destroys it after logging shutdown. WM reconstruction after a failed exec reuses this
owner; failed constructors cannot leak pipe descriptors or replace process handlers.
Reconstruction is only for an explicit restart whose exec failed after a prepared state
handoff. Configuration, construction, event-loop, and restart-preparation exceptions
terminate with a nonzero status and a diagnostic naming the failed phase.

X resources belong to the WM's connection and are released when it closes, including on
initialization failure. Restart is the exception: closing the last X connection in
DestroyAll mode resets an otherwise empty server and loses saved root properties. The
old WM therefore retains its connection's resources, marking its internal window and the
root with `_LWM_RESTART_OWNER`. After claiming root ownership, the replacement validates
that marker and kills the retained predecessor through X11 before continuing
initialization. This preserves empty-display restart without accumulating old X clients.
Normal shutdown and constructor failure keep DestroyAll semantics.

`core/xproperty.hpp` owns raw property replies and validates their type, format, and
completeness before decoding. Callers retain protocol-specific cardinality and fallback
decisions.

## Restart

Graceful restart encodes `State::snapshot()` into the root property `_LWM_RESTART`:
focus, showing-desktop, per-workspace layout, ratios, tile order and remembered focus,
and per-client placement, mode, geometry, preferences, urgency and fullscreen-monitor
hints. Named scratchpad records contain either a claimed window or a pending launch;
empty slots need no record. Floating clients also retain their tile-return slots,
including original output names.
The snapshot also records oldest-to-newest fullscreen claims, including hidden and
iconic clients. Claim order is authoritative even when no owner is currently visible;
it is independent of focus recency and X stacking/adoption order.

Each saved monitor records its output name and geometry. Indices in the handoff refer
to that saved graph, never directly to the successor's enumeration. Startup registers
fixtures and reads all dock reservations before restoring client placement.
`State::restore_workspaces()` first rebinds the handoff onto discovered outputs through
the same workspace-transfer policy as live hotplug: surviving names retain their
workspaces; removed outputs append their tiles to output 0 after surviving members,
without replacing its layouts or focus memory. Client placement and the focused monitor
use that same mapping. A smaller configured workspace count folds removed workspaces
into the last one; additional workspaces retain configuration defaults.

A changed output topology fits floating rectangles using the live-hotplug policy and
clears fullscreen-monitor index hints. An unchanged topology preserves intentional
floating geometry and hints. Rebinding happens once, before client adoption, so windows
without a record join the restored current workspace. Adoption places each saved client
directly from its record instead of replaying rules, and `State::restore_membership()`
then applies tile order, remembered focus, recency and scratchpad claims. After restoring
saved focus and claims, pending launches can claim matching adopted clients, including
windows that arrived during the handoff. Later map and metadata events fulfill the same
pending request. Removed scratchpad names and disappeared claimed windows are skipped.
Fullscreen counters are rebuilt from the saved claim order, skipping clients that disappeared or no longer request fullscreen. Newly adopted fullscreen
windows without a saved claim come afterward, in their adoption claim order; subsequent
requests outrank all restored claims. Autostart is skipped when a predecessor
handed over, even if its snapshot was unusable.

The snapshot is private to one format version. A different format word, a truncated or
out-of-range record, or trailing data rejects the whole snapshot; windows are then
adopted as on a fresh start, which keeps their EWMH desktops. There is no cross-version
compatibility layer. Requested client state such as fullscreen, sticky, iconic and
maximize travels through the windows' own `_NET_WM_STATE`.

## Logging

`core/log` configures the Quill version pinned in [CMakeLists.txt](CMakeLists.txt) with
its standard `SystemdSink` (default) or `ConsoleSink` (stderr), one worker, and a
preallocated 256 KiB `BoundedDropping` producer queue. The WM thread alone submits
records and controls the lifecycle. `LWM_LOG_*` macros add only a null guard around
Quill's macros; Quill owns level gating, copied arguments, formatting, source metadata,
control character handling, rate limiting, and overflow reporting. There is no custom
sink, argument codec, truncation policy, or transport protocol in LWM.

Formatting and delivery happen on the worker. Its transit buffer is limited to 256
events. Idle polling is 100 ms at INFO or higher and 10 ms at DEBUG/TRACE. At `off`, no
logger, worker or producer queue is started. Immediate flush and Quill signal handlers
are disabled. Normal event handling never flushes or waits for the worker. A full
producer queue rejects new records; a record larger than that queue is rejected rather
than truncated. Call sites use fixed format strings and should log concise diagnostics,
not arbitrary payloads.

Normal shutdown disables submissions and joins the worker after draining queued records.
**This can wait indefinitely for a stalled output destination.** The bounded queue
protects event handling and limits queued data; it does not make sink I/O nonblocking.
Invariant failures abort without draining so output cannot hold up the abort; aborts and
unexpected crashes can lose queued records.

Exec restart deliberately does not flush, stop, or recreate logging. Successful `exec`
replaces all threads and closes the journal's close-on-exec socket; queued messages can
be lost. Failed exec leaves the existing logger running, including its instance and
notification count. There is no logging recovery state machine. The worker alone blocks
SIGPIPE so a closed stderr pipe becomes a reported output error without changing the
signal disposition inherited by launched applications.

The journal sink uses libsystemd for native framing, timestamps, and source/severity
fields; stderr uses Quill's standard formatting and color configuration. Tests redirect
the journal address only inside a dedicated probe.

A non-logging error callback retains a notification count and the latest 1 KiB of
notification text for `lwmctl log status`. Its mutex is used only by backend
notifications and explicit status queries, never ordinary submissions. Quill reports
queue overflow in summaries, so notifications are not a lost-record counter. Missing
journal service is silently accepted by libsystemd. Neither sink confirms durable
storage. Initialization failure is a startup error reported by the CLI on stderr.

Logging policy is fixed by startup options, not TOML reload:

- INFO records process readiness, configuration outcomes, changed monitor topology,
  restart and shutdown reasons. Ordinary window operations do not produce INFO records.
- WARN/ERROR identify actionable failures. Launch failures include the executable,
  origin, failing stage and error code, without arguments or shell contents. Spawning
  a shell successfully does not imply that commands inside it succeeded; those errors
  remain the shell's responsibility on inherited stderr.
- DEBUG records state changes at their owning boundary: focus, workspace, client
  placement and kind, fullscreen requests and resolved ownership. Classification
  records show natural versus rule-resolved kind and transient parent, without titles.
- TRACE records binding resolution, repeat suppression and submitted geometry. A
  submitted rectangle is not an acknowledgement from the X server. Unchanged geometry
  produces no submission record. Raw key releases and intermediate repeat predicates
  are not narrated.

Asynchronous X errors include numeric error code, major/minor request opcode,
resource and full sequence number. BadWindow and BadDrawable can arise from client
teardown races and are DEBUG; other errors are WARN. Each category has a five-second
Quill rate limit, as do recurring RandR and config-reload warnings. Suppression bounds
noise but can hide distinct errors within the interval; the retained record is not
an inventory of every failed request. No diagnostic lookup adds X round trips.

Stderr records include time, severity and source location; journal records use native
journal timestamps and metadata. Logs are best-effort explanations, not an audit trail
or a state database. Use IPC snapshots for current state and external IPC, CPU and X
request tools for performance measurement.

## Invariants

`ClientMode` makes kind and kind-specific storage agree, fixtures cannot carry
placement, and `LayerHint` cannot represent both Above and Below. Other relationships
require runtime checks. `invariants::validate(State const&)` checks after every
completed operation that:

- each tiled client appears in exactly one workspace and each workspace entry
  resolves to a tiled client with that placement;
- every client has a valid monitor and workspace, and no window is both a client and a
  fixture;
- a client has a nonzero fullscreen claim exactly when fullscreen is enabled, and
  fullscreen excludes maximize;
- remembered focus is a member tile that is not iconic;
- every named scratchpad claim and pool entry refers to a managed client, and no client
  has two scratchpad records;
- the active window is managed and can hold focus.

Debug builds run it on entry to the event loop and after each completed operation,
including startup, X events, IPC, signal reloads, and timeout/topology work. A
violation logs the reason and aborts; Release builds omit these checks. Visibility and
fullscreen ownership need no check because they are derived. A generated sequence test
drives thousands of `State` operations and validates after each. X properties and
observable ordering require integration tests.
