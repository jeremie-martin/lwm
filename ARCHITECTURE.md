# Architecture

LWM separates a pure domain model (`State`) from an X11 shell (`WindowManager`).
Handlers change the model; one completion step derives and publishes its outputs.
The event loop is single-threaded. Only log delivery runs on a worker.

Configuration syntax belongs in [config.toml.example](config.toml.example).
[X11.md](X11.md) and [IPC.md](IPC.md) own externally observable protocol behavior;
[TESTING.md](TESTING.md) covers validation and the integration harness.

## Code map

Paths below are relative to `src/`.

| Area | Owner |
| --- | --- |
| Startup, options, exec/recovery | `app/main.cpp`, `app/cli.*` |
| Command-line IPC client | `app/lwmctl.cpp` |
| Strict TOML input and resolved configuration | `lwm/config/` |
| Client/fixture/workspace records and mutations | `lwm/core/types.hpp`, `state.*` |
| Derived layout, geometry, visibility, focus, stacking | `lwm/core/geometry.cpp`, `focus.*`, `stacking.*`, `floating.*`, `classification.*`; `lwm/layout/` |
| Desktop numbering and output rebinding | `lwm/core/policy.hpp` |
| Actions, command grammar, IPC transport, event encoding | `lwm/core/action.*`, `command.*`, `ipc*`, `events.*` |
| X resource ownership, properties, struts | `lwm/core/connection.*`, `ewmh.*`, `xproperty.hpp`, `workarea.*` |
| Process signals, logging, snapshot codec | `lwm/core/signals.*`, `log.*`, `restart.*` |
| Event loop, X reads, reload, spawning | `lwm/wm.cpp` |
| X admission and initial size observations | `lwm/wm_manage.cpp` |
| Metadata consequences, rule actions, named scratchpad operations | `lwm/core/client_policy.cpp` |
| X events and protocol requests | `lwm/wm_events.cpp` |
| Action execution, pointer interactions, scratchpads | `lwm/wm_actions.cpp`, `wm_drag.cpp`, `wm_scratchpad.cpp` |
| Completion and X publication | `lwm/wm_transition.cpp` |
| Queries and exec handoff | `lwm/wm_ipc.cpp`, `wm_restart.cpp` |

## Model and ownership

Each monitor has an output name, geometry, workarea struts, and a fixed-size vector
of workspaces. One workspace is current; the previous index supports toggling.
`focused_monitor` selects the target for commands and is distinct from X input focus.

The registry separates two kinds of managed window:

- `Client`: a normal window with a valid monitor/workspace placement and a
  `ClientMode` variant. `TiledMode` holds an optional remembered floating rectangle;
  `FloatingMode` holds its normal rectangle and optional tile-return slot.
- `Fixture`: a dock or desktop with no placement, layout participation, or focus.
  Popup-only types are mapped directly and never registered.

The following state is authoritative:

| Record | Meaning |
| --- | --- |
| Client placement, mode, requested flags, preferences | User/application/rule intent; effective classification is derived |
| `Workspace::windows` | Tiled membership and layout order; each tiled client occurs exactly once |
| `Workspace::preferred_tile` | Destination intent after relocation; actual tiled focus clears it |
| `Client::order` / `Fixture::order` | Registration order across both registries |
| `Client::mru_order` | Completed focus recency; zero means never focused |
| `State::fullscreen_claims_` | Fullscreen clients in interaction order, oldest to newest |
| `active_window` | Final selected managed window, or none |
| Named scratchpad slots and pool | The only scratchpad membership records |

`State` exposes const views and named mutations. Use `find()` at X event boundaries
where a window may be unmanaged or destroyed; use `require()` after ownership is
established. Mutations maintain membership and claims and advance a revision counter.
User-time bookkeeping is not published and does not advance it.

Visibility, fullscreen ownership, effective layer/skip values, and tiled rectangles
are derived. `Output` and `RootOutput` are different: they record what was last
published so completion can avoid redundant X requests. They must not drive domain
eligibility or geometry decisions.

## Lifecycle and transitions

An operation is one dispatched X event, IPC request, signal reload, timeout/topology
pass, or startup scan. Bindings and IPC commands share `Action` and `execute()`;
read-only queries bypass execution. Handlers call `State` and retain only obligations
that cannot be derived from it: ConfigureRequest replies, event facts, forwarded
restacks, workarea refreshes, and crossing-event drains.

`complete_transition()` performs ordered completion:

1. Return if neither the revision nor any presentation input/obligation changed.
2. Refresh workareas, validate the drag, repair focus, and complete final focus
   recency, user time, and urgency changes.
