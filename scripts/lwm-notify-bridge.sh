#!/bin/bash
# lwm-notify-bridge: D-Bus notification monitor → LWM urgency bridge.
#
# Monitors org.freedesktop.Notifications.Notify method calls via busctl
# and tells LWM which window to mark urgent. Resolution layers:
#   1. x-window-id hint (exact, if sender includes it via $WINDOWID)
#   2. app-name / desktop-entry fallback only when LWM finds one unique client
#
# Dependencies: busctl, jq, lwmctl
# Usage: lwm-notify-bridge &

busctl --user monitor \
    --match "interface='org.freedesktop.Notifications',member='Notify'" \
    --json=short 2>/dev/null |
while IFS= read -r line; do
    parsed=$(printf '%s' "$line" | jq -r '[
        .payload.data[0] // "",
        (.payload.data[6]["x-window-id"].data // 0 | tostring),
        .payload.data[6]["desktop-entry"].data // ""
    ] | @tsv' 2>/dev/null) || continue

    IFS=$'\t' read -r app_name x_wid desktop_entry <<< "$parsed"

    # Layer 1: explicit x-window-id hint (e.g. notify-send -h int:x-window-id:$WINDOWID)
    if [ -n "$x_wid" ] && [ "$x_wid" != "0" ]; then
        lwmctl notify-attention "window=$x_wid" >/dev/null 2>&1 &
        continue
    fi

    # Layer 2: name-based fallback — LWM ignores ambiguous app/class matches.
    args=()
    [ -n "$desktop_entry" ] && args+=("desktop-entry=$desktop_entry")
    [ -n "$app_name" ] && args+=("app-name=$app_name")
    if (( ${#args[@]} )); then
        lwmctl notify-attention "${args[@]}" >/dev/null 2>&1 &
    fi
done
