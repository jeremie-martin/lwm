#!/usr/bin/env bash
#
# Reproduce: lwm's "kill" keybind (super+q) is a silent no-op when no window is
# the active window — e.g. when only a non-activating overlay is on screen.
#
# Why this matters: the ABILITY companion runs a fullscreen, always-on-top,
# click-through overlay. When its dev server died the window went blank-white
# and stayed on top, but lwm had no active_window_, so super+q (bound to `kill`)
# did nothing. Only `pkill` recovered the screen. See KillAction in
# src/lwm/wm_events.cpp: `if (active_window_ == XCB_NONE) return false;`.
#
# Usage:
#   ./repro.sh              # visible: opens a nested Xephyr you can watch
#   HEADLESS=1 ./repro.sh   # headless: Xvfb, automated, prints PASS/FAIL + exit code
#
# Exit code: 0 if the bug is ABSENT (ghost was killed — i.e. lwm is fixed),
#            1 if the bug is PRESENT (ghost survived super+q).

set -u

here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/../.." && pwd)"
lwm_bin="$root/build/src/app/lwm"
ghost_src="$here/ghost_overlay.c"
ghost_bin="$here/ghost_overlay"

headless="${HEADLESS:-0}"

log()  { printf '  %s\n' "$*"; }
step() { printf '\n== %s\n' "$*"; }

cleanup() {
    [ -n "${base_pid:-}" ] && kill "$base_pid" 2>/dev/null
    [ -n "${ovl_pid:-}"  ] && kill "$ovl_pid"  2>/dev/null
    [ -n "${lwm_pid:-}"  ] && kill "$lwm_pid"  2>/dev/null
    [ -n "${xsrv_pid:-}" ] && kill "$xsrv_pid" 2>/dev/null
    [ -n "${cfg_dir:-}"  ] && rm -rf "$cfg_dir"
}
active_win() { xprop -root _NET_ACTIVE_WINDOW 2>/dev/null | grep -o '0x[0-9a-f]*' | head -1; }
hexid()      { printf '0x%x' "$1"; }
alive()      { [ -n "$(xdotool search --name "$1" 2>/dev/null)" ]; }
trap cleanup EXIT

# --- preflight ---------------------------------------------------------------
for tool in xdotool xprop cc; do
    command -v "$tool" >/dev/null || { echo "missing required tool: $tool"; exit 2; }
done
[ -x "$lwm_bin" ] || { echo "lwm not built at $lwm_bin (build it first)"; exit 2; }

if [ "$headless" = "1" ]; then
    xserver="$(command -v Xvfb)" || { echo "Xvfb not found"; exit 2; }
else
    xserver="$(command -v Xephyr)" || { echo "Xephyr not found (use HEADLESS=1 for Xvfb)"; exit 2; }
fi

step "Build ghost overlay client"
cc "$ghost_src" -o "$ghost_bin" -lxcb || { echo "compile failed"; exit 2; }
log "built $ghost_bin"

# --- pick a free display -----------------------------------------------------
disp=""
for n in $(seq 110 150); do
    [ -e "/tmp/.X${n}-lock" ] && continue
    disp=":$n"; break
done
[ -n "$disp" ] || { echo "no free display number found"; exit 2; }

step "Start X server ($([ "$headless" = 1 ] && echo Xvfb || echo Xephyr)) on $disp"
if [ "$headless" = "1" ]; then
    "$xserver" "$disp" -screen 0 1280x720x24 -nolisten tcp >/dev/null 2>&1 &
else
    "$xserver" "$disp" -screen 1280x720 -resizeable >/dev/null 2>&1 &
fi
xsrv_pid=$!
for _ in $(seq 1 50); do xdotool getdisplaygeometry >/dev/null 2>&1 && DISPLAY="$disp" xdpyinfo >/dev/null 2>&1 && break; sleep 0.1; done
export DISPLAY="$disp"
for _ in $(seq 1 50); do xdpyinfo >/dev/null 2>&1 && break; sleep 0.1; done
xdpyinfo >/dev/null 2>&1 || { echo "X server did not come up on $disp"; exit 2; }
log "X server ready on $disp"

