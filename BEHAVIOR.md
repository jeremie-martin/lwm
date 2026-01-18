# LWM Behavior Specification

This document defines the **high-level, user-visible behavior** of LWM, independent of implementation.
Protocol compliance obligations (ICCCM/EWMH, property/message semantics) are defined in COMPLIANCE.md
and are not duplicated here.

---

## 1. Concepts and Model

### 1.1 Monitors
- LWM manages one or more monitors (physical displays).
- Each monitor has its own independent set of workspaces.
- Exactly one monitor is the **active monitor** at any time.

### 1.2 Workspaces
- Each monitor has N workspaces (configurable; commonly 0–9).
- Exactly **one workspace per monitor is visible** at a time.
- Each workspace maintains:
  - an ordered list of **tiled** windows (the tiling layout order)
  - a remembered "last-focused" tiled window (if any)
- **Floating windows** are tracked separately at the window manager level:
  - They are associated with a monitor and workspace but stored in a separate list.
  - Workspace focus memory tracks only tiled windows; if the last-focused window was floating,
    focus restoration may fall back to a tiled window or none.
- **Implementation Note**: All window state (fullscreen, iconic, sticky, above, below, maximized,
  shaded, modal) is stored in a unified `Client` record. The tiled/floating distinction affects
  only layout participation and workspace storage, not state management.

### 1.3 Window Classes (Behavioral)
Once classified, windows behave as follows:
- **Tiled**: participates in the workspace tiling layout.
- **Floating**: positioned independently; does not affect the tiling layout.
- **Panels/Docks** (`_NET_WM_WINDOW_TYPE_DOCK`):
  - Reserve screen edges via struts (reduce usable area for tiling).
  - Included in `_NET_CLIENT_LIST` per EWMH (with `_NET_WM_STATE_SKIP_TASKBAR/PAGER`).
  - Do not participate in workspace membership or normal focus.
  - Always visible across all workspaces (effectively sticky).
  - Stacked above normal windows but below fullscreen.
  - See SPEC_CLARIFICATIONS.md for rationale.
- **Desktop Windows** (`_NET_WM_WINDOW_TYPE_DESKTOP`):
  - Positioned below all other windows.
  - Not focus-eligible; do not participate in workspace membership.
- **Ephemeral Popups** (menus/tooltips/notifications): may be displayed but do not behave like
  normal managed windows (do not participate in tiling, workspace membership, or normal focus).

(Exact classification signals are defined in COMPLIANCE.md.)

### 1.4 Visibility
- A window is **visible** iff it belongs to the monitor's currently visible workspace,
  except for special visibility rules (see §1.5 Sticky Windows).
- Only visible windows may be focused.

### 1.5 Sticky Windows
**Sticky windows** have special visibility rules:
- A sticky window is **visible on all workspaces of its owning monitor** (not globally across all monitors).
- Sticky windows are included in layout regardless of which workspace is currently active on their monitor.
- **Focusing a sticky window does NOT switch workspaces**. The current workspace remains unchanged.
- Sticky state is indicated by:
  - `_NET_WM_STATE_STICKY` in the window's state atoms.
  - `_NET_WM_DESKTOP = 0xFFFFFFFF` (per EWMH specification).
- Sticky scope is per-monitor: a sticky window on monitor A is NOT visible on monitor B.

### 1.6 EWMH Desktop Mapping (Per-Monitor Workspaces)
LWM uses per-monitor workspaces and maps them into EWMH desktops as follows:
- `_NET_NUMBER_OF_DESKTOPS = monitors * workspaces_per_monitor`.
- Desktop index = `monitor_index * workspaces_per_monitor + workspace_index`.
- `_NET_CURRENT_DESKTOP` reflects the **active monitor’s** current workspace only.
- `_NET_DESKTOP_VIEWPORT` repeats each monitor’s origin per workspace slot.
This mapping is intentionally per-monitor; some pagers may assume global desktops.

---

## 2. Focus and Active Monitor Policy

### 2.1 Global Focus Invariant
- At most one window is focused globally at any time (or none).
- Focus is only assigned to **visible, focus-eligible** windows (as constrained by COMPLIANCE.md).

### 2.2 Focus-Follows-Mouse (FFM)
- When the pointer enters a focus-eligible window, that window becomes focused.
- **Motion within a window**: If the pointer moves within a focus-eligible window that is not
  currently focused, that window gains focus. This handles the case where a new window took
  focus (per §5.2), but the cursor remained in a different window.
- **Click within a window**: Clicking within a focus-eligible window focuses it, even without
  modifier keys.

### 2.3 Empty Space Semantics (Key Multi-Monitor Behavior)
LWM defines focus behavior for pointer movement over “gaps/background” as follows:

1) **Empty space on the same monitor**
- Moving the pointer onto empty space (background/gaps) on the **same monitor** does **not**
  change focus.
- The previously focused window remains focused.

2) **Crossing to a different monitor**
- When the pointer crosses onto a **different monitor**, that monitor becomes the **active monitor**.

3) **Empty space on a different monitor**
- If the pointer is on empty space of the **other** monitor, global focus is **cleared**
  (no window is focused).

4) **Entering a window on the other monitor**
- As soon as the pointer enters any focus-eligible window on that monitor, that window becomes focused.

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
- Exceptions are permitted when required by COMPLIANCE.md (e.g., initial iconic state, focus-ineligible
  windows, or focus-stealing prevention constraints).
- **Interaction with FFM**: When a new window takes focus, if the cursor is physically inside
  another window, that other window regains focus as soon as the user moves or clicks within it
  (see §2.2 Focus-Follows-Mouse).

---

## 6. Floating Windows

### 6.1 Association and Visibility
- Floating windows are associated with a monitor and workspace, and appear/disappear with that workspace.
- Unlike tiled windows (stored per-workspace), floating windows are tracked in a separate global list
  with explicit monitor/workspace association. This affects focus restoration (see §1.2).

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

## 9. Non-Goals for This Document
- Protocol-level property/message requirements (COMPLIANCE.md).
- Internal data structures, event names, or flowcharts tied to X11 mechanics.
- Exact keybinding tables (configuration-level documentation).
- Hardcoded visuals (borders/colors), unless explicitly part of the product spec.
