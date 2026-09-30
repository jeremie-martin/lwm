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
| `src/lwm/config/` | strict TOML parsing and built-in defaults |
| `src/lwm/keybind/` | key grabs and lookup in the prepared configuration |
| `src/lwm/layout/` | pure master-stack and monocle geometry, split ratios, hit testing |
| `src/lwm/core/log.*` | Quill configuration, standard sinks, process-boundary lifecycle |
| `src/lwm/core/types.hpp` | domain state: clients, monitors, workspaces, geometry |
| `src/lwm/core/stacking.*` | desired global stacking order from clients and resolved fullscreen owners |
| `src/lwm/core/focus.*` | shared client eligibility, fallback, cycle ordering, and pointer monitor selection |
| `src/lwm/core/policy.hpp` | pure visibility, workspace, fullscreen, and hotplug decisions |
| `src/lwm/core/ewmh.*` | EWMH atoms, classification, and property I/O |
| `src/lwm/core/restart.*` | bounded restart record encoding/decoding without X or live-state mutation |
| `src/lwm/core/ipc_server.*` | bounded independent connections, deadlines, ordered subscription output |
| `src/lwm/core/ipc_command.*` | shared command grammar, argument validation, and CLI help metadata |
| `src/lwm/wm_ipc.cpp` | command handling and IPC query results |
| `src/lwm/wm.cpp` | construction, client registration/removal, visibility, stacking, geometry writes |
| `src/lwm/wm_transition.cpp` | operation boundaries and ordered completion of accumulated effects |
| `src/lwm/wm_rules.cpp` | classification, rule application, and runtime reevaluation |
| `src/lwm/wm_ewmh.cpp` | root properties, client lists, workareas, EWMH desktop projection |
| `src/lwm/wm_events.cpp` | X event dispatch, client messages, property changes, RANDR |
| `src/lwm/wm_focus.cpp` | focus assignment, fallback, and cycling |
| `src/lwm/wm_workspace.cpp` | workspace and monitor commands |
| `src/lwm/core/state.*` | authoritative mutations, membership, preferences, restoration, and completion effects |
| `src/lwm/wm_floating.cpp`, `wm_drag.cpp` | floating geometry and pointer-driven move/resize/reorder |
| `src/lwm/wm_restart.cpp`, `wm_scratchpad.cpp` | exec handoff and scratchpad state |

`WindowManager` owns one event loop. It polls the X connection, the SIGHUP self-pipe,
and IPC connections. All domain mutation is single-threaded; only log delivery runs on a
worker. The CLI and server share a command grammar, while tests use independent wire
peers. IPC callbacks, X events, config reloads, and timeouts enter the same mutation and
completion model.

## State model

