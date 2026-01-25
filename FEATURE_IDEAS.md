# LWM Feature Ideas

> **Documentation Navigation**
> - Previous: [BEHAVIOR.md](BEHAVIOR.md) (User-facing behavior) | [CLAUDE.md](CLAUDE.md) (Development guide)
> - Related: [DOCS_INDEX.md](DOCS_INDEX.md) (Documentation roadmap) | [IMPLEMENTATION.md](IMPLEMENTATION.md) (Architecture)

This is a living ideas backlog for LWM. It grows from current behavior while keeping the system simple, consistent, and future-proof.

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

These principles guide feature selection and prioritization throughout this document.

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

- Per-workspace layout toggle (e.g., master-stack <-> monocle)
- Layout ratio control (grow/shrink master, swap master)
- Cyclic focus in stack without changing order
- Swap with next/prev (reorder without dragging)
- Move within stack (nudge up/down)
- Smart focus restore after window close (prefer last focused non-iconic)
- Config option: focus new windows or not (spawn follow)
- Config option: focus-follows-mouse on/off
- Config option: tiling padding per monitor/workspace
- Toggle borders per-workspace or per-window
- Urgency handling policy (auto focus? change border? bar hint only?)
- Binding modes (e.g., resize/move mode while held)
- Marks for windows (jump to mark, swap with mark)

---

## Workspace & Monitor Model

- Workspace scratchpad: hidden workspace toggled as overlay
- Optional full workspace history (in addition to current/previous)
- "Mirror" mode: keep same workspace index across monitors
- Swap entire workspaces between monitors
- Workspace rename at runtime (bar updates + EWMH names)
- Workspace rules: default workspace for app classes
- Workspace-to-output assignment (sticky mapping of workspaces to monitors)

---

## Layout System

- Additional layouts: monocle, centered master, grid, columns, fibonacci
- Layout parameters: master ratio, master count, stack direction
- Layout per workspace (persisted)
- Layout constraints: respect size hints optionally per workspace
- Fixed master: keep master window size constant if possible
- Layout preview overlay during drag (optional)
- Tabbed/stacked layout modes (grouped windows)
- Explicit split direction control (force vertical/horizontal splits)
- Preselect insertion region for next window
- Rotate/mirror the layout tree

---

## Window Rules & Policies

- Rule system by WM_CLASS/WM_NAME/role:
  - float/tiling, workspace target, monitor target, border style
  - focus policy, above/below, skip taskbar/pager
- Per-app floating size/placement defaults
- Transient handling rules (inherit workspace/monitor from parent)
- Delayed rules for late WM_CLASS/WM_NAME updates
- Regex criteria for rules (class/title/role)

---

## Floating & Geometry

- Snap to edges when moving floating windows
- Tiling-aware float: snap into layout slots on drop
- Restore last floating geometry per window
- Per-monitor floating layer ordering (above tiled but below docks)
- Pseudotiling: keep window size but place within tile slot

---

## Input & Interaction

- Mousebind for swap with window under cursor (no drag)
- Modifier + scroll to change workspace
- Modifier + scroll to cycle focus within stack
- Drag to move across monitors (existing behavior, keep)
- Keyboard-based stack reorder (no mouse)
- Focus parent/child in layout tree

---

## Bar & UI

- Layout indicator in internal bar
- Active window title with truncation rules
- Workspace activity markers (empty/occupied/urgent)
- Clickable bar regions (left/right click to change workspace, scroll to cycle)
- Optional per-monitor bar toggle
- External bar integration hooks (scriptable output)
- Optional "mode" indicator (when binding mode is active)

---

## EWMH/ICCCM Completeness

- Optional sticky windows (with clear UX constraints); see [COMPLIANCE.md](COMPLIANCE.md#_net_wm_state) for current implementation
- Optional maximize states (if layout supports)
- Better _NET_WM_STATE adherence for above/below
- Client-initiated move/resize for tiled windows (ignored vs. policy)
- Track and expose per-window capabilities in EWMH allowed actions

---

## IPC & Extensibility

- `lwmctl` CLI (query state, switch workspace, move window, etc.)
- Simple IPC over Unix socket or X11 client messages
- Config reload without restart
- Hooks: pre/post workspace switch, window manage/unmanage
- Remote scriptability (drive layout/workspace/window actions externally)
- Optional plugin interface for advanced features

---

## Multi-Monitor & Hotplug

- Preserve workspace assignment across hotplug events
- Keep "previous workspace" per monitor across hotplug
- Per-monitor layout ratio and padding
- Prefer pointer monitor for new windows (existing behavior, make optional)

---

## Quality & Safety

- Add unit tests for layout slot mapping
- Add tests for drag-reorder edge cases (cross-monitor, empty target)
- More deterministic tests around focus state changes
- Logging levels per subsystem

---

## Stretch / Experimental Ideas

- Animated transitions (opt-in, minimal)
- "Overview" mode with all workspaces on a monitor
- Live layout preview ghost while dragging
- Adaptive gap/border based on window count
- "Swallow" feature (terminal swallows child GUI)
- Hot-reloadable config (no restart, immediate apply)

---

## Open Questions / Decisions Needed

1) Tag-like multi-assignment or strict workspaces?
2) True dynamic layouts (params per workspace) or global?
3) Should tiled windows respond to client move/resize requests?
4) Per-monitor focus-follows-pointer configuration?

---

## Next Iteration Ideas

- Rank backlog by value and complexity
- Decide on IPC mechanism (X11 client messages vs. Unix socket)
- Pick 2-3 near-term features that align with simplicity

---

## Sources Consulted

- i3 user guide: https://i3wm.org/docs/userguide.html
- bspwm README: https://github.com/baskerville/bspwm
- Hyprland README: https://github.com/hyprwm/Hyprland
- xmonad README: https://github.com/xmonad/xmonad
- Qtile README: https://github.com/qtile/qtile
- river README: https://github.com/riverwm/river
