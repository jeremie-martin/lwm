# LWM Behavior Specification

> **Documentation Navigation**
> - Previous: [README.md](README.md) (Quick start)
> - Related: [CLAUDE.md](CLAUDE.md) (Development guide) | [COMPLIANCE.md](COMPLIANCE.md) (Protocol requirements)

This document defines the **high-level, user-visible behavior** of LWM, independent of implementation.
Protocol compliance obligations (ICCCM/EWMH, property/message semantics) are defined in [COMPLIANCE.md](COMPLIANCE.md)
and are not duplicated here.

---

## 1. Concepts and Model

### 1.1 Monitors
- LWM manages one or more monitors (physical displays).
- Each monitor has its own independent set of workspaces.
- Exactly one monitor is the **active monitor** at any time.

### 1.2 Workspaces

- Each monitor has N workspaces (configurable; commonly 0-9).
- Exactly **one workspace per monitor is visible** at a time.
- Each workspace maintains:
  - an ordered list of **tiled** windows (tiling layout order)
  - a remembered "last-focused" tiled window (if any)
- **Floating windows** are tracked globally with explicit monitor/workspace association. Focus restoration searches floating windows in MRU order (workspace focus memory tracks only tiled windows).

**Implementation Note**: All window state (fullscreen, iconic, sticky, above, below, maximized, shaded, modal) is stored in a unified `Client` record. The tiled/floating distinction affects only layout participation and workspace storage.

### 1.3 Window Classes (Behavioral)

- **Tiled**: participates in the workspace tiling layout.
- **Floating**: positioned independently; does not affect the tiling layout.
- **Panels/Docks** (`_NET_WM_WINDOW_TYPE_DOCK`):
  - Reserve screen edges via struts (reduce usable area for tiling).
  - Included in `_NET_CLIENT_LIST` per EWMH (with `_NET_WM_STATE_SKIP_TASKBAR/PAGER`).
  - Do not participate in workspace membership or normal focus.
  - Always visible across all workspaces (effectively sticky).
  - Stacked above normal windows but below fullscreen.
- **Desktop Windows** (`_NET_WM_WINDOW_TYPE_DESKTOP`):
  - Positioned below all other windows.
  - Not focus-eligible; do not participate in workspace membership.
- **Ephemeral Popups** (menus/tooltips/notifications): mapped directly without full management; do not participate in tiling, workspace membership, or normal focus.

