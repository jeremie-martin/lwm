#!/bin/bash
# Forward only explicit X11 window hints from desktop notifications to LWM.
# Apps without an x-window-id hint cannot be attributed reliably.
set -euo pipefail
for program in busctl jq lwmctl; do
    command -v "$program" >/dev/null 2>&1 || {
        printf 'lwm-notify-bridge: missing dependency: %s\n' "$program" >&2
        exit 1
    }
done
busctl --user monitor \
    --match "interface='org.freedesktop.Notifications',member='Notify'" \
    --json=short |
while IFS= read -r line; do
    window=$(printf '%s\n' "$line" | jq -er '
        .payload.data[6]["x-window-id"].data // empty |
        tostring | select(test("^(0[xX][0-9a-fA-F]+|[0-9]+)$"))
    ') || continue
    [ "$window" != 0 ] || continue
    # Serialize this producer; errors remain visible to its service/terminal.
    lwmctl notify-attention "window=$window" >/dev/null || true
done
