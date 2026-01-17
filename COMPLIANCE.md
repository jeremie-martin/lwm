# LWM Protocol Compliance Checklist

This checklist captures the interoperability requirements LWM commits to.
Items are marked as:
- Implemented: fully supported in code
- Best-effort: supported but with documented limitations

## ICCCM (Non-Negotiable)

### WM ownership and discovery
- Implemented: claim `WM_S0` selection and set `_NET_SUPPORTING_WM_CHECK` (single-WM enforcement)
- Implemented: supporting window + `_NET_WM_NAME` for WM identification

### Map/Unmap/Destroy/Configure
- Implemented: manage `MapRequest` for top-level windows, ignore `override_redirect`
- Implemented: distinguish client vs WM-initiated `UnmapNotify` via tracking set
- Implemented: send `ConfigureNotify` for tiled windows on `ConfigureRequest`
- Implemented: honor `ConfigureRequest` for floating/override windows

### Focus and WM_PROTOCOLS
- Implemented: `WM_DELETE_WINDOW` honored for close requests
- Implemented: `WM_TAKE_FOCUS` sent when supported
- Implemented: `WM_HINTS` Input flag respected (fallback focus to root when needed)

### Transients and legacy state
- Implemented: `WM_TRANSIENT_FOR` drives floating management + placement
- Implemented: `WM_STATE` maintained for Normal/Iconic

### Size hints
- Implemented: `WM_NORMAL_HINTS` min/max/increment/aspect respected in layout and floating geometry

## EWMH Core (Non-Negotiable)

### Root properties
- Implemented: `_NET_SUPPORTED`
- Implemented: `_NET_CLIENT_LIST`
- Implemented: `_NET_ACTIVE_WINDOW`
- Implemented: `_NET_NUMBER_OF_DESKTOPS`, `_NET_DESKTOP_NAMES`, `_NET_CURRENT_DESKTOP`
- Implemented: `_NET_DESKTOP_VIEWPORT`
- Implemented: `_NET_WORKAREA`

### Per-window properties
- Implemented: `_NET_WM_DESKTOP`
- Implemented: `_NET_WM_STATE` (fullscreen/above/hidden/attention)
- Implemented: `_NET_WM_WINDOW_TYPE` handling (dock/normal/dialog/utility/etc.)

### Client messages
- Implemented: `_NET_CURRENT_DESKTOP`
- Implemented: `_NET_ACTIVE_WINDOW`
- Implemented: `_NET_WM_STATE` (ADD/REMOVE/TOGGLE)

### Struts/docks
- Implemented: `_NET_WM_STRUT` / `_NET_WM_STRUT_PARTIAL`

## EWMH Recommended (Robustness)

- Best-effort: `_NET_WM_PING` (WM sends ping on close; timeouts tracked)
- Best-effort: `_NET_WM_SYNC_REQUEST` + `_NET_WM_SYNC_REQUEST_COUNTER`
  (sync requests are sent on WM-initiated resizes; WM does not block waiting)
- Implemented: `_NET_CLOSE_WINDOW`
- Implemented: `_NET_WM_FULLSCREEN_MONITORS` (multi-monitor bounds honored)

## Notes

- `_NET_WM_SYNC_REQUEST` support is intentionally non-blocking to keep the event loop responsive.
- Fullscreen monitor selection uses the stored monitor indices to compute a bounding rectangle.
