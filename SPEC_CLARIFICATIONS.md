# Spec Clarifications

> **Documentation Navigation**
> - Previous: [COMPLIANCE.md](COMPLIANCE.md) (Protocol requirements)
> - Related: [BEHAVIOR.md](BEHAVIOR.md) (User-facing behavior) | [COMPLETE_STATE_MACHINE.md](COMPLETE_STATE_MACHINE.md) (Implementation reference)

This document records explicit design decisions where EWMH/ICCCM specifications allowed for interpretation, or where [BEHAVIOR.md](BEHAVIOR.md) and [COMPLIANCE.md](COMPLIANCE.md) needed clarification.

---

## 1. `_NET_CLIENT_LIST` and Dock Windows

**Issue**: BEHAVIOR.md stated docks are "not included in `_NET_CLIENT_LIST`", but EWMH defines `_NET_CLIENT_LIST` as containing **all windows managed by the WM**.

**Decision**: **Include dock windows in `_NET_CLIENT_LIST`** for protocol conformance.

**Rationale**:
- EWMH specification states `_NET_CLIENT_LIST` should contain all managed windows
- Docks are managed windows (tracked, struts handled, events responded to)
- Docks marked with `_NET_WM_STATE_SKIP_TASKBAR` and `_NET_WM_STATE_SKIP_PAGER`
- Taskbars/pagers should use state atoms to filter, not rely on exclusion from list
- Maintains protocol compliance while achieving same practical effect

**Implementation**:
- Dock windows ARE added to `clients_` registry (with `Kind::Dock`)
- Dock windows ARE included in `_NET_CLIENT_LIST` and `_NET_CLIENT_LIST_STACKING`
- Dock windows have `skip_taskbar=true` and `skip_pager=true` by default
- Desktop windows (`_NET_WM_WINDOW_TYPE_DESKTOP`) follow same pattern

---

## 2. `_NET_CLIENT_LIST` Update Timing

**Issue**: COMPLIANCE.md stated "Update on map/unmap" which could be interpreted as workspace visibility toggling (causes map/unmap operations).

**Decision**: **Update `_NET_CLIENT_LIST` on manage/unmanage, NOT on visibility changes.**

**Rationale**:
- EWMH defines `_NET_CLIENT_LIST` as the list of managed windows
- Window hidden on another workspace is still managed
- Workspace switching uses `wm_unmap_window()` which tracks WM-initiated unmaps
- Only client-initiated unmaps (withdraw requests) trigger unmanagement
- Prevents unnecessary churn of `_NET_CLIENT_LIST` on workspace switches

**Implementation**:
- `update_ewmh_client_list()` called from `manage_window()`, `unmanage_window()`, `manage_floating_window()`, `unmanage_floating_window()`
- NOT called from `switch_workspace()` or visibility toggle operations
- ICCCM unmap tracking (`wm_unmapped_windows_`) distinguishes WM vs client unmaps

---

## 3. Sticky Window Scope

**Issue**: Traditional WMs make sticky windows visible on all desktops globally.

**Decision**: **Sticky windows are visible on all workspaces of their owning monitor only.**

**Rationale**:
- Per BEHAVIOR.md §1.5, sticky scope is per-monitor
- Matches per-monitor workspace model (each monitor has independent workspaces)
- Sticky window on monitor A does NOT appear on monitor B
- `_NET_WM_DESKTOP = 0xFFFFFFFF` still indicates sticky per EWMH

**Implementation**:
- `is_window_visible()` checks sticky flag for visibility within owning monitor
- `set_window_sticky()` sets flag and updates `_NET_WM_DESKTOP` to `0xFFFFFFFF`
- Focus on sticky window does NOT switch workspaces (confirmed in `focus_window_state()`)

---

## 4. Popup/Ephemeral Window Handling

**Issue**: How to handle `_NET_WM_WINDOW_TYPE_TOOLTIP`, `_NET_WM_WINDOW_TYPE_NOTIFICATION`, `_NET_WM_WINDOW_TYPE_POPUP_MENU`, `_NET_WM_WINDOW_TYPE_DROPDOWN_MENU`, etc.

**Decision**: **Map popups directly without full management.**

**Rationale**:
- Short-lived, application-controlled windows
- Should not participate in tiling, workspace membership, or focus tracking
- Not added to `_NET_CLIENT_LIST`
- Application responsible for positioning and dismissing

**Implementation**:
- In `handle_map_request()`, popup types detected and mapped without management
- Do not enter `clients_` registry
- Do not receive focus automatically

---

## 5. Desktop Window Handling

**Issue**: How should `_NET_WM_WINDOW_TYPE_DESKTOP` windows be managed?

**Decision**: **Desktop windows are managed but excluded from normal workflow.**

