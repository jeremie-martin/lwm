#!/bin/sh
# Show a desktop notification and mark this terminal/window for LWM attention.

set -u

[ "$#" -gt 0 ] || exit 0

if command -v notify-send >/dev/null 2>&1; then
    notify-send "$@" >/dev/null 2>&1 || true
fi

case "${WINDOWID:-}" in
    ''|*[!0-9]*)
        exit 0
        ;;
esac

if command -v lwmctl >/dev/null 2>&1; then
    lwmctl notify-attention "window=$WINDOWID" >/dev/null 2>&1 || true
fi

exit 0
