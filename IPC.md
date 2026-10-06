# IPC

This is the contract for scripts and panels that drive LWM. For interactive use,
`lwmctl --help` and command-specific help list commands without connecting.
[README.md](README.md) covers installation and startup.

```sh
lwmctl window fullscreen                         # actions are silent on success
lwmctl state | jq .windows.focused               # queries print one value
lwmctl watch | jq --unbuffered -r '.windows.focused'   # the state now and on change
```

## Transport

IPC runs over the X display named by `DISPLAY`; there is no separate socket. Anything
that can open the display can command LWM, which matches what such a client can
already do through EWMH and XTEST.

The running LWM owns the ICCCM `WM_S0` selection. A command is UTF-8 text in the
`_LWM_COMMAND` property of a window the caller owns, announced to the selection owner
window by a `_LWM_COMMAND` ClientMessage (format 32, `data.l[0]` = that window, sent
with an empty event mask). LWM reads and deletes the property, executes the command,
completes the operation, and then writes one UTF-8 reply into `_LWM_REPLY` on the same
window:

- `ok`
- `ok VALUE`
- `error MESSAGE`

The reply follows the operation's effects on LWM's own connection, so the X server has
applied them before the caller can observe the reply. Commands execute in the order
LWM receives their messages, interleaved with other X events. Requests must be shorter
than 4096 bytes; anything else gets `error request too large` or `error request must be
UTF-8 text`.

`lwmctl` performs this exchange from a private window. Actions reply `ok` and print
nothing; queries reply `ok VALUE` and print `VALUE`. Errors print to stderr with exit
status 1: `lwm is not running` when no WM owns the screen, `lwm exited before replying`
when the owner window disappears while waiting, and a timeout after `--timeout MS`
(1–600000, default 2000). A timeout or exit leaves the outcome unknown, because LWM
may have executed the command; do not retry toggles blindly.

`lwmctl --help` and command-specific help (for example `lwmctl workspace --help`) print
to stdout without connecting; an unknown help group returns status 1. Usage errors
print to stderr and return 1 without connecting. Use `--` before arguments that
resemble options, and shell-quote names or paths containing spaces.

## Commands

Queries read current state. Mutating commands are also accepted verbatim in a
binding's `action` string, for example `action = "window fullscreen"`; both use one
parser and executor. Queries and `watch` cannot be bound. Bindings use a
structured `action` table to launch processes; IPC callers can launch processes
themselves. Commands concerning the active window return
`error no active window` when none is selected. Relative monitor/workspace commands
target the focused monitor; indices are zero-based.

Consecutive `focus next` / `focus prev` commands retain their starting recent-use
order, including sticky windows, and skip ineligible clients. Activation, a changed
monitor/workspace or active window, or a new registration starts a fresh traversal.
Cycling fails when no window is eligible, including while showing the desktop.

| Command | Result |
| --- | --- |
| `ping` | `pong` |
| `state` | consistent combined state snapshot as JSON |
| `watch` | the state snapshot now and after every change, one JSON line each |
| `version` | LWM version |
| `log status` | logging configuration and backend notifications as JSON |
| `reload-config` | reload the configured file |
| `restart` | exec-restart the current binary |
| `exec PATH` | restart with another binary |
| `layout set master-stack` | set the current workspace layout |
| `layout set monocle` | set the current workspace layout |
| `ratio set VALUE` | set the current workspace's root split ratio |
| `ratio reset` | clear all split ratios on the current workspace |
| `ratio adjust DELTA` | adjust the current workspace's root split ratio |
| `notify-attention window=<xid>` | mark a managed window urgent unless it is active |
| `workspace switch N` | switch to zero-based workspace `N` |
| `workspace next` / `workspace prev` | switch with wraparound |
| `workspace toggle` | switch back to the previous workspace |
| `workspace list` | workspace state as JSON |
| `monitor focus left` / `monitor focus right` | focus the adjacent monitor |
| `focus window=<xid>` | focus a window, switching workspace if needed |
| `focus next` / `focus prev` | cycle eligible windows in recent-use order |
| `window list` | tiled and floating client state as JSON |
| `window close` | close the active window |
| `window fullscreen` / `window float` | toggle fullscreen or floating on the active window |
| `window swap next` / `window swap prev` | swap the active tile with its neighbor |
| `window to-workspace N` | move the active window to workspace `N` of its monitor |
| `window to-monitor left` / `window to-monitor right` | move the active window to the adjacent monitor |
| `scratchpad stash` | move the active window to the generic pool |
| `scratchpad cycle` | cycle the generic pool |
| `scratchpad toggle NAME` | show, hide, or launch a named scratchpad |
| `scratchpad cancel-launch NAME` | clear pending launch state without terminating a process |
| `scratchpad list` | named and generic scratchpad state as JSON |

