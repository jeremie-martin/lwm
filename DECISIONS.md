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
Lost: occurrences with no state equivalent (`key_action`, SIGHUP or binding reload
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
- Dropping `RootOutput::fullscreen_owners` loses the owner-change log line that
  integration tests assert.
- Removing the defaulted `operator==` on actions removes syntax, not knowledge.
- Giving runtime `Config` reflect-cpp types would remove the duplicated input schema
  but leak the parsing library past the input boundary.
