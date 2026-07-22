# IPC

LWM exposes a local, newline-delimited Unix-socket protocol. `lwmctl` is the
supported command-line client; other local tools can use the same socket.

## Socket discovery

`lwmctl` checks these sources, in order:

1. `--socket PATH`
2. `LWM_SOCKET`
3. the root-window `_LWM_IPC_SOCKET` property
4. the default runtime socket path

The root-window property is a UTF-8 string and is removed during normal WM
shutdown.

## Requests and replies

Send one command followed by `\n`.

- `ok` means success without a result.
- `ok VALUE` means success and returns `VALUE` (which may be JSON).
- `error MESSAGE` means the command failed.

Normal commands close after the response. `subscribe [FILTER]` keeps the
connection open after replying `ok subscribed` and then sends one JSON event
per line. An empty filter selects every event; otherwise use a comma-separated
list of event names. Unknown filter names are ignored, and a filter with no
recognized names is rejected.

## JSON commands

`workspace list` returns:

```json
{
  "focused_monitor": 0,
  "monitors": [
    {
      "index": 0,
      "name": "HDMI-0",
      "current_workspace": 0,
      "workspaces": [
        {"index": 0, "name": "1", "current": true, "window_count": 2, "layout": "master-stack"}
      ]
    }
  ]
}
```

`window list` returns the currently managed non-dock/non-desktop windows:

```json
{
  "focused": 123456,
  "windows": [
    {
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
    }
  ]
}
```

All monitor and workspace indices are zero-based. They are runtime indices;
window identifiers are X11 window IDs, not persistent IDs.

## Events

Every event has an `event` field. Current payloads are:

| Event | Additional fields |
| --- | --- |
| `window_map` | `window`, `class`, `kind`, and usually `monitor`, `workspace` |
| `window_unmap` | `window`, `kind`, `monitor`, `workspace` |
| `focus_change` | `window`, `class`, `title` |
| `workspace_switch` | `monitor`, `from`, `to` |
| `layout_change` | `action`; `value` or `delta` for actions that need one |
| `config_reload` | `success`, `source`, and `error` when unsuccessful |
| `key_action` | `action` |

There is currently no IPC schema version negotiation. Consumers should ignore
unknown fields and tolerate new event types. Update this document with any
intentional wire-format change.