Ratio values must be finite numbers with no trailing characters. `ratio set` rejects
values outside `[min_ratio, 1 - min_ratio]` from the active configuration; `ratio
adjust` clamps to that range.

For named scratchpads, a definite exec failure is logged and leaves the slot
retryable. Successful exec does not guarantee a matching window: `scratchpad cancel-launch NAME`
clears a pending launch so the user can retry. It is a no-op for an empty or already
claimed slot and rejects unknown names. It does not terminate a process; a late matching
window can still be claimed. Both named scratchpad commands reject unknown names.

## Logging status

`lwmctl log status` is read-only. It returns:

```json
{"target":"journal","level":"info","instance":"1336-43855073878725","active":true,"backend_notifications":0,"last_backend_notification":""}
```

`active` means a logger is enabled, not that output has been delivered. It is false at
level `off` and after shutdown. Initialization failures are reported by the CLI before
the WM starts.

`backend_notifications` counts Quill notifications, including overflow summaries,
formatting errors, and reported sink errors. `last_backend_notification` retains the
first 1 KiB of the latest notification. Notifications are asynchronous and are not an exact count of dropped messages:
overflow is summarized, a stalled worker cannot report until it resumes, and libsystemd
accepts an absent journal silently. Successful sends do not confirm durable storage.

The count and logging `instance` survive failed exec and reset on successful exec; the
logger outlives the WM reconstructed after a failed exec.

## JSON results

`workspace list` returns:

```json
{
  "focused_monitor": 0,
  "monitors": [{
    "index": 0,
    "name": "HDMI-0",
    "current_workspace": 0,
    "workspaces": [{
      "index": 0,
      "name": "1",
      "current": true,
      "window_count": 2,
      "layout": "master-stack",
      "ratio": 0.5
    }]
  }]
}
```

`window list` excludes dock and desktop clients and returns:

```json
{
  "focused": 123456,
  "windows": [{
    "id": 123456,
    "monitor": 0,
    "workspace": 0,
    "kind": "tiled",
    "class": "XTerm",
    "instance": "xterm",
    "title": "shell",
    "focused": true,
    "fullscreen": false,
    "urgent": false,
    "sticky": false,
    "iconic": false
  }]
}
```

`workspace list.window_count` counts tiled workspace membership; floating clients appear
only in `window list`. `ratio` is the workspace's root split ratio.

`scratchpad list` returns:

```json
{
  "named": [{"name": "terminal", "window": 123456, "pending": false}],
  "pool": [234567]
}
```

A named entry uses `window: 0` when it has not claimed a window. The pool array is
ordered by recall rotation, with its current target last; see
[Scratchpads](ARCHITECTURE.md#pointer-interactions-and-scratchpads) for cycle behavior.

All window values are X11 window IDs. Monitor and workspace indices are zero-based
runtime indices; none of these identifiers are persistent.

## Watching state

`state` returns one snapshot containing `workspaces`, `windows`, and `scratchpads`, with
exactly the shapes of the corresponding list commands above.

`lwmctl watch` prints the same snapshot as one JSON line, then a new line whenever it
changes, and runs until interrupted. It is the interface for panels and scripts:

```sh
lwmctl watch | jq --unbuffered -r '.windows.focused'
```

LWM publishes the snapshot as the UTF-8 `_LWM_STATE` property of its `WM_S0` owner
window after each operation completes. `watch` selects property changes before reading,
so it cannot miss a change, and it prints only a value that differs from the previous
line. Changes within one operation appear as one state; consecutive changes may
coalesce into the latest state, so a watcher sees current state, not a history.
Read-only commands and no-op actions print nothing. The state covers only the fields
above, not window geometry: a pointer drag publishes its outcome when it ends.

The property dies with the WM's X connection. `watch` follows exec restart and failed-exec
recovery to the successor, which announces itself with the ICCCM `MANAGER` message, and
waits while no WM owns the screen. It fails only if no WM runs when it starts or the X
connection closes. A closed stdout pipe ends it normally.
