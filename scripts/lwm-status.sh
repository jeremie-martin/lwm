#!/bin/sh
# LWM state that EWMH cannot carry, for this bar's monitor: a monocle layout
# marker and the number of windows stashed in the scratchpad pool.
# `lwm-status toggle` switches the focused workspace between master-stack and monocle.
if [ "$1" = toggle ]; then
    layout=$(lwmctl state | jq -r '.workspaces as $w | $w.monitors[$w.focused_monitor] | .workspaces[.current_workspace].layout')
    [ "$layout" = monocle ] && exec lwmctl layout set master-stack
    exec lwmctl layout set monocle
fi
# watch prints the whole state on every change; print only changed labels.
lwmctl watch | jq --unbuffered -r --arg monitor "${MONITOR:-}" '
    .workspaces as $w
    | (first($w.monitors[] | select(.name == $monitor)) // $w.monitors[$w.focused_monitor]) as $m
    | [.scratchpads.pool[] as $id | .windows.windows[] | select(.id == $id and .iconic)] | length as $stashed
    | [ (if $m.workspaces[$m.current_workspace].layout == "monocle" then "%{F#EC407A}[M]%{F-}" else empty end),
        (if $stashed > 0 then "%{F#4DD0E1}[+\($stashed)]%{F-}" else empty end) ]
    | join(" ")' | awk '$0 != last { print; fflush(); last = $0 }'