3. Freeze `State`. Derive fullscreen visibility and one projection per client,
   then apply any tiled drag preview.
4. Hide/configure/map clients and acknowledge ConfigureRequests. Commit explicit
   focus, publish client and fixture properties, root properties, and stacking.
   Withdraw removed windows.
5. Drain stale crossing events after visible movement when appropriate, flush X,
   and emit events. Thaw the model and check Debug invariants.

This orders effects; it is not rollback or an atomic X-server transaction. Handlers
may read X and register protocol resources before completion. Publication must not
mutate the frozen model.

A changed projection is compared with the previous output. When an external write
invalidates an owned field, forget that cached field. A ConfigureNotify mismatch
forgets geometry without forcing immediate completion, because it may be LWM's own
superseded request. A conflicting urgency hint forces reconciliation. Mark
`presentation_dirty_` for inputs outside `State`, such as appearance or a drag
preview. Explicit focus reasserts X focus, focused state, and stacking even when the
selected window is unchanged. Initial root publication writes empty lists too.

The event loop polls X, the signal pipe, and IPC. X batches stop after 64 events or
2 ms between handlers, including coalesced drag motion; individual handlers can take
longer. IPC and deadlines run between batches. Motion coalescing stops at the next
non-motion event. Crossing barriers drain only events already in XCB's queue and
save unrelated events for outer dispatch; handlers never recursively dispatch.

## Placement and geometry

`insert()`, `erase()`, `relocate()`, and `floating()` own membership and focus
consequences. Callers must not repeat those sequences. Relocation can preserve,
center, or translate a floating rectangle when changing monitors. An active client
follows a shown destination; otherwise the source chooses replacement focus. Tiled
reorders preserve membership history, while hidden destinations can remember a
preferred tile.

Tile-to-float conversion restores a remembered floating rectangle or derives the
normal tile slot; wholly off-workarea remembered rectangles are centered. Float-to-tile
conversion saves the rectangle and restores its tile slot only on the slot's original
output name and workspace. Visiting elsewhere does not change that identity; losing
the original output/workspace invalidates it permanently.

`State::project()` returns every client in registration order with an optional
rectangle: absent means hidden. `normal_geometry()` derives a tiled client's slot
even when minimized, off-workspace, or fullscreen. Fullscreen/maximize and drag
previews affect presentation, never the saved normal floating rectangle.

Layout arrangement, split hit-testing, and drop targeting share the same subdivision.
Master-stack is an iterative sequence of cuts: split 0 divides master from stack;
later splits divide stack slots from the remainder. Monocle has no resize boundaries.
Participants are eligible tiles from the current workspace, then sticky tiles from
other workspaces. Fullscreen clients do not consume layout slots.

`write_geometry()` owns WM-driven configure requests, sync messages, and synthetic
ConfigureNotify. Unchanged geometry can be skipped, but an outstanding
ConfigureRequest still needs its acknowledgement. Visible replies reuse the frozen
projection; hidden clients use their derived presentation geometry.

### Admission and topology

Admission owns classification, matching, application observations and X resource
setup once per window. Live maps register their candidate immediately; startup
passes observations to State once, then places registered clients by ID. There is
no second manage pass or conditional insertion of an already registered client.
A valid handoff establishes client identity and fixture role before current metadata
can choose fresh admission defaults; surviving clients receive full observations even
when their current type would admit a dock or popup. Fresh fixtures and popups have
no client placement.
Fresh clients use desktop hints or the focused monitor, apply initial state, and then
rules. Startup registers the complete scene and applies tiled rules before placing
floating clients. Cold startup applies each tiled rule at admission, preserving
fullscreen-claim priority relative to subsequent application claims. A bounded
parent walk places floating parents before children, including malformed cycles. Managed parents supply derived presentation geometry;
only unmanaged parents require X geometry reads. Initial placement and subsequent
size-hint updates share one implementation. Initial placement follows a managed
parent's workspace and centers without a position hint; later size-only updates
preserve the chosen origin.

Metadata operations belong to State: updating title, class, type, or transient parent
also resolves classification defaults, parent placement, changed rule actions, and
pending named scratchpad claims. Adapters supply the current configuration without
retaining another policy copy. For transient updates, the model derives a managed
parent's presentation after membership changes; the shell supplies only an active
drag preview override. No X-reading callback enters the model.

Metadata applies a rule only when its resulting actions change; losing a match leaves
previous actions in place. Pending scratchpad claims take precedence over changed
rule actions. Explicit reload reapplies every matching rule, including unchanged
actions, without claiming pending launches. Initial rule application uses the admitted
match; restart restores saved ownership and history without replaying placement.
Named scratchpad matching, claiming, presentation, and toggling are model operations;
a toggle can request a launch, which the shell performs before marking it pending.

