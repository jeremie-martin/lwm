# IPC

LWM exposes a local Unix-domain socket. `lwmctl` is the supported interactive client;
this document defines the raw protocol for integrations.

## Discovery and transport

The default socket is:

```text
$XDG_RUNTIME_DIR/lwm/ipc-<display>.sock
```

When `XDG_RUNTIME_DIR` is unset, LWM uses `/tmp/lwm-<uid>/ipc-<display>.sock`.
Characters outside ASCII letters, digits, `.`, `_`, and `-` in `DISPLAY` are replaced
with `_`. The socket is mode `0600`.

`lwmctl` resolves a socket in this order:

1. `--socket PATH`
2. `LWM_SOCKET`
3. the root-window `_LWM_IPC_SOCKET` property
4. the default path above

LWM services up to 32 ordinary connections and eight subscriptions concurrently.
Commands still execute sequentially on the WM event loop; a partial request or slow
reply does not occupy another client's slot. Excess connections are rejected, with
`error busy` when the rejection reply can be delivered. Requests and replies each have a
500 ms deadline. A command line must contain fewer than 4096 bytes including its
newline; replies are limited to 8 MiB. Writes resume when the socket is writable, with
at most 64 KiB written per connection per dispatch. A timed-out exchange is
disconnected.

`lwmctl --timeout MS` bounds connection, request transmission, response, and
subscription acknowledgement waits (default 2000 ms). A full listener backlog is retried
within the original connection deadline. Idle subscriptions do not time out; once an
event starts arriving, its complete line must arrive within the timeout. Explicit socket
selection takes precedence even if the path is unavailable. Discovery properties must be
complete text without embedded NULs.

## Framing

Send one text command followed by `\n`. LWM reads only the first line and returns one
line:

- `ok`
- `ok VALUE`
- `error MESSAGE`

A write-side EOF also terminates a nonempty request. The connection closes after the
complete reply is sent. `lwmctl` removes the `ok` envelope, prints `VALUE` to stdout,
and prints an error message to stderr with a nonzero exit status. Empty, truncated, or
unrecognized replies are errors. A subscription begins only after the exact `ok
subscribed` acknowledgement. Interrupted reads and writes retry. Truncated subscription
events are rejected without printing partial contents. Ordinary command output failures
return nonzero; a closed stdout pipe ends a subscription normally.

`lwmctl --help` and command-specific help (for example `lwmctl workspace --help`) print
to stdout without connecting. Usage and runtime errors print to stderr and return status
1; successful commands return 0. Use `--` before arguments that resemble options, and
shell-quote names or paths containing spaces. Arguments containing line breaks or NULs
cannot be represented by this line protocol.

## Commands

Queries answer from the current state. Every other command is the same action a key
binding can perform (see [config.toml.example](config.toml.example)), executed by the
same code; only `spawn` has no IPC spelling, because IPC callers can start processes
themselves. Window commands act on the active window and return `error no active window`
without one. Monitor- and workspace-relative commands target the focused monitor.
Consecutive `focus next` / `focus prev` commands retain their starting order, including sticky windows, and
skip windows that are no longer eligible. Ordinary activation, a change of
monitor/workspace or active window, or a new client registration starts a fresh
recent-use traversal. Cycling returns an error when no window is eligible, including
while showing the desktop.

