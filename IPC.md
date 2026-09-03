# IPC

LWM exposes a local Unix-domain socket. `lwmctl` is the supported interactive
client; this document defines the raw protocol for integrations.

## Discovery and transport

The default socket is:

```text
$XDG_RUNTIME_DIR/lwm/ipc-<display>.sock
```

When `XDG_RUNTIME_DIR` is unset, LWM uses
`/tmp/lwm-<uid>/ipc-<display>.sock`. Characters outside ASCII letters, digits,
`.`, `_`, and `-` in `DISPLAY` are replaced with `_`. The socket is mode
`0600`.

`lwmctl` resolves a socket in this order:

1. `--socket PATH`
2. `LWM_SOCKET`
3. the root-window `_LWM_IPC_SOCKET` property
4. the default path above

LWM accepts one incomplete ordinary request at a time. A second connection is
answered with `error busy`. A request must complete within 500 ms and contain
fewer than 4096 received bytes, including the terminating newline.

## Framing

Send one text command followed by `\n`. LWM reads only the first line and
returns one line:

- `ok`
- `ok VALUE`
- `error MESSAGE`

The connection then closes. `lwmctl` removes the `ok` envelope, prints `VALUE`
to stdout, and prints an error message to stderr with a nonzero exit status.

## Commands

Monitor- and workspace-relative commands target the focused monitor.

| Command | Result |
| --- | --- |
| `ping` | `pong` |
| `version` | LWM version |
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
| `workspace list` | workspace state as JSON |
| `focus window=<xid>` | focus a window, switching workspace if needed |
| `focus next` / `focus prev` | move through the MRU focus cycle |
| `window list` | tiled and floating client state as JSON |
| `scratchpad stash` | move the active window to the generic pool |
| `scratchpad cycle` | cycle the generic pool |
| `scratchpad toggle NAME` | show, hide, or launch a named scratchpad |
| `scratchpad list` | named and generic scratchpad state as JSON |

The permitted ratio range is `[min_ratio, 1 - min_ratio]` from the active
configuration. Scratchpad commands are currently raw-protocol commands and are
not exposed by `lwmctl`.

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

`workspace list.window_count` counts tiled workspace membership; floating
clients appear only in `window list`.

`scratchpad list` returns:

```json
{
  "named": [{"name": "terminal", "window": 123456, "pending": false}],
  "pool": [234567]
}
```

A named entry uses `window: 0` when it has not claimed a window.

All window values are X11 window IDs. Monitor and workspace indices are
zero-based runtime indices; none of these identifiers are persistent.

## Subscriptions

`subscribe [FILTER]` is the one long-lived request. After
`ok subscribed`, LWM sends one JSON object per line. An omitted filter selects
all events. Otherwise, use a comma-separated list; unknown names are ignored
and a filter containing no recognized name is rejected. At most eight
subscribers may be connected.

| Event | Fields after `event` |
| --- | --- |
| `window_map` | `window`, `class`, `kind`; `monitor` and `workspace` except for a direct-mapped popup |
| `window_unmap` | `window`, `kind`, `monitor`, `workspace` |
| `focus_change` | `window`, `class`, `title` |
| `workspace_switch` | `monitor`, `from`, `to` |
| `layout_change` | `action`; `value` or `delta` where applicable |
| `config_reload` | `success`, `source`; `error` on failure |
| `key_action` | `action` |

`config_reload.source` is `ipc`, `sighup`, or `keybind`. LWM does not emit a
`focus_change` event when focus is cleared.

Delivery is best-effort and non-blocking. LWM drops an event when a subscriber
would block and disconnects a subscriber after a partial or failed write. There
is no replay, ordering acknowledgement, or protocol-version negotiation.
Consumers should ignore unknown JSON fields and event names.
