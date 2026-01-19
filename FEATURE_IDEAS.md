# LWM Feature Ideas (Draft)

This is a living ideas backlog for LWM. It starts from current behavior and grows with
new concepts. The goal is to keep the system simple, consistent, and future-proof.

---

## Current Baseline (Keep Strong)

- Per-monitor workspaces with EWMH desktop mapping.
- Master-stack tiling layout.
- Floating windows for dialogs/transients.
- Internal bar (optional), struts, workarea.
- Focus-follows-mouse + explicit focus management.
- Workspace toggle (current <-> previous).
- Tiled drag-reorder with drop-based layout insertion.
- Config-driven keybinds and mousebinds.

---

## Principles

1) Predictable: no hidden state, no implicit magic.
2) Simple core, optional extensions.
3) Safe defaults; everything configurable.
4) Minimal visual noise; clarity over cleverness.
5) Per-monitor behavior is the default, not a special case.

---

## Survey Highlights (From Other WMs)

- i3: tabbed/stacked layouts, binding modes (modal keymaps), marks for jump/swap, criteria rules.
- bspwm: explicit split direction, preselect insertion region, tree rotations/mirrors.
- hyprland: pseudotiling, special workspaces (scratchpad), window groups (tabbed), live config reload.
- qtile: remote command shell + scriptability for full state control.
- xmonad: per-workspace layout selection, large extension library model.
- river: hot-swappable window-manager process (policy decoupling).

---

## High-Value, Low-Complexity Ideas

- Per-workspace layout toggle (e.g., master-stack <-> monocle).
- Layout ratio control (grow/shrink master, swap master).
- "Send to monitor and follow" (move focused window and switch to target).
- "Focus next/prev window in stack" (cyclic focus without changing order).
- "Swap with next/prev" (reorder without dragging).
- "Move within stack" (nudge up/down in stack).
- Smart focus restore after window close (prefer last focused non-iconic).
- Config option: focus new windows or not (spawn follow).
- Config option: warp pointer on monitor switch (on/off).
- Config option: focus-follows-mouse on/off.
- Config option: tiling padding per monitor/workspace.
- Toggle borders per-workspace or per-window.
- Urgency handling policy (auto focus? change border? bar hint only?).
- Binding modes (e.g., resize/move mode while held).
- Marks for windows (jump to mark, swap with mark).

---

## Workspace & Monitor Model

- Workspace scratchpad: hidden workspace that can be toggled as overlay.
- Optional full workspace history (in addition to current/previous).
- "Mirror" mode: keep same workspace index across monitors.
- Workspace move: swap entire workspaces between monitors.
- Workspace rename at runtime (bar updates + EWMH names).
- Workspace rules: default workspace for app classes.
- Workspace-to-output assignment (sticky mapping of workspaces to monitors).

---

## Layout System

- Additional layouts: monocle, centered master, grid, columns, fibonacci.
- Layout parameters: master ratio, master count, stack direction.
- Layout per workspace (persisted).
- Layout constraints: respect size hints optionally per workspace.
- "Fixed master": keep master window size constant if possible.
- Layout preview overlay during drag (optional).
- Tabbed/stacked layout modes (grouped windows).
- Explicit split direction control (force vertical/horizontal splits).
- Preselect insertion region for next window.
- Rotate/mirror the layout tree.

---

## Window Rules & Policies

- Rule system by WM_CLASS/WM_NAME/role:
  - float/tiling, workspace target, monitor target, border style
  - focus policy, above/below, skip taskbar/pager
- Per-app floating size/placement defaults.
- Transient handling rules (inherit workspace/monitor from parent).
- Delayed rules for late WM_CLASS/WM_NAME updates.
- Regex criteria for rules (class/title/role).

---

## Floating & Geometry

- Snap to edges when moving floating windows.
- Optional "tiling-aware float": snap into layout slots on drop.
- Restore last floating geometry per window.
- Per-monitor floating layer ordering (above tiled but below docks).
- Pseudotiling: keep a window's size but place within tile slot.

---

## Input & Interaction

- Mousebind for "swap with window under cursor" (no drag).
- Modifier + scroll to change workspace.
- Modifier + scroll to cycle focus within stack.
- Drag to move across monitors already exists (keep).
- Drag to reorder stack via keyboard (no mouse).
- Focus parent/child in the layout tree.

---

## Bar & UI

- Layout indicator in internal bar.
- Active window title with truncation rules.
- Workspace activity markers (empty/occupied/urgent).
- Clickable bar regions:
  - left/right click to change workspace
  - scroll to cycle workspace
- Optional per-monitor bar toggle.
- External bar integration hooks (scriptable output).
- Optional "mode" indicator (when a binding mode is active).

---

## EWMH/ICCCM Completeness

- Optional sticky windows (with clear UX constraints).
- Optional maximize states (if layout supports).
- Better _NET_WM_STATE adherence for above/below.
- Client-initiated move/resize for tiled windows (ignored vs. policy).
- Track and expose per-window capabilities in EWMH allowed actions.

---

## IPC & Extensibility

- `lwmctl` CLI (query state, switch workspace, move window, etc.).
- Simple IPC over Unix socket or X11 client messages.
- Config reload without restart.
- Hooks: pre/post workspace switch, window manage/unmanage.
- Remote scriptability (drive layout/workspace/window actions externally).
- Optional plugin interface for advanced features.

---

## Multi-Monitor & Hotplug

- Preserve workspace assignment across hotplug events.
- Keep "previous workspace" per monitor across hotplug.
- Per-monitor layout ratio and padding.
- Prefer pointer monitor for new windows (already behavior, make optional).

---

## Quality & Safety

- Add unit tests for layout slot mapping.
- Add tests for drag-reorder edge cases (cross-monitor, empty target).
- More deterministic tests around focus state changes.
- Logging levels per subsystem.

---

## Stretch / Experimental Ideas

- Animated transitions (opt-in, minimal).
- "Overview" mode with all workspaces on a monitor.
- Live layout preview ghost while dragging.
- Adaptive gap/border based on window count.
- "Swallow" feature (terminal swallows child GUI).
- Hot-reloadable config (no restart, immediate apply).

---

## Open Questions / Decisions Needed

1) Do we want any tag-like multi-assignment or keep strict workspaces?
2) Do we want true dynamic layouts (params per workspace), or global?
3) Should tiled windows ever respond to client move/resize requests?
4) Should focus follow pointer be configurable per-monitor?

---

## Next Iteration Ideas

- Rank the backlog by value and complexity.
- Decide on IPC mechanism (X11 client messages vs. Unix socket).
- Pick 2-3 near-term features that align with simplicity.

---

## Sources Consulted

- i3 user guide: https://i3wm.org/docs/userguide.html
- bspwm README: https://github.com/baskerville/bspwm
- Hyprland README: https://github.com/hyprwm/Hyprland
- xmonad README: https://github.com/xmonad/xmonad
- Qtile README: https://github.com/qtile/qtile
- river README: https://github.com/riverwm/river
