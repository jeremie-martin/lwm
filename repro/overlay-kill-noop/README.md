# Repro: `kill` keybind is a no-op when there is no active window

Standalone reproduction (not wired into the Catch2 suite — lives on the side).

## The bug

`super+q` is bound to the `kill` action, which targets `active_window_`. The
root cause is upstream of `kill`: an **overlay-layer window can never become the
active window**, so `kill` (and focus-follows-mouse, and cycling) can never
reach it. When such a window covers the screen and there is nothing focusable
beneath it, `active_window_ == None` and `super+q` is a silent no-op — the
window cannot be dismissed from the keyboard at all.

`is_focus_eligible` (`src/lwm/wm_focus.cpp`) conflated two concerns:

```cpp
if (client.kind == Client::Kind::Dock || client.kind == Client::Kind::Desktop
    || client.layer == WindowLayer::Overlay)   // <-- layer != focusability
    return false;
```

The overlay *layer* is a **stacking** concern; it should not also make a window
unfocusable. Docks/desktops correctly decline focus by **kind**, independent of
layer.

## Where it bit us

The ABILITY companion (Electron) runs a fullscreen, transparent, always-on-top,
**click-through** overlay (`setIgnoreMouseEvents`). When its Vite dev server
died, the window went blank-white and stayed on top. Because that overlay never
becomes lwm's `active_window_`, `super+q` was a no-op and the only recovery was
`pkill` from a TTY.

This repro stands in for that overlay with a tiny `_NET_WM_WINDOW_TYPE_UTILITY`
window lifted to the `overlay` layer — the same path
`tests/test_integration_overlay.cpp` proves never becomes the active window.

## Run it

```sh
./repro.sh              # visible: nested Xephyr you can watch; try Super+Q yourself
HEADLESS=1 ./repro.sh   # headless: Xvfb, automated, prints verdict + exit code
```

Exit code: `0` = bug absent (overlay was killed → lwm is fixed),
`1` = bug present (overlay survived Super+Q), `2` = inconclusive (setup/control
failed).

### What it does

A persistent **base** app stands in for whatever the overlay covers, so the
overlay gets a distinct X id and the active-window checks are unambiguous.

1. Starts its own X server and an `lwm` with `super+q = kill` plus the
   utility→overlay rule.
2. Maps the **base** window → asserts it is active.
3. Maps the **overlay** over it → asserts the overlay did **not** steal focus
   on map (active still the base).
4. Moves the pointer onto the overlay → asserts focus-follows-mouse focuses the
   overlay (active becomes the overlay — unconfounded, since it was the base
   immediately before).
5. Presses `super+q` → asserts the overlay is killed and the base survives.

### Observed

```
broken lwm:  overlay never active → super+q no-op → overlay survives   (exit 1)
fixed lwm:   no steal on map; pointer-over focuses; super+q kills it    (exit 0, PASS)
```

## The fix

Two surgical edits, splitting "*can* be focused" from "*should grab* focus":

1. `src/lwm/wm_focus.cpp` — `is_focus_eligible` drops the
   `layer == WindowLayer::Overlay` clause. Overlay-layer windows become
   focus-eligible (focus-follows-mouse, cycling, and `kill` can reach them);
   only `kind == Dock/Desktop` still decline focus.
2. `src/lwm/wm_events.cpp` (`map_floating_window`) — overlay-layer windows do
   **not** auto-grab focus on map (`&& client.layer != WindowLayer::Overlay`),
   so they stay focusable-on-demand without stealing focus from the app they
   cover. This keeps the `test_integration_overlay` no-steal invariant green.

With both, `./repro.sh` exits `0` (PASS) and the full `lwm_tests` suite passes.
