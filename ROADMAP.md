# Roadmap

Open work only. Closed items live in commit history.
Audited against `git log` and `src/` on 2026-05-16.

## Layout

- More layout strategies in `src/lwm/layout/strategy.cpp` — `build_layout_tree` currently returns master-stack regardless of the selected strategy. Candidates: monocle/tabbed, dwindle/spiral, columns, centered-master.
- Per-workspace layout switching — `lwmctl layout set` already parses; needs the strategies above wired.
- Keyboard window swap/nudge (e.g. `Super+Shift+J/K`). The reorder drag already moves tiled windows; keyboard equivalent is missing.
- Master ratio grow/shrink at runtime.
- Per-monitor layout parameters (ratio, gaps).

## Multi-monitor

- Cursor warp on focus-monitor-left/right (config flag exists; verify wiring).
- Seamless drag of floating windows across monitor boundaries.
- Multi-output integration harness covering cross-monitor moves, floating geometry, and hotplug rebind. `test_monitor_hotplug.cpp` covers hotplug only.

## Protocol / X11

- XSync resize — upgrade fire-and-forget to blocking sync with timeout for slow clients.
- `_NET_WM_WINDOW_OPACITY` so opacity rules cooperate with picom/compton.
- EWMH partial-strut handling for panels that reserve space on only part of an edge.

## Rules & config

- Auto-move-to-workspace / auto-float-by-size / opacity in window rules.
- Validate or reject out-of-range rule geometry before narrowing into `Geometry`.
- Config flag for "focus new windows" behavior.
- User-facing workspace rename action (the `_NET_DESKTOP_NAMES` write path already exists).

## Doc debt

- Reconcile `ARCHITECTURE.md` with the current stacking and hotplug authorities — last touched before the recent correctness sweep.

## Needs design first

- True bspwm-style split-graph layouts (binary split tree currently drives tiled resize only).
- Global marks / bookmarks for jump and swap.
- Policy for tiled-window client `_NET_WM_MOVERESIZE` requests.
- Non-blocking animation system.

## Open questions

- Strict single-assignment workspaces vs. tag-like multi-assignment?
- Focus-follows-mouse: global toggle, per-monitor, or both?
- IPC surface: Unix socket only (current), or also X11 client-messages?

## Out of scope

Plugin system, full session-management integration, integrated panels/widgets.
