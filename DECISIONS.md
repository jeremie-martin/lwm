# Decisions

Significant design and product choices, with their tradeoffs and reasons. Routine
implementation changes belong in git history, not here.

## Drop `_NET_WM_SYNC_REQUEST` (2026-10-06)

LWM sent sync requests but never waited for the counter, so clients repainted exactly
as without them. Motion coalescing already limits drag configures (5 per 200 motion
events in `transition_counts.py`). Revisit if resizing heavy applications visibly lags:
implement drag-only sync that awaits the counter through an XSync alarm, with a
timeout, rather than restoring the fire-and-forget form.

## Replace event subscriptions with a watched state (2026-10-06)

`subscribe` streamed eight event types with filters, sequence numbers and a
subscribe-then-query recovery recipe. `lwmctl watch` streams the complete state instead,
published as `_LWM_STATE` through the same property cache as EWMH state, so it adds
no change detection of its own. Actions reply silently; their outcome is the state.
Lost: occurrences with no state equivalent (`key_action`, binding reload
outcomes, map/unmap of popups and fixtures, same-window focus). Revisit only for a
concrete consumer; publish such a fact as state rather than restoring an event stream.

## Carry IPC over X instead of a Unix socket (2026-10-06)

Commands travel as a property on the caller's window announced by a ClientMessage
to the `WM_S0` owner, and the reply is a property written after completion, as in
herbstluftwm. This deletes the non-blocking socket server (deadlines, connection
caps, partial writes, backpressure), socket discovery (`--socket`, `LWM_SOCKET`,
`_LWM_IPC_SOCKET`) and lwmctl's socket client; the X server already provides
buffering, ordering and client lifetimes, and the reply now follows every effect.
Lost: IPC without X access to the display. Considered and rejected: keeping the
socket for commands plus a snapshot stream (about 300 lines more, same user
surface). An otherwise empty server must not reset during the handoff; see
"Hand the X connection across exec".

## Keep the exec-restart snapshot (2026-10-06)

Storing each client's private intent as a property on its window, with workspace
state on the root, would make every start a cold adoption and survive crashes. Four
independent reviews with code access found that it relocates the state rather than
removing mechanism: most validation is relational (unique ranks, tiled membership
agreeing with placement, disjoint scratchpad ownership), so splitting the record
replaces whole-record rejection with repair rules for contradictory records written
by a non-transactional completion. Restoration, topology rebinding and the exec
handoff remain either way; estimates ranged from -100 lines (only with lenient decoding
and restores that may blend two operations) to +250. It would
add crash loops (today the snapshot is deleted as it is read), stale state after
another WM, and self-declared fixtures. Its main benefit, crash survival, is void
where LWM is the session process, as on both maintainer hosts. Revisit only if
something respawns LWM inside a surviving X server, and then first publish the
existing snapshot continuously through the property cache, which costs about no lines.

## Keep a declarative configuration file, and make it the whole configuration (2026-10-06)

A shell script of `lwmctl bind/rule/set` commands was rejected by four independent
reviews with code access. It moves the loader into new command grammars and an argv
quoting layer; keeping whole-file rejection needs staged transactions across separate
`lwmctl` calls, i.e. per-caller state the X-carried IPC deliberately lacks; the
workspace count is fixed before a script could run; clearing and re-running on reload
releases scratchpad claims and reapplies rules repeatedly; and readiness, restart and
error reporting all get harder. Host differences stay in separate files or a generator.