# --- config: overlay rule + super+q -> kill ----------------------------------
cfg_dir="$(mktemp -d)"
mkdir -p "$cfg_dir/lwm"
cat > "$cfg_dir/lwm/config.toml" <<'TOML'
[workspaces]
count = 1
names = ["1"]

# Lift utility windows to the overlay layer (sticky, on-top, non-activating) —
# same rule used by tests/test_integration_overlay.cpp.
[[rules]]
match = { type = "utility" }
apply = { layer = "overlay" }

# The user's real keybind.
[[binds]]
key = "super+q"
kill = true
TOML

step "Start lwm"
XDG_CONFIG_HOME="$cfg_dir" "$lwm_bin" >/dev/null 2>&1 &
lwm_pid=$!
for _ in $(seq 1 50); do
    [ -n "$(xprop -root _NET_SUPPORTING_WM_CHECK 2>/dev/null | grep -o '0x[0-9a-f]*')" ] && break
    sleep 0.1
done
log "lwm pid=$lwm_pid"

# A persistent base app stands in for whatever the companion overlay sits on
# top of. It stays alive throughout, so the overlay gets a *distinct* X id and
# the active-window checks below are unambiguous (no id reuse).
fail=0

step "Map base app window (stays alive — the thing the overlay covers)"
"$ghost_bin" --normal &
base_pid=$!
for _ in $(seq 1 50); do alive '^ghost-normal$' && break; sleep 0.1; done
alive '^ghost-normal$' || { echo "base window never appeared"; exit 2; }
base_hex="$(hexid "$(xdotool search --name '^ghost-normal$' | head -1)")"
log "base=$base_hex  active=$(active_win)  (expect base)"

step "Map overlay over the base — must NOT steal focus on map"
"$ghost_bin" &
ovl_pid=$!
for _ in $(seq 1 50); do alive '^ghost-overlay$' && break; sleep 0.1; done
alive '^ghost-overlay$' || { echo "overlay window never appeared"; exit 2; }
ovl_hex="$(hexid "$(xdotool search --name '^ghost-overlay$' | head -1)")"
sleep 0.3
a="$(active_win)"
if [ "$a" = "$ovl_hex" ]; then
    log "overlay=$ovl_hex  active=$a  !! overlay STOLE focus on map (no-steal invariant broken)"; fail=1
else
    log "overlay=$ovl_hex  active=$a  ok: overlay did not steal focus"
fi

step "Move pointer onto the overlay (focus-follows-mouse must reach it)"
xdotool mousemove 200 150
sleep 0.4
a="$(active_win)"
if [ "$a" = "$ovl_hex" ]; then
    log "active=$a  ok: focus-follows-mouse focused the overlay"
else
    log "active=$a  !! focus-follows-mouse did NOT focus the overlay (expected $ovl_hex)"; fail=1
fi

step "Press super+q  (kill the now-focused overlay)"
sleep 0.2
xdotool key --clearmodifiers super+q
sleep 1.0
ovl_alive=1; base_alive=1
alive '^ghost-overlay$' || ovl_alive=0
alive '^ghost-normal$'  || base_alive=0
log "after super+q: overlay_alive=$ovl_alive base_alive=$base_alive"

# --- verdict -----------------------------------------------------------------
echo
if [ "$ovl_alive" = 1 ]; then
    printf '>> BUG PRESENT: overlay survived super+q — keyboard cannot dismiss it.\n'
    verdict=1
elif [ "$base_alive" = 0 ]; then
    printf '>> WRONG TARGET: super+q killed the base app, not the overlay.\n'
    verdict=1
elif [ "$fail" = 1 ]; then
    printf '>> REGRESSION: overlay was killed, but a focus invariant above failed (see !! lines).\n'
    verdict=1
else
    printf '>> PASS: overlay stays focusable + killable, does not steal focus on map, base app untouched.\n'
    verdict=0
fi

if [ "$headless" != "1" ]; then
    printf '\nThe nested Xephyr is still open. Try pressing Super+Q over each window, then:\n'
    read -r -p "press Enter to tear everything down... " _
fi

exit "$verdict"