| Command | Result |
| --- | --- |
| `ping` | `pong` |
| `state` | consistent combined state snapshot as JSON |
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
| `notify-attention window=<xid>` | mark a non-active tiled/floating window urgent |
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
adjust` clamps to that range and replies `ratio unchanged` at a bound. A definite exec failure is logged and leaves the scratchpad retryable.
Successful exec does not guarantee a matching window: `scratchpad cancel-launch NAME`
clears a pending launch so the user can retry. It is a no-op for an empty or already
claimed slot and rejects unknown names. It does not terminate a process; a late matching
window can still be claimed. Both named scratchpad commands reject unknown names.

## Logging status

`lwmctl log status` is read-only and does not emit `state_change`. It returns:

```json
{"target":"journal","level":"info","instance":"1336-43855073878725","active":true,"backend_notifications":0,"last_backend_notification":""}
```

`active` means a logger is enabled, not that output has been delivered. It is false at
level `off` and after shutdown. Initialization failures are reported by the CLI before
the WM starts.

`backend_notifications` counts Quill notifications, including overflow summaries,
formatting errors, and reported sink errors. `last_backend_notification` retains the
first 1 KiB of the latest notification. Notifications are asynchronous; a stalled worker
cannot report further errors until it resumes. They are not an exact count of dropped
messages. In particular, libsystemd treats an absent journal as success, and successful
sends do not guarantee persistent storage.

The count and logging `instance` survive failed exec and reset on successful exec. This
logger lifetime differs from the WM `instance` used by state snapshots and
subscriptions: reconstructing the WM after failed exec creates a new WM instance while
retaining the logger.

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
      "layout": "master-stack"
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
only in `window list`.

`scratchpad list` returns:

```json
{
  "named": [{"name": "terminal", "window": 123456, "pending": false}],
  "pool": [234567]
}
```

A named entry uses `window: 0` when it has not claimed a window.

All window values are X11 window IDs. Monitor and workspace indices are zero-based
runtime indices; none of these identifiers are persistent.

## Subscriptions

`subscribe [FILTER]` is the one long-lived request. After `ok subscribed`, LWM sends one
JSON object per line. An omitted filter selects all events. Otherwise, use a
comma-separated list; unknown names are ignored and a filter containing no recognized
name is rejected.

| Event | Fields after `event` |
| --- | --- |
| `window_map` | `window`, `class`, `kind`; `monitor` and `workspace` for tiled and floating clients |
| `window_unmap` | `window`, `kind`; `monitor` and `workspace` for tiled and floating clients |
| `focus_change` | `window`, `class`, `title` |
| `workspace_switch` | `monitor`, `from`, `to` |
| `layout_change` | `action`; `value` or `delta` where applicable |
| `config_reload` | `success`, `source`; `error` on failure |
| `key_action` | `action` |
| `state_change` | no additional fields; refresh a state snapshot |

`kind` is `tiled`, `floating`, `dock`, `desktop`, or `popup` (direct-mapped, `window_map`
only). Docks and desktops have no workspace. `config_reload.source` is `ipc`, `sighup`,
or `keybind`. LWM does not emit a `focus_change` event when focus is cleared.

`key_action.action` is the binding's action name as written in the configuration; the
monitor actions add their direction (`focus_monitor_left`, `move_to_monitor_right`).
`layout_change.action` names the layout action (`set_layout` with a string `value`,
`set_ratio` with a numeric `value`, `adjust_ratio` with a `delta`, `reset_ratios`,
`swap_next`, `swap_prev`), whether triggered by a binding or IPC, or `resize_split` with
the new ratio as `value` when a pointer drag of a split ends.

Events are sent after the triggering operation has completed its geometry, focus,
property, and stacking updates. Within an operation, events are ordered as workspace
changes, final focus, map/unmap, then action or reload outcomes, followed by
`state_change` when subscribed. Multiple workspace changes on one monitor coalesce to
its initial and final workspace; intermediate focus choices are omitted. Explicit
same-window focus still emits `focus_change`. This ordering does not combine separate X
events or separate IPC commands.

Every event also has an `instance` string identifying this WM lifetime and a
monotonically increasing `sequence` number within it. Filtering may leave gaps; a gap is
not evidence of loss. Restart and reconstruction after failed exec both start a new WM
instance.

Subscription delivery is ordered and non-blocking. Each subscriber has at most 1 MiB of
queued output and must make write progress within 500 ms while output is pending.
Partial writes resume; queue overflow, delivery timeout, or a socket error disconnects
the subscriber instead of silently dropping events. An event larger than the queue limit
also disconnects it. Registration precedes acknowledgement, and subsequent events queue
behind `ok subscribed`.

There is no replay. Consumers must treat EOF or an error as a reason to reconnect and
resynchronize, and ignore unknown JSON fields and event names.

## State consumers and recovery

`state` returns one snapshot containing `instance`, `sequence`, `workspaces`, `windows`,
and `scratchpads`. The last three values have exactly the shapes of the corresponding
list commands above. The sequence is the last published event at the time of the
snapshot; producing a snapshot does not publish an event.

For a panel or other state consumer:

1. Open `subscribe state_change` and wait for `ok subscribed`.
2. Query `state` on a separate connection and install that snapshot.
3. Ignore queued events from that instance whose sequence is at or before the
   snapshot's sequence. A later `state_change` means to query a fresh snapshot;
   several pending notifications can share one refresh.
4. On EOF, errors, or a different instance, discard the old stream and repeat.

`state_change` is an invalidation notification after completion, not a patch. It is
sent when the exposed list state (workspaces, windows, and scratchpads) differs from the
state behind the previous notification, including metadata, urgency, placement, and
scratchpad changes. Read-only commands and no-op actions do not emit it; the first
completed operation after a new subscription may notify without a change. Individual
focus/map/workspace events remain available for consumers that need those occurrences
instead of a current-state view. Notifications cover only the fields exposed by the list
API, not every X property or application state.