Instead the TOML file became the whole configuration. The example file is embedded as
the built-in defaults, deleting the hand-written default bindings, commands and mouse
bindings and the rules for when a group replaced a built-in one. Bindings are a table
mapping a combo to command text or an argv list, deleting `[commands]`, `ref` and
`shell` (`["sh", "-c", "..."]` is an argv). Workspace names are the count, deleting the
separate count and its reconciliation. `[[workspace_keys]]` groups carry both switch
and move modifiers; mouse bindings are a table, and duplicates are rejected. `[autostart]`
was removed: the session script (`~/.xinitrc` or the display manager's) already owns
process lifetimes, readiness and cleanup, and LWM-run autostart duplicated it.

## Close politely, and close twice to kill (2026-10-06)

Closing sends `WM_DELETE_WINDOW`, and closing a still-open window again kills it; a
client without `WM_DELETE_WINDOW` is killed at once. A fixed kill timer for every client
destroyed windows that were merely showing a confirmation dialog. A ping-based timer
that killed only clients failing to answer `_NET_WM_PING` within 5 seconds replaced it,
then was dropped too: it was the event loop's only deadline, and the second close
already recovers a hung client. Revisit only if hung clients prove common.

## Repeat every binding alike (2026-10-06)

Toggle actions were protected from key auto-repeat by tracking releases. No binding is
special: holding a key repeats whatever it is bound to, which the user controls with
the X keyboard repeat settings.

## Drop show desktop (2026-10-06)

`_NET_SHOWING_DESKTOP` was a global mode that every visibility, focus, drag and
admission decision had to consult. A tiling WM's empty workspace already shows the
desktop. Revisit only for a concrete workflow, as a per-workspace action rather than
a mode.

## Ignore application minimize requests (2026-10-06)

A tiling layout has no place for iconified windows and LWM has no taskbar to restore
them from, so a client that minimized itself simply disappeared. `WM_CHANGE_STATE`,
`_NET_WM_ACTION_MINIMIZE` is not advertised; only scratchpads make windows iconic.
`_NET_WM_ACTION_MINIMIZE` is not advertised; LWM hides windows only through scratchpads.

## Restore hiding only from private intent (2026-10-07)

Once application minimize was removed, accepting `HIDDEN` during cold adoption was
another route for applications or an unrelated predecessor to make windows vanish.
Hiding now belongs to `ClientIntent` and the versioned restart snapshot. Only an
accepted snapshot restores it; fresh adoption and rejected-snapshot fallback show
windows according to their workspace/fullscreen eligibility. There is no property
fallback for hiding, and no migration of older snapshots. Restoration uses the
same scratchpad reconciliation as reload, so removing a named slot releases its
window and its hiding on both paths.

## Give property publication one owner (2026-10-07)

EWMH merging used a separate write path while completion cached its desired state
before the write could be refused. Completion now owns reads, size validation,
submission and one property cache; EWMH only converts atoms. The cache stores owned
state values for shared properties, so foreign atoms neither consume cache memory
nor drive model decisions. A refused write remains pending by comparison with the
model; a later external property change retries it without a retry flag or timer.

## Drop SIGHUP reload (2026-10-06)

`lwmctl config reload` and bindings already reload; the signal added a self-pipe, a
second event source in the loop and a third reload origin. The X connection is now
the only event source.

## Drop `log status` (2026-10-06)

The query counted Quill backend notifications for a CLI nobody read, at the cost of
a shared counter, a mutex and a logging instance identity. Backend errors are now
dropped, since reporting them could block on the stalled destination; revisit with a
monitoring consumer.

## Require RandR (2026-10-06)

Every X server LWM targets has RandR; running without the extension was an untested
second topology path. Startup now fails with a clear error without it. Topology still
comes from RandR alone: a report with no usable output yields one root-sized output.

## Drop rule `apply.scratchpad` (2026-10-06)

A rule could name a scratchpad to claim matching windows, beside the scratchpad's own
required `match`. Two routes to one claim; the scratchpad's matcher is the one kept.
Lost: claiming by window type or transient state, which only rules match.

## Make rule geometry relative and partial (2026-10-06)

Rule `geometry` was absolute root coordinates with 0/0/800/600 filled in for omitted
fields, and `center` separately discarded the position. Now x and y are relative to
the target monitor's workarea and given together, omitting them centres the window,
and omitted sizes keep the window's own. `center` is gone: `geometry = { }` centres.

## One command vocabulary (2026-10-06)

Commands name their object first and step with `next`/`prev` everywhere:
`window focus next|prev|ID`, `window attention ID`, `monitor focus next|prev`,
`window to-monitor next|prev`, `config reload`; window IDs lost the `window=` prefix.
Mouse bindings take `move`, `resize`, or anything a key binding takes, so
`toggle_float` became `"window float"`. Rule types are the lowercase names of the six
types that become clients. `ping` is gone (`version` answers as well) and so is
`workspace list.window_count`, which counted only tiles; `window list` has the windows.

## Keep scalar defaults in `Config` initializers (2026-10-06)

The example file restated every default and a test kept the two in agreement. Making
the example the only home would leave `Config{}` zeroed for the model and tests and
need a merge of omitted keys; instead the example shows defaults commented out and
binds keys, and the initializers are the one home.

## Keep scratchpads as slots and a pool (2026-10-06)

A from-scratch study (gpt-6.1-sol, checked against the code) found that rule-assigned
tags with one toggle command would be roughly line-neutral: client-owned membership,
pool ranks independent of focus, pending launches and singleton occupancy would all
move rather than disappear, and rules stop at the first match where named matching
skips occupied slots. Only dropping launch-on-demand and dedicated recall sizing would
save about 150 lines. Revisit if prestarted recall groups are preferred to dedicated
launch-on-demand pads.

## Keep WM_HINTS urgency mirroring (2026-10-06)

Writing urgency into the application-owned `WM_HINTS`, detecting its echo and
rewriting `_NET_CLIENT_LIST` exist so that polybar's `xworkspaces` urgent label works.
The cleaner design publishes urgency only through `_NET_WM_STATE_DEMANDS_ATTENTION`
and IPC, with the bar fed by `lwmctl watch`. Revisit when the bar no longer
depends on `WM_HINTS`.

## Keep remembered maximize on tiled clients (2026-10-06)

Tiled clients retain and publish maximize flags that only floating presentation
honors. The window is told it is maximized when it is not; the alternative is to stop
advertising and publishing maximize while tiled. Undecided; revisit with a concrete
application that misbehaves.

## Keep minimal-move restacking (2026-10-06)

Restacking everything on each change is simpler but exceeds the X request budgets
enforced by `transition_counts.py`.

## Keep these behaviours (2026-10-06)

Removing each would save 25–40 lines, but it would remove behaviour users rely on:

- the `workspace list`, `window list` and `scratchpad list` queries alongside `state`
- unmodified gap-click split resizing, with double-click and Ctrl-click reset
- `_NET_WM_FULLSCREEN_MONITORS`

## Reject nonzero X screens explicitly (2026-10-07)

LWM manages one X screen with RandR outputs; its ownership selection and EWMH setup
use screen 0. A shared connection boundary rejects nonzero DISPLAY screen selectors
for both WM and CLI before issuing screen requests. Silently addressing screen 0 was
incorrect. Supporting additional legacy X screens was rejected because RandR outputs
are the existing monitor contract; it would broaden ownership and IPC contracts
without simplifying them. This restriction does not limit the number of RandR outputs.

## Hide clients right of the root (2026-10-21)

Managed clients stay mapped. Hiding moves a client's last submitted rectangle to x
32767, the largest X coordinate. Windows extend rightwards, so no frame of any size
reaches back into the root; 16-bit coordinates make 32767 pixels the practical root
width. Toolkits never see a resize, and windows are not unmapped, so applications
are not told to stop drawing. State owns visibility; completion derives the hidden
rectangle, and synthetic notifications report the rectangle the client would have if
shown. On Xvfb, a switch that moves 200 windows costs about 6% more than unmapping
them.

Rejected, in the order they were tried:

- A fixed negative x: a wide client extends back into the root.
- A borderless 1×1 rectangle (October 7): GTK 4 answers the resize with a minimum-size
  request that becomes the floating geometry. A GTK probe returned an 800×600 window as
  122×38.
- Unmapping (October 8 to 21): LWM's own unmaps had to be told apart from withdrawals
  by request sequence, and hidden clients withdrew only by synthetic notification,
  which raced with showing. Cold startup had to adopt unmapped `WM_STATE` windows, a
  client reparented while hidden had to be detected separately, and an application's
  `MapRequest` could reveal a hidden client. Showing remapped the window, so content
  could be redrawn.
- Moving left by the frame's own width, as dwm does: 16-bit coordinates cannot hide a
  frame wider than 32768 pixels.

Restart still moves hidden clients onto the screen before exec, because the exec
target might not be LWM. They flash until the successor hides them.

## Preserve direct focus with usable protocol timestamps (2026-10-08)

Directly focusing clients that also support `WM_TAKE_FOCUS` is deliberate: March 26
commit `d78347b` repaired keyboard input stranded by slow or buggy clients. Keep that
behavior rather than changing focus policy during cleanup. A separate defect was
sending timestamp zero at startup/restart, and pairing cached protocol time with a
newer direct focus time. A responsive child-focusing client reproduced both missing
and stale protocol times. Focus of such a client now observes fresh X server time and
uses it for both requests, so the client's response is accepted. A request's own
timestamp is not reused: X ignores focus older than its last focus change, so an
accepted activation could otherwise be published without moving keyboard focus. The observation
rewrites the supporting window's unchanged name and keeps other events in the
deferred queue, at the
cost of one synchronous round trip only for `WM_TAKE_FOCUS` clients.

Using the triggering click or key time for X focus was reconsidered on 2026-10-21 and
rejected. LWM sets other focus with `CurrentTime`, and LWM does not track FocusIn, so X
would silently refuse an older event time and leave keyboard focus elsewhere while
State and `_NET_ACTIVE_WINDOW` name the new window. Clicks gain nothing: the replayed
click and LWM's focus leave in the same flush, and a `WM_TAKE_FOCUS` client answers
after LWM's direct focus.

## Bound shared-state output, not raw input (2026-10-18)

Rejecting `_NET_WM_STATE` from its raw size before merging was rejected: an oversized
input can contain owned duplicate atoms that merging removes, leaving a valid output.
Keep the merged-output limit. Reducing repeated oversized reads without changing that
behavior would require chunk scanning and counting retained foreign atoms.

## Keep matched-rule actions on clients (2026-10-18)

Metadata updates compare new matching actions with the previous match, applying only
changed actions and leaving earlier effects when a match is lost. Recomputing both
sides around each metadata mutation would remove the retained action copy but double
regex matching on title/class updates. Keep the cache unless measurements show that
removing it improves the tradeoff; reload deliberately refreshes and reapplies it.

## Read user time on demand (2026-10-19)

`_NET_WM_USER_TIME` only arbitrates application activation against the active client,
so LWM reads that client's value, through its user-time window, only when `State`
says an application's request competes with it. Eager tracking subscribed to foreign windows, including the root, and cost a
synchronous read for every keystroke in toolkits that update the property per input.
The comparison uses the newer of the observed value and LWM's recorded focus time.

## Hand the X connection across exec (2026-10-19)

The server resets when its last live client disconnects, and retained resources do
not count. Restart previously combined RetainPermanent, a self-identifying owner
window the successor killed, and a wait of up to 250 ms for the IPC requester to
disconnect. Keeping the predecessor's connection open through exec gives the same
guarantee directly: a duplicate of its descriptor survives exec, the connection is
never shut down, and the successor closes it after claiming the screen. A failed exec
recovers the same way, with the old WM alive until the recovery owns the screen.
Exec into a binary that does not know `LWM_RESTART_FD` keeps the old connection until
that process exits, so the predecessor first drops every grab and event selection;
the variable names its process ID, which exec keeps, so copies inherited elsewhere
are ignored. Handing over a fresh idle connection instead was rejected: the successor
would reuse the predecessor's client slot and window IDs, which tests and watchers use
to tell WM lifetimes apart. The `restart_resources` CTest probe guards the
empty-server case.