(Exact classification signals are defined in [COMPLIANCE.md](COMPLIANCE.md#_net_wm_window_type); see also [SPEC_CLARIFICATIONS.md](SPEC_CLARIFICATIONS.md#4-popephemeral-window-handling).)

### 1.4 Visibility
- A window is **visible** iff it belongs to the monitor's currently visible workspace, except for special visibility rules (see §1.5 Sticky Windows).
- Only visible windows may be focused.

### 1.5 Sticky Windows

**Sticky windows** have special visibility rules:
- Visible on all workspaces of their owning monitor (not globally across monitors).
- Included in layout regardless of which workspace is currently active.
- **Focusing a sticky window does NOT switch workspaces** (current workspace remains unchanged).
- Sticky state is indicated by `_NET_WM_STATE_STICKY` and `_NET_WM_DESKTOP = 0xFFFFFFFF` (per EWMH).

See [SPEC_CLARIFICATIONS.md](SPEC_CLARIFICATIONS.md#3-sticky-window-scope) for design rationale on per-monitor scope.

### 1.6 EWMH Desktop Mapping (Per-Monitor Workspaces)
LWM uses per-monitor workspaces mapped to EWMH desktops:
- `_NET_NUMBER_OF_DESKTOPS = monitors * workspaces_per_monitor`
- Desktop index = `monitor_index * workspaces_per_monitor + workspace_index`
- `_NET_CURRENT_DESKTOP` reflects the **active monitor's** current workspace only
- `_NET_DESKTOP_VIEWPORT` repeats each monitor's origin per workspace slot

This mapping is intentionally per-monitor; some pagers may assume global desktops.

---

## 2. Focus and Active Monitor Policy

### 2.1 Global Focus Invariant
- At most one window is focused globally at any time (or none).
- Focus is only assigned to **visible, focus-eligible** windows (as constrained by COMPLIANCE.md).

### 2.2 Focus-Follows-Mouse (FFM)
- When the pointer enters a focus-eligible window, that window becomes focused.
- **Motion within a window**: If the pointer moves within a focus-eligible window that is not currently focused, that window gains focus. This handles cases where a new window took focus (per §5.2) while the cursor remained in a different window.
- **Click within a window**: Clicking a focus-eligible window focuses it.

See [COMPLETE_STATE_MACHINE.md](COMPLETE_STATE_MACHINE.md#focus-policy) for implementation details on focus eligibility and event handling.

### 2.3 Empty Space Semantics (Key Multi-Monitor Behavior)

LWM defines focus behavior for pointer movement over empty space:

1. **Empty space on the same monitor**: Focus does not change; remains on the previously focused window.
2. **Crossing to a different monitor**: The target monitor becomes the active monitor.
3. **Empty space on a different monitor**: Global focus is cleared (no window is focused).
4. **Entering a window on the other monitor**: The entered window becomes focused immediately.

### 2.4 Focus Restoration
When the focused window disappears or becomes ineligible:
- Focus moves to the workspace’s remembered last-focused window if still visible/eligible,
  otherwise to another eligible window in that workspace, otherwise to none.

When switching to a workspace:
- LWM attempts to focus that workspace’s remembered last-focused window (if eligible),
  otherwise another eligible window, otherwise none.

---

## 3. Workspace Behavior

### 3.1 Switching Workspaces
When switching the visible workspace on the active monitor:
- The requested workspace becomes visible on that monitor.
- The workspace is laid out (tiling + floating placement).
- Focus is set according to Section 2.4.

If the workspace is already visible, no change occurs.

### 3.2 Moving a Window to Another Workspace
When moving a window to another workspace:
- The window is removed from the source workspace and inserted into the destination workspace.
- If the destination workspace is not visible, the window becomes non-visible.
- Focus in the source workspace is restored according to Section 2.4.

---

## 4. Monitor Behavior

### 4.1 Switching Active Monitor (Explicit)
When explicitly switching the active monitor:
- The target monitor becomes active.
- Focus is restored on that monitor’s visible workspace according to Section 2.4.

Cursor warping is a configurable UI behavior; it is not required unless explicitly enabled.
Configuration: `[focus].warp_cursor_on_monitor_change = true`.

### 4.2 Moving a Window to Another Monitor
When moving a window to another monitor:
- The window is reassigned to the destination monitor’s currently visible workspace (default policy).
- Layout is recomputed on both involved monitors/workspaces.
- The destination monitor becomes active and the moved window is focused.

---

## 5. New Windows and Placement

### 5.1 Default Placement
When a new window appears:
- It is classified (tiled/floating/dock/popup).
- Default placement target:
  - Transient/floating windows preferentially appear with their parent (same monitor/workspace),
    otherwise on the active monitor’s visible workspace.
  - Tiled windows appear on the active monitor’s visible workspace.

### 5.2 Default Focus on New Windows

- By default, a newly created window **receives focus** if it is focus-eligible and visible.
- Exceptions are permitted when required by COMPLIANCE.md (e.g., initial iconic state, focus-ineligible windows, or focus-stealing prevention constraints).
- **Interaction with FFM**: When a new window takes focus, if the cursor is inside another window, that window regains focus on motion or click (see §2.2).

---

## 6. Floating Windows

### 6.1 Association and Visibility

- Floating windows are associated with a monitor and workspace, appearing/disappearing with that workspace.
- Unlike tiled windows (stored per-workspace), floating windows are tracked in a separate global list with explicit monitor/workspace association. This affects focus restoration (see §1.2).

### 6.2 Placement and Interaction
- Floating windows appear in a sensible default position (e.g., centered on usable area or relative
  to a parent).
- User-driven move/resize is supported (mechanism/config is not specified here).

### 6.3 Focus
- Floating windows participate in focus-follows-mouse if focus-eligible.
- Closing a floating window restores focus according to Section 2.4.
- **Note**: Per-workspace "last-focused" memory only tracks tiled windows; if focus should be restored
  to a floating window, the WM searches floating windows on that workspace in most-recently-used order.

---

## 7. Tiling Layout

### 7.1 Default Layout Behavior (Master/Stack)
- 1 window: occupies the usable area.
- 2 windows: split the usable area into two regions.
- 3+ windows: master + stack arrangement (one primary region plus a stack region shared by others).

Exact ratios/gaps are configuration concerns.

### 7.2 Usable Area
Tiling uses the monitor’s usable area excluding space reserved by panels/docks.

---

## 8. Tiled Window Drag-Reorder (Current Behavior)

LWM supports mouse-based reordering/moving of **tiled** windows without converting them to floating.

### 8.1 Interaction Model
- A drag gesture on a tiled window enters “reorder mode”.
- During drag:
  - the window follows the pointer with temporary geometry (visual feedback),
  - the tiling layout is not finalized until drop,
  - focus remains on the dragged window.

### 8.2 Drop Semantics
On drop:
- The target monitor is determined by pointer position (default: monitor under pointer).
- The window is inserted into the target monitor’s **currently visible workspace**.
- The tiling layout is recomputed.
- The dragged window remains focused (if focus-eligible and visible).

How the drop position maps to an insertion index is layout-dependent.

---

## 9. Window Rules

### 9.1 Overview
Window rules configure windows automatically based on properties (class, instance, title, type).
Rules are applied at map time (when the window first appears) to customize behavior beyond EWMH classification.

### 9.2 Rule Evaluation
- Rules are defined in configuration as an ordered list.
- **First-match-wins**: The first matching rule is applied; subsequent rules are ignored.
- An empty criteria set matches all windows.

### 9.3 Matching Criteria
All specified criteria in a rule must match (AND logic). Available criteria:
- **class**: WM_CLASS class name (regex pattern)
- **instance**: WM_CLASS instance name (regex pattern)
- **title**: Window title (regex pattern)
- **type**: EWMH window type (normal, dialog, utility, toolbar, splash, menu)
- **transient**: Match only transient (child) windows

If a criterion is not specified, it matches any value.

### 9.4 Rule Actions
When a rule matches, the following actions can be applied:
- **floating**: Force floating (true) or tiled (false) mode
- **workspace/workspace_name**: Assign to a specific workspace
- **monitor/monitor_name**: Assign to a specific monitor
- **fullscreen**: Start in fullscreen state
- **above/below**: Set stacking order
- **sticky**: Make visible on all workspaces
- **skip_taskbar/skip_pager**: Exclude from taskbar/pager
- **geometry**: Set position/size for floating windows
- **center**: Center on monitor (floating windows only)

### 9.5 Relationship to EWMH Classification
- Window rules are applied **after** EWMH classification.
- Rules cannot override fundamental window type behavior (dock, desktop, popup windows).
- Rules can override the tiled/floating decision for normal windows.
- EWMH state atoms are set according to rule actions (e.g., sticky, above, below).

For detailed EWMH window type handling and precedence, see [COMPLIANCE.md](COMPLIANCE.md#_net_wm_window_type).

---

## 10. Non-Goals for This Document
- Protocol-level property/message requirements (COMPLIANCE.md).
- Internal data structures, event names, or X11-specific mechanics.
- Exact keybinding tables (configuration-level documentation).
- Hardcoded visuals (borders/colors), unless part of the product spec.
