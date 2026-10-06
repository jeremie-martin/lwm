#!/bin/bash
# Replace this session's Polybar processes with one bar per connected output.

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
CONFIG_FILE="$PROJECT_DIR/config/polybar.ini"
export LWM_SCRIPTS="$SCRIPT_DIR"

killall -q polybar

while pgrep -u $UID -x polybar >/dev/null; do sleep 1; done

if type "xrandr" > /dev/null 2>&1; then
    for m in $(xrandr --query | grep " connected" | cut -d" " -f1); do
        MONITOR=$m polybar --config="$CONFIG_FILE" main &
    done
else
    polybar --config="$CONFIG_FILE" main &
fi

echo "Polybar launched"