**Rationale**:
- Desktop windows (desktop backgrounds/file managers) are managed windows
- Should be below all other windows at all times
- Should not be focus-eligible
- Should appear on all workspaces

**Implementation**:
- Desktop windows added to `desktop_windows_` list and `clients_` registry
- Included in `_NET_CLIENT_LIST` (per EWMH)
- Have `skip_taskbar=true` and `skip_pager=true`
- Stacked at the bottom (`XCB_STACK_MODE_BELOW`)
- Not focus-eligible (`is_focus_eligible()` returns false)

---

## 6. WM_STATE and Workspace Visibility

**Issue**: Should `WM_STATE` change when hiding windows for workspace switches?

**Decision**: **WM_STATE is NOT changed for workspace visibility.**

**Rationale**:
- Per ICCCM, `WM_STATE=NormalState` means window is managed and logically visible
- Window on non-visible workspace is still "normal" - just not currently displayed
- Only explicit iconification (minimize) changes `WM_STATE` to `IconicState`
- Workspace visibility expressed via `_NET_WM_DESKTOP` and physical mapping state

**Implementation**:
- `wm_unmap_window()` unmaps without changing `WM_STATE`
- Only `iconify_window()` sets `WM_STATE=IconicState`
- `unmanage_window()` sets `WM_STATE=WithdrawnState`

---

## 7. Transient Windows and State Inheritance

**Issue**: How should transient windows be handled regarding state atoms?

**Decision**: **Transients automatically get `SKIP_TASKBAR` and `SKIP_PAGER`.**

**Rationale**:
- Per COMPLIANCE.md, transients should not clutter taskbars/pagers independently
- Inherit workspace from parent
- Stack above parent when both visible
- Matches common WM conventions

**Implementation**:
- In `manage_floating_window()`, transient windows get `skip_taskbar` and `skip_pager` set
- Workspace inheritance handled via `resolve_window_desktop()` checking parent
- Stacking handled via `restack_transients()`

---

## 8. Window Visibility: Off-Screen Positioning

**Issue**: When switching workspaces, how should windows be hidden?

**Decision**: **Use off-screen positioning (move windows to OFF_SCREEN_X = -20000).**

**Background**:

Two main approaches used by X11 window managers:

| Approach | Used By | Pros | Cons |
|----------|---------|------|------|
| **Unmap/Map** | i3, bspwm | Memory efficient, cleaner semantics | GPU renderer issues after remap |
| **Move off-screen** | lwm, dwm | No rendering issues, instant show | Higher memory usage, off-screen windows receive events |

**DWM's approach** (from `showhide()`):
```c
if (ISVISIBLE(c)) {
    XMoveWindow(dpy, c->win, c->x, c->y);      // show: normal position
} else {
    XMoveWindow(dpy, c->win, WIDTH(c) * -2, c->y);  // hide: off-screen
}
```

**Problem with Unmap/Map:**
GPU-accelerated applications (Chromium, Electron, Qt/PyQt with OpenGL) may not properly redraw after unmap/remap. GPU renderer caches framebuffers and doesn't invalidate when window is remapped at same size. This causes:
- Blank/gray windows after workspace switch
- Stale content displayed until user interaction

**Rationale for off-screen visibility**:
- No GPU rendering issues (windows stay mapped, just positioned off-screen)
- Instant visibility switching (no unmap/remap latency)
- Consistent with minimalist design (simple geometric visibility control)
- Hidden windows still appear in `_NET_CLIENT_LIST` (managed, just not visible)

**Implementation**:
- `hide_window(window)` moves window to `OFF_SCREEN_X = -20000`
- `show_window(window)` clears hidden flag; caller must restore geometry via `rearrange_monitor()` or `apply_floating_geometry()`
- Windows always mapped (`xcb_map_window()` called once on management)
- No unmap counter tracking needed (WM never calls `xcb_unmap_window()`)
- With off-screen visibility, ALL `UnmapNotify` events are client-initiated withdraw requests
- `client.hidden` flag tracks off-screen state: true = at OFF_SCREEN_X, false = at on-screen position

**Sticky windows**:

- `hide_window()` returns early for sticky windows (never moved off-screen).
- Sticky windows visible on all workspaces by design.
- Controlled via workspace tiling algorithm and layout filtering, not via visibility management.

**Window lifecycle with off-screen visibility:**
```
MapRequest → classify → manage_window() → xcb_map_window() (always)
    ↓
If iconic or not visible: hide_window() → move to OFF_SCREEN_X
    ↓
Workspace switch: hide_window() for windows leaving, show_window() + rearrange for entering
    ↓
UnmapNotify: Always client-initiated withdraw → unmanage window
```

---

## Version History

- 2026-01-24: Corrected §8 to reflect actual off-screen visibility implementation
- 2026-01-18: Initial document created during production-ready implementation work