Each RANDR monitor owns a fixed-size vector of workspaces and identifies one as current.
The same configured workspace names are repeated per monitor.
[X11.md](X11.md#desktops-and-root-properties) defines the external EWMH desktop
numbering; internal placement uses separate monitor and workspace indices.

`focused_monitor_` is the target for commands. It usually follows the focused window or
pointer but is distinct from X input focus.

`State::clients_` is the registry for every managed window. `WindowManager` keeps const
views of this registry and the monitor graph. `Client::state` holds one of four
alternatives; `Client::kind()` derives its classification from that state:

- `Tiled`: present in exactly one `Workspace::windows` vector.
- `Floating`: independently positioned and absent from tiled membership.
- `Dock`: outside normal focus/layout, contributes a strut, and uses the Above
  stacking tier.
- `Desktop`: outside normal focus/layout and uses the Below stacking tier.

Docks and desktops are not workspace members: their monitor and workspace fields
carry no placement, and visibility policy treats them as always visible.

Popup-only window types are mapped directly and never enter `clients_`.

The important authorities are:

- `Client`: placement, classification, normal geometry, protocol state,
  urgency, and scratchpad membership.
- `Workspace::windows`: tiled membership and layout order.
- `Workspace::focused_window` and `focus_history`: remembered tiled focus.
- `Client::mru_order`: in-memory focus recency for tiled and floating clients;
  only floating order is persisted across an exec restart.
- `active_window_`: the focused managed window.
- `Monitor::fullscreen_owner`: the effective fullscreen owner for one monitor.

`State` owns the registry, monitors, workspace membership and focus memory, active
focus, explicit preferences, and scratchpad claims/pool. Readers receive const views.
Named operations update state and record completion obligations together. Registration
accepts an unowned candidate; restart accepts a decoded record rather than a writable
managed client. Topology replacement rebuilds the workspace graph inside the same owner.

`Client::preferences` stores optional mode, layer, and taskbar/pager choices. Unset
fields follow classification defaults. Modal and fullscreen state project the effective
layer without destroying the underlying preference. [X11.md](X11.md#classification)
defines precedence between defaults, application requests, user actions, and rules.

`Client::presentation` contains only backend acknowledgements: off-screen status, last
written geometry/border, urgency echo suppression, and sync counters. Layout, focus
eligibility, and drag validity use logical visibility, not this cache. Backend helpers
can mutate this subrecord without changing domain state. Completion writes visibility
and geometry acknowledgements during publication; hint handling also updates protocol
bookkeeping such as sync counters.

## Lifecycle and transitions

An operation is one dispatched X event, IPC command, signal reload, timeout and
coalesced topology pass, or startup scan. Mutations record `TransitionEffects`; only the
boundary calls `complete_transition()`. Completion has this order:

1. Refresh input-dependent workareas. Resolve affected fullscreen owners and
   visibility, validate drag participants, and repair focus.
2. Compute affected layouts and geometry targets. Resolve publication dependencies
   (focus changes require current-desktop/stacking publication; urgency requires
   the panel client-list notification), then freeze the effects record.
3. Publish visibility/geometry, ConfigureNotify replies, and maps; publish final
   focus and properties. Geometry IDs are deduplicated before writing.
   `_NET_WM_STATE` changes merge with one fresh property read per affected client,
   preserving atoms LWM does not own.
4. Reconcile stacking, perform at most one crossing-event barrier, and flush X.
5. Emit settled subscription events and check Debug invariants. Publishers cannot
   mutate managed records or schedule earlier work; Debug checks guard this boundary.

Choose effects by the dependency that changed:

| Mutation | Completion work |
| --- | --- |
| Placement, kind, iconic/sticky/fullscreen state | Reconcile affected monitors' ownership and visibility, then layout and geometry |
| Transient parent | Reconcile the client's monitor even without relocation; the parent determines fullscreen exemption |
| Split ratio | Recompute layout without a visibility scan or stacking reconciliation |
| Floating normal rectangle | Request client geometry without invalidating unrelated layouts |
| Focus intent | Resolve final focus, current desktop, stacking, and urgency before publication |
| Urgency | Publish client state and the panel client-list notification |

`request_*` helpers record output obligations; `publish_*` helpers write them.
`queue_event()` constructs JSON only for interested subscribers.

Dock registration, removal, and strut notifications request a workarea refresh. The
refresh reads current dock properties and, when a reservation exists, the root geometry
once. It projects root-relative reservations onto each monitor, publishes workareas, and
invalidates layout. Consecutive dock registrations during startup share a refresh;
adopting a normal client consumes pending workarea changes before placement and rules.
Hotplug likewise refreshes before relocating floating clients. Completion consumes any
remaining refresh before arranging monitors. There is no persistent dock-property cache,
and adoption retains its existing traversal order.

Empty effects require no reconciliation. The outer loop flushes direct protocol replies
after each bounded event batch. A batch consumes at most 64 X events or 2 ms between
events, counting coalesced motions. IPC, signals, and deadlines are serviced between
batches. This prevents drain-until-empty starvation; an individual handler can still
take longer than 2 ms. After a crossing barrier, only events already in XCB’s queue are
drained, so fresh socket input cannot extend it forever.

This is ordered completion, not rollback or an atomic X-server transaction. Helpers may
read X hints, refresh workareas needed for placement, and register protocol resources
while mutating state. Visibility, geometry, focus publication, stacking, and event
delivery remain owned by completion. Drag motion compression happens in outer dispatch
and stops at the first non-motion event. A drag handler never recursively dispatches
another event.

## Client registration and placement

`manage_client()` registers both tiled and floating clients for new MapRequests and
adoption of existing windows. It captures hints, establishes placement and workspace
membership, initializes protocol resources, and applies restart state or rules. Floating
placement remains a separate policy calculation. New clients are mapped at completion;
adopted clients are not remapped. Docks and desktops retain dedicated registration
because their layout and focus roles differ; popup-only types are mapped without
registration.

Metadata handlers update the relevant property through `State`, compare old and new rule
results, and pass changed actions to `apply_rule_result_to_window()`. Reload calls the
same action handler for each matching client. Pending scratchpad claims are checked
independently of rule-result changes. Precedence and replay semantics are specified in
[X11.md](X11.md#classification).

`unmanage_window()` removes all managed kinds: it writes `WM_STATE=WithdrawnState`,
removes authoritative membership and pending kill state, and requests visibility/focus
repair and EWMH list publication.

`State::relocate()` moves either normal client kind, updates desktop publication, and
invalidates both affected monitors. Callers choose whether floating geometry is
preserved or centered when crossing monitors, and choose subsequent focus. A tiled
reorder within one workspace preserves membership and focus history. `State::floating()`
records explicit mode intent and uses the same geometry and tile-slot policy as
classification defaults. `State::change_kind()` owns the underlying membership change
and publication obligations. Scratchpads use explicit state payloads for temporary
hidden representation and restoration: stashing a tile keeps its tiled rectangle while
preserving its prior floating rectangle separately. Restart restores saved intent
directly rather than applying a new mode choice.

Manage and unmanage use the same tile attachment/removal mechanics at the registry
boundary. Hotplug is a bulk owner operation: it rebuilds the workspace graph before
assigning clients their new monitor/workspace indices. Restart restores saved tile order
after adopting clients. Neither bulk path replays ordinary moves against partially
restored membership.

## Layout and geometry

`Layout` computes rectangles and resize boundaries without X calls. Master-stack is
evaluated as an iterative sequence of cuts: split 0 divides master from stack, and split
i divides stack slot i from the remaining slots. Monocle gives every window the content
rectangle and has no resize boundaries. Arrangement, drop selection, and resize hit
testing share the same calculation; they do not allocate an intermediate tree.

Split identities are stable slot indices. Restart serialization retains the version-3
depth/path encoding of the former right-leaning tree for existing splits. Beyond depth
32, the path field is saturated and the full index remains in the depth field. Non-chain
legacy addresses have no meaning in the supported layouts and are ignored on restore.

Floating clients keep one normal rectangle. Maximize projects selected axes onto the
workarea; fullscreen projects onto its target monitor rectangle. Neither projection
overwrites normal geometry. Tiled layout writes `tiled_geometry` as its target,
independently of the last rectangle sent to X. Until the first layout, this holds the
initial client rectangle, so reclassification on an inactive workspace preserves valid
geometry; that rectangle can still lie off its monitor (an off-monitor initial request
or a tile moved before it was arranged), so `State::change_kind` recovers tiled-to-floating
rectangles onto the monitor as described in [X11.md](X11.md). A floating-to-tiled transition seeds this rectangle from the normal floating rectangle
until layout runs. Ordinary mode changes restore prior floating geometry when
present, otherwise the tiled target; commands, rules and metadata classification share
that decision in `State`. They never recover normal geometry from fullscreen, maximized
or off-screen X presentation. Managed transient placement uses the parent's intended
presentation; server geometry reads are for initial capture and unmanaged parents.

`write_geometry()` owns WM-driven configure requests, sync notifications, and synthetic
ConfigureNotify events. Its output cache skips an already-applied rectangle and border,
coalescing duplicate requests without reordering layout or client traversal. Off-screen
hiding and conflicting ConfigureNotify events invalidate that cache. A client's
ConfigureRequest creates a separate reply obligation, so skipping a geometry write never
skips a required acknowledgement. The writer returns whether it sent geometry and its
synthetic acknowledgement; completion consumes the corresponding obligation in its
publication record. The WM holds no persistent mutable reference to pending effects;
Debug guards reject writable access during publication.

## Visibility

LWM maps a normal client once, then hides it by moving it to `OFF_SCREEN_X`. Three terms
must remain distinct:

- `iconic`: the client is logically minimized; ICCCM `WM_STATE` is
  `IconicState` and EWMH includes `_NET_WM_STATE_HIDDEN`.
- policy-visible: the client is non-iconic and is sticky or belongs to the
  current workspace on its monitor. Showing-desktop hides non-sticky clients.
- `hidden`: LWM has physically moved the client off-screen.

A policy-visible client can still be hidden when another window owns fullscreen. Normal
workspace changes therefore update `hidden`, not `WM_STATE`, and an incoming
`UnmapNotify` means client withdrawal rather than a workspace change.

Mutations invalidate affected monitors. During completion, `realize_visibility()`
selects fullscreen ownership and records visibility targets, then `arrange_monitor()`
computes layout targets. Geometry is written after all monitor arrangements. While an
operation is pending, eligibility checks use its preferred fullscreen owner and current
domain state, rather than stale physical visibility.

## Fullscreen and stacking

At most one non-iconic, policy-visible tiled or floating client owns fullscreen on a
monitor. Reconciliation honors an eligible explicitly preferred owner, otherwise keeps
the existing owner when valid, then falls back to the newest eligible managed client.
Other policy-visible tiled/floating clients on that monitor are suppressed and hidden. A
managed transient whose `transient_for` is the owner is exempt.

Fullscreen state may remain set on iconified or off-workspace clients; ownership is
effective only when they return to visible scope. Showing-desktop removes fullscreen
ownership while active. `_NET_WM_FULLSCREEN_MONITORS` changes geometry only and does not
create cross-monitor ownership.

`stacking::compute_order()` computes one bottom-to-top order directly from the client
registry and resolved monitor fullscreen owners. Its hidden prefix includes iconic,
off-workspace, and fullscreen-suppressed clients. The same complete order drives server
reconciliation and EWMH publication. Desktop clients use the Below tier and docks use
Above. Ordinary clients use Below, Normal, Above, or Fullscreen according to effective
state. Within a tier, floating clients are above tiled clients, active preference is
applied within each kind, then registration order and window ID break ties.

Visible floating transients must follow their visible managed parents, even across
tiers. Among windows whose parent constraint is satisfied, the lowest base-ranked window
is emitted next. Missing or hidden parents impose no constraint. Each transient has at
most one parent: an iterative parent walk identifies cycles and ignores the parent edge
of the lowest base-ranked member of each cycle (including self-links). Remaining
relationships are honored. These decisions are local to ordering; client hints are not
rewritten. Sorting and dependency ordering take O(n log n) time and O(n) temporary
space, without recursion or repeated repair passes. Windows without visible floating
transient hints take the direct sorted path without constructing dependency links.

`apply_stacking()` reconciles this result once during completion, before the
crossing-event drain and IPC publication. It compares the complete desired order with a
fresh root `QueryTree` reply. A longest increasing subsequence identifies windows
already in order; each remaining window needs one sibling move. This repairs external
restacks without a second cached authority for X order. Unrelated root children are not
themselves moved. Hidden clients participate so the published list and actual managed X
order agree, and fullscreen-suppressed clients cannot sit above their owner even while
off-screen.

## Focus

`focus_any_window()` records focus intent. It validates eligibility, deiconifies when
necessary, switches the target monitor's workspace when necessary, checks fullscreen
suppression, and updates active focus, memory, and recency. `commit_focus()` publishes
the final choice once during completion: it sends `WM_TAKE_FOCUS` when advertised, sets
X input focus and updates EWMH state and borders. Explicit same-window focus still
reasserts input focus, protocol notifications, and server stacking. Intermediate choices
within one operation do not produce focus events. Urgency is cleared during resolution,
before publication is frozen.

Docks, desktops, iconic clients, and fullscreen-suppressed clients are excluded from
fallback and cycling. Explicit activation can deiconify a client before the final
eligibility check. A managed client accepts focus when `WM_HINTS.input` is true or it
advertises `WM_TAKE_FOCUS`.

Fallback selection prefers the workspace's remembered focus, its bounded focus history,
reverse tiled order, sticky tiled clients on the monitor, then visible floating clients
by recency. `core/focus` reads the client registry directly for both fallback and
cycling, using one eligibility predicate and one resolved fullscreen owner per
selection. Fallback does not allocate candidate lists.

Cycling retains only window IDs in descending recency order; consecutive steps keep that
order while recording actual focus recency. Each step checks current client state, so
removed or newly ineligible windows are skipped, and existing windows that become
eligible can join the traversal. Sticky tiled and floating clients participate across
workspaces on their monitor. Ordinary activation (including same-window activation), a
changed monitor/workspace or active window, or a new client registration starts a fresh
traversal. The WM detects these changes from its existing recency/registration counters
and current focus context; cycling acknowledges its own recency update. Equal recency is
ordered by newest registration, then window ID.

Visibility-changing transitions finish with `flush_and_drain_crossing()` before
accepting subsequent pointer-driven focus. This round-trip discards stale crossing and
motion events that could otherwise overwrite the intended focus under
focus-follows-mouse.

## Pointer interactions

One optional `Drag` owns an acquired pointer grab, the initiating button, and
start/latest pointer coordinates. Its operation is either a window move/resize or a
tiled split resize. Failed acquisition leaves domain state unchanged; tiled-to-floating
conversion and exiting maximize happen only after acquisition. Window drags retain their
expected kind and placement. Completion cancels them when the client disappears, becomes
hidden/fullscreen, presents maximized (floating only; tiled clients retain maximize
flags without presenting them), changes kind, or is moved elsewhere by another operation.
Split drags retain the participants, workarea, workspace, and strategy defining their
split; layout invalidation checks that this context still exists. Config reload,
topology reconciliation, and restart cancel the interaction before replacing its
context.

`tiled_participants()` supplies the same ordered windows to arrangement, split
hit-testing, and drop targeting: visible members of the current workspace, followed by
visible sticky members of other workspaces. Fullscreen windows get fullscreen geometry
rather than layout slots. Drops translate a visible slot back to a membership insertion
point, excluding the dragged window. Hidden members and sticky guests are not reordered.
Dropping on the guest suffix appends to the destination's current workspace, before its
sticky guests in the resulting layout.

Window movement and edge-aware resize share pure, saturating geometry arithmetic.
Floating drags update the normal rectangle. Tiled movement is a temporary presentation
override, written through `write_geometry()` during completion; it does not change
membership until release. Cancellation removes that override without committing a
reorder. Completed floating changes and split adjustments remain in place. The
initiating button's release applies its final coordinates before ending the drag;
unrelated releases do not end it. All endings release the pointer and request the normal
crossing-event drain.

## Scratchpads

Named and generic scratchpads remain ordinary managed clients. Hidden scratchpads are
iconic and off-screen. Showing one rehosts it on the focused monitor's current workspace
and restores its tiled membership or floating geometry through the normal visibility and
focus funnels.

A named scratchpad enters launch-pending state only after successful process creation
and exec. Process exit is not used as a window-creation signal: a launcher may delegate
to another process. Pending launches suppress duplicate toggles until a matching window
arrives or the user explicitly cancels the pending launch through IPC. Cancellation
neither kills the program nor prevents a late matching window from being claimed.

## Configuration and reload

Configuration loading validates and prepares a candidate before changing the WM.
`Config` owns compiled rule and scratchpad regexes, the resolved keybinding map, and
typed mouse actions. Runtime matching and input handling use that data directly; there
is no second compiled configuration to synchronize. `KeybindManager` borrows its owning
WM's stable `Config` object. Resolving a key returns an action by value because
executing it may reload and replace the configuration that contained it.

Reload validates workspace-count compatibility before replacing `Config`. The state
owner reconciles scratchpad names with the accepted configuration. Surviving scratchpad
names retain both claimed windows and pending launches, even when their matchers change.
Removed names release their windows. The WM then regrabs input, updates EWMH workspace
metadata, and reapplies matching rules through the normal transition helpers. Invalid
configuration leaves the active configuration and runtime claims unchanged; this does
not promise rollback of X-server errors during application. The user-visible reload
limits are recorded in [README.md](README.md).

## Monitor topology

RandR screen, output, and CRTC notifications mark topology dirty. The event loop
coalesces each batch before reconciliation. Discovery supplies fresh output geometry;
surviving monitor names retain complete workspace state, including tiled order, focus
history, split ratios, and layout strategy. Removed outputs' clients move to monitor 0,
with surviving workspace order/focus taking precedence. Returning outputs start fresh
and do not reclaim relocated clients. All client kinds use the same old-to-new monitor
mapping. Workareas, floating geometry, visibility, and focus are then reconciled.
Fullscreen monitor-index hints are cleared because the indices may have changed.

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
decisions. Pipelined restart reads keep their existing request ordering; standard XCB
ICCCM/EWMH helpers remain in use.

## Restart

Graceful restart serializes global, workspace, client, ordering, ratio, and scratchpad
state into private X properties, execs the selected binary, restores that state during
the next scan, then removes the handoff properties. Autostart is skipped during this
handoff. These properties are private implementation details, not a compatibility API.
The X envelope (type, format, and completeness) is checked before decoding. Client
records validate before mutation; layout restore retains complete workspace records and
discards an incomplete tail. One invalid client record does not discard valid peers. The
codec retains the existing restore-rectangle wire slots for cross-binary handoff,
folding legacy maximize/fullscreen restore rectangles into the one normal rectangle on
decode. Legacy slots do not introduce additional live geometry state.

Client handoff retains a 28-word `_LWM_RESTART_CLIENT` record and a versioned
`_LWM_RESTART_PREFERENCES` extension. The extension distinguishes unset preferences from
explicit false and identifies its predecessor WM window. That window must advertise the
extension version; an intervening older binary cannot forward stale preferences even if
X resource IDs are reused. Older records preserve the saved mode; extended records
preserve which fields still follow defaults. The codecs and wire sizes are defined in
`core/restart.*`.

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

`ClientState` makes kind and kind-specific storage agree by construction; `LayerHint`
cannot represent both Above and Below. Other relationships require runtime checks. Every
completed transition must preserve:

- each tiled client appears in exactly one workspace and each workspace entry
  resolves to a tiled client;
- tiled/floating client monitor and workspace indices are valid;
- active and remembered focus never point at an iconic or absent client;
- the resolved fullscreen owner is eligible on its monitor, and published
  visibility agrees with that ownership;
- managed X stacking and `_NET_CLIENT_LIST_STACKING` come from the same order.

`invariants::validate()` checks registry identity, placement, tiled membership,
workspace focus, effective fullscreen ownership, and active focus without X calls. It
checks domain records and output bookkeeping, not the X server itself. Debug builds run
it on entry to the event loop and after each completed operation, including startup, X
events, IPC, signal reloads, and timeout/topology work. A violation logs the reason and
aborts at that boundary; Release builds omit these checks. X properties and observable
ordering require integration tests.