Topology notifications are coalesced. Discovery reads new geometry and dock struts
before `State::replace_monitors()`. Output names identify survivors, whose complete
workspace graphs are transferred. Removed outputs' clients move to output 0 after
surviving tiles; returning outputs start fresh. Floating rectangles are clamped for
survivors and centered for displaced clients. Index-based fullscreen-monitor hints
are cleared. Restart rebinding shares these policies rather than replaying commands.

## Visibility, focus, and stacking

A client is in view when it is not iconic and is sticky or on its monitor's current
workspace; showing the desktop leaves only sticky clients in view. Outside
show-desktop mode, the newest fullscreen claim in view owns the monitor. Other
normal clients are suppressed except managed transient descendants of that owner.
`FullscreenVisibility` computes owners and descendant exemptions for one read pass;
reverse parent links and visited sets bound cyclic hints, with O(n) work per owner
and no ancestry construction when there is no owner. Intermediate ancestors may
be hidden or on another monitor; the descendant's own monitor selects its owner.

Keep three concepts distinct: `iconic` is explicit minimization, visibility is
model-derived, and `Output::hidden` means LWM moved the window off-screen. Normal
clients are mapped once; workspace hiding does not unmap them.

`fullscreen()` is an idempotent assignment, used by rules. `request_fullscreen()`
renews the claim even when already enabled, used for explicit interactions and
restoration from minimization. Entering fullscreen clears maximize. Focus eligibility
requires visibility, an input hint or `WM_TAKE_FOCUS`, and no show-desktop mode.

`State::focus()` deiconifies and selects placement, then falls back if the target
is suppressed. Removal, minimization, workspace changes, and show-desktop manage
focus within `State`. `complete_focus()` repairs eligibility and records only the
final choice; registration and mode changes alone are not focus. Pointer barriers
prevent stale crossing events from undoing the completed choice.

Fallback prefers the destination tile, then the most recent eligible tile in the
current workspace (reverse membership order breaks never-focused ties), then sticky
tiles in reverse workspace/membership order, then floating clients by recency.
Cycling retains IDs in descending recency order, with registration and ID tie-breaks,
and rechecks eligibility at each step. Activation, changed monitor/workspace or active
window, and registration clear the traversal; consecutive cycle steps retain it while recording actual focus.

