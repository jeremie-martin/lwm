# Decisions

Settled design and product choices, with the reason and what would reopen them. Record
removed features and rejected proposals here; [ARCHITECTURE.md](ARCHITECTURE.md)
describes only the current design. Keep each entry to a few lines.

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
surface). A restart waits up to 250 ms for its requester to disconnect, or an
otherwise empty server could reset during the handoff.

## Keep the exec-restart snapshot (2026-10-06)

Storing each client's private intent as a property on its window, with workspace
state on the root, would make every start a cold adoption and survive crashes. Four
independent reviews with code access found that it relocates the state rather than
removing mechanism: most validation is relational (unique ranks, tiled membership
agreeing with placement, disjoint scratchpad ownership), so splitting the record
replaces whole-record rejection with repair rules for contradictory records written
by a non-transactional completion. Restoration, topology rebinding and the
retained-predecessor handoff remain either way; estimates ranged from -100 lines (only
with lenient decoding and restores that may blend two operations) to +250. It would
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

Closing sends `WM_DELETE_WINDOW`; only a client that also answers `_NET_WM_PING` and
then fails to answer within 5 seconds is killed, and closing a still-open window again
kills it. A fixed kill timer for every client destroyed windows that were merely showing
a confirmation dialog. A client without `WM_DELETE_WINDOW` is killed at once.

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
`_NET_WM_STATE_HIDDEN` requests and iconic `WM_HINTS.initial_state` are ignored and
`_NET_WM_ACTION_MINIMIZE` is not advertised; LWM hides windows only through scratchpads.

## Drop SIGHUP reload (2026-10-06)

`lwmctl reload-config` and bindings already reload; the signal added a self-pipe, a
second event source in the loop and a third reload origin. The X connection is now
the only event source.

## Drop `log status` (2026-10-06)

The query counted Quill backend notifications for a CLI nobody read, at the cost of
a shared counter, a mutex and a logging instance identity. Backend errors are now
dropped, since reporting them could block on the stalled destination; revisit with a
monitoring consumer.

## Require RandR (2026-10-06)

Every X server LWM targets has RandR; the fallback to a single root-sized monitor was an
untested second topology path. Startup now fails with a clear error without it.

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

## Rejected simplifications (2026-10-06)

- A shared properties base for `WindowObservation` and `Client` breaks
  designated-initializer construction in tests.
- Modeling `WindowRole` as `Fixture::Role` weakens the type.
- Dropping `drag_to`'s client re-check lets completion skip settling.
- Dropping `WindowManager::fullscreen_owners_` loses the owner-change log line that
  integration tests assert.
- Removing the defaulted `operator==` on actions removes syntax, not knowledge.
- Giving runtime `Config` reflect-cpp types would remove the duplicated input schema
  but leak the parsing library past the input boundary.

## Open opportunities (2026-10-06)

Candidates for subsystem-level simplification, not yet decided. Move an entry to a
decision above once it is adopted or rejected.

How these were found: two exhaustive line-level reviews found only about 70 lines of
genuine mechanism removal, and concluded the code was near its structural minimum.
Asking instead whether the IPC subsystem needed to exist in its current form led to
the X-carried IPC and watched state above: about 770 fewer production lines, a
simpler `lwmctl` (silent actions, `state`, `watch` that follows restarts), and replies
that follow their effects. The process that got there:

1. State the subsystem's purpose from the user's side, ignoring its implementation.
2. Have independent agents with code access design it from scratch and attack the
   leading proposal; weigh their disagreements against the code, not their claims.
3. Measure what the proposal moves onto hot paths before adopting it (here, state
   serialization: about 13 µs per change at 50 windows, skipped during drags).
4. Implement in commits that keep every suite green, then have an independent review
   look for defects (it found a real one: oversized state could close the connection).

The same question applied to the remaining subsystems:

- **Scratchpads as tagged windows.** Named slots, the pool, pending launches and
  claims form a separate membership system beside rules. A rule-assigned tag and one
  generic toggle-by-tag command might cover both kinds. Less certain than the above.
- **Logging.** Asynchronous Quill logging exists so that a slow log reader cannot
  stall the WM. Probably essential, but unquestioned.