`stacking::compute_order()` derives a bottom-to-top order for clients and fixtures.
Its tiers and transient constraints are specified in [X11.md](X11.md#desktops-and-root-properties).
An iterative parent walk cuts the lowest base-ranked edge in each cycle; a priority
queue then respects parent dependencies. Work is O(n log n), with O(n) temporary
space. Completion reconciles changed order, forwarded restacks, or explicit focus
against a fresh QueryTree. A longest increasing subsequence minimizes sibling moves.
Unchanged ordinary transitions do not query the tree.

## Pointer interactions and scratchpads

One optional `Drag` owns the pointer grab, initiating button, coordinates, and either
a window move/resize or a tiled split resize. Failed grabs leave the model unchanged.
Completion cancels a drag if its client eligibility or captured split context changes;
reload, topology reconciliation, and restart cancel before replacing context.
Floating drags change normal geometry; tiled movement is a preview until release.
Drops translate visible slots into membership indices without reordering hidden
members or sticky guests. Dropping on the guest suffix appends to the current
workspace. Cancellation discards the preview; all endings release the grab and
drain crossing events.

Named scratchpad slots are empty, launch-pending, or claimed. The generic pool is an
ordered list whose last entry is the recall target. Both contain ordinary clients;
explicit hiding uses iconic state. Recall relocates to the focused monitor's current
workspace. Named clients float at configured size; pooled clients keep their mode
and floating offset within the workarea.

Cycling recalls an invisible/remote pool target or focuses an inactive local one.
An active local target is hidden and rotated to the front before recalling the next;
a one-window pool toggles. Removing members preserves remaining order. Named launches
become pending only after successful spawn/exec, not until a process exits. A launcher
may delegate to another process. Claims and pending launches survive restart and reload
while their configured names survive; removing a name releases its window. User-facing
launch cancellation is specified in [IPC.md](IPC.md#commands).

## Configuration and events

A private reflect-cpp schema validates TOML structure over toml++; LWM resolves names,
bounds, commands, `Action`s, and compiled regexes into ordinary runtime values.
Workspace names resolve at load time; monitor names resolve against current outputs
when rules apply. Bindings' `action` strings and IPC requests use the same command
parser; bindings require an `Action`, rejecting queries and subscriptions.
Command declarations bind typed argument parsers directly to requests and derive CLI
argument counts, without an intermediate value variant or runtime extraction.
Configuration also validates workspace bounds, ratios, and scratchpad names before installation.
A binding has one `action` value: WM command text or a structured process command.
The schema models command forms as variants; named declarations can contain only
executables, while use sites can also contain references. One resolver lowers both
variants to argv and validates process arguments. Rule selectors each accept an
index or name, and layer is one of normal/above/below, so conflict reconciliation
is unnecessary.
Window types decode directly from the runtime enum instead of a second registry.
Shell text becomes `/bin/sh -c` argv, and named command references resolve during
loading. The command registry is local to the loader; runtime bindings, autostart,
and scratchpads retain only argument vectors. Event action labels describe executed
operations independently of their input spelling.

Reload validates the candidate and workspace-count compatibility before replacing
`Config`, reconciling scratchpad slots, regrabbing input, and reapplying matching
rules. Invalid input leaves the active configuration and claims unchanged; this is
not rollback of later X-server failures. Key bindings are grabbed at the root.
Execution copies a binding's action because reload can replace its owning config.
See the example's reload and rule sections for user-visible semantics.

Workspace events compare current indices by output name with the last completion.
Focus events describe the final explicit focus request. Completion emits these final
results first, then queued map/unmap facts, then action/reload outcomes in occurrence
order, and finally any state invalidation. It needs one fact buffer and no priority
sort or merged event buffer.

Event records define the subscription wire schema. Their C++ type names are the
public event names; changing a name is a protocol change. Native reflection derives
the filter-name list, while the event variant defines internal filter bits. Placement
constructors provide either both immutable indices or neither. Optional fields are
omitted and layout values retain their JSON types. `ipc::Server::emit()` accepts only
a typed event, checks subscribers, assigns the sequence, and serializes one complete
instance/sequence/event envelope over the record by reference. Event identity and filtering cannot contradict the payload type. Queued facts own captured metadata;
borrowed action/kind/source labels refer to static strings. Transport retains only
encoded bytes. IPC queries serialize derived views directly. A new subscription records the exposed snapshot as its baseline;
`state_change` is emitted only when that snapshot changes, with revision checks
avoiding unnecessary comparisons. [IPC.md](IPC.md#subscriptions) owns ordering and
consumer synchronization guarantees.

## Restart and process lifetime

`SignalPipe` owns SIGHUP/SIGCHLD handlers and a nonblocking, close-on-exec self-pipe.
Main creates it before logging and destroys it after logging shutdown. Failed-exec
reconstruction reuses both process-level owners. Unexpected failures in config,
construction, the event loop, or restart preparation exit nonzero with a phase
diagnostic; reconstruction is only for a prepared restart whose exec failed.

X resources normally die with the connection, including on construction failure.
Restart retains the predecessor's resources so closing the last connection cannot
reset an empty X server and erase the handoff. `_LWM_RESTART_OWNER` marks its internal
window and the root. After claiming root ownership, the successor validates the
marker and kills that retained X client before continuing initialization.

`State::snapshot()` records private intent in `_LWM_RESTART`: placement, modes,
preferences, urgency, focus ranks, workspace layouts/ratios/order/preferences,
scratchpad claims/pending launches/pool order, and fullscreen-monitor hints.
Client intents and fixture records carry their registration ranks directly; fixture
roles also survive. There is no separate registration-ID list.
Registration order spans clients and fixtures; fullscreen claim order is separate
from focus and server stacking. Derived rectangles are omitted. Requested flags
such as fullscreen, sticky, iconic, and maximize travel through `_NET_WM_STATE`.

Adoption proceeds in this order:

1. Resolve each surviving window's established registry identity before fresh
   classification. Observe clients, register fixtures with their saved roles, and
   refresh dock reservations before rebinding or placing clients. Only newcomers
   choose admission roles from current type hints.
2. For the normal client candidates, `State::restore_graph()`
   overlays saved `ClientIntent` values on surviving observations and installs the
   saved `MonitorState`/`Workspace` graph directly, including tiled order. Saved
   mode wins even when observed type/transient defaults now suggest another mode;
   restoration preserves representation instead of replaying metadata changes or
   rules. Observed metadata still drives layer/skip defaults, input eligibility and
   transient stacking. Subsequent metadata changes use the ordinary live policy.
3. Rebind through the same `replace_monitors()` implementation as live hotplug.
   Fewer configured workspaces fold into the last; additional workspaces keep
   defaults. Changed topology fits floating rectangles and clears fullscreen-monitor
   hints; unchanged restart topology preserves them. Filter vanished clients after
   merging, retaining the existing workspace-preference precedence.
4. Admit newcomers on the restored current workspace unless a concrete desktop hint
   places them elsewhere. Saved registration ranks remain reserved, including when
   fixtures were scanned first. Preserve surviving fullscreen claims and append new
   claims in observation order. Restore scratchpad claims, place new floating clients,
   restore focus, and let pending launches claim remaining matching clients.

Missing clients and removed scratchpad names are skipped. Autostart is suppressed
whenever a predecessor handed over, even if the snapshot is unusable.

The live persistent value types are also the snapshot schema; X observations,
RandR handles, dock reservations and publication caches are not serialized.
Native C++26 reflection and bundled yyjson encode
required fields (optional values use null), tagged modes, and checked integers.
LWM also rejects duplicate/extra fields. One persistent graph validator serves
both decoded snapshots and Debug model checks: placement, identities, tiled
membership, focus ranks, fullscreen claims and exclusive scratchpad ownership are
checked before reconciliation. Pool members must be distinct saved clients, disjoint
from named claims; fixture registration is insufficient. Active IDs must name saved
clients, and named slots require nonempty unique names. Valid clients that disappear
during handoff are filtered later; invalid saved ownership rejects the whole graph.
Named slots share one value type in memory and on the wire: null is launch-pending,
NONE is empty in memory (omitted from the handoff), and a client ID is claimed.
The CARDINAL32 envelope contains a format word, byte length, and zero-padded JSON;
length and padding must match exactly. Output names preserve opaque bytes. The
current format is 13 and follows the domain values directly, with no legacy field
aliases or conversion layer. Schema changes require a format bump. Incompatible
or malformed snapshots are rejected as a whole; windows are adopted afresh through
the normal startup path. There is no migration codec. Consequently, the first
restart from a format-12 binary loses private workspace and focus history;
subsequent same-format restarts preserve it.

## Logging

`core/log` owns Quill's standard journal/stderr sinks, one worker, and a preallocated
256 KiB bounded dropping producer queue. The WM thread submits; the worker formats
and delivers. `LWM_LOG_*` adds a null guard to Quill's level-gated macros. Quill owns
argument copying, formatting, control characters, rate limits, and overflow reporting.
Oversized records are rejected rather than truncated. The transit buffer is capped
at 256 events. Idle polling is 100 ms at INFO or above,
10 ms at DEBUG/TRACE. At `off`, no logger or worker starts.

Normal event handling never waits for delivery. Normal shutdown disables submissions
and joins the draining worker; stalled sink I/O can block it. Invariant failures abort
without draining. Exec does not flush or stop logging: success replaces all threads,
failure retains the logger, instance, and notification count. The worker alone blocks
SIGPIPE so application signal dispositions remain unchanged.

The backend callback retains a count and the latest 1 KiB of notification text under
a mutex used only for notifications/status queries. See [IPC.md](IPC.md#logging-status)
for interpretation. Startup options own logging policy; TOML reload does not change it.
Use fixed format strings and concise diagnostics. INFO reports lifecycle/configuration/
topology outcomes; DEBUG explains domain decisions; TRACE records bindings and submitted
geometry. Launch failures identify the executable and failing stage, never arguments
or shell contents. Shell command failures belong to the shell's inherited stderr.

Asynchronous X errors record code, opcode, resource, and sequence without extra X
round trips. Teardown-related BadWindow/BadDrawable errors are DEBUG, others WARN.
Each category and recurring RandR/reload warnings have a five-second rate limit, so
logs explain failures but are not an inventory or authoritative state database.

## Invariants

Types prevent mode/storage disagreement, fixture placement, and simultaneous
Above/Below preferences. `invariants::validate()` checks the remaining relationships:
valid placement; exact tiled membership; disjoint client/fixture IDs; unique bounded
registration and nonzero focus ranks; fullscreen claims consistent with fullscreen
and excluding maximize; valid non-iconic tile preferences; exclusive live scratchpad
membership; and a managed, eligible active window.

Debug builds validate the persistent snapshot graph, then add live registry/order
bounds, iconic preferences, fullscreen flags and focus eligibility checks. They
check on event-loop entry and after completed operations, logging and aborting on
violation. Snapshot construction is confined to these Debug checks and actual
handoff; Release publication does not materialize validation snapshots. Derived
visibility and ownership need no synchronized copies. Model invariants do not establish X delivery or ordering;
those require integration tests.
