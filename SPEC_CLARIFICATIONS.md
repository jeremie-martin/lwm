# Spec Clarifications

This document records explicit design decisions where EWMH/ICCCM specifications 
allowed for interpretation, or where BEHAVIOR.md/COMPLIANCE.md needed clarification.

---

## 1. `_NET_CLIENT_LIST` and Dock Windows

**Issue**: BEHAVIOR.md stated docks are "not included in `_NET_CLIENT_LIST`", but 
EWMH defines `_NET_CLIENT_LIST` as containing **all windows managed by the WM**.

**Decision**: **Include dock windows in `_NET_CLIENT_LIST`** for protocol conformance.

**Rationale**:
- EWMH specification clearly states `_NET_CLIENT_LIST` should contain all managed windows
- Docks are managed windows (we track them, handle their struts, respond to their events)
- Docks are marked with `_NET_WM_STATE_SKIP_TASKBAR` and `_NET_WM_STATE_SKIP_PAGER`
- Taskbars/pagers should use these state atoms to filter, not rely on exclusion from the list
- This maintains protocol compliance while achieving the same practical effect

**Implementation**:
- Dock windows ARE added to `clients_` registry (with `Kind::Dock`)
- Dock windows ARE included in `_NET_CLIENT_LIST` and `_NET_CLIENT_LIST_STACKING`
- Dock windows have `skip_taskbar=true` and `skip_pager=true` by default
- Desktop windows (`_NET_WM_WINDOW_TYPE_DESKTOP`) follow the same pattern

---

## 2. `_NET_CLIENT_LIST` Update Timing

**Issue**: COMPLIANCE.md stated "Update on map/unmap" which could be interpreted as 
workspace visibility toggling (which causes map/unmap operations).

**Decision**: **Update `_NET_CLIENT_LIST` on manage/unmanage, NOT on visibility changes.**

**Rationale**:
- EWMH defines `_NET_CLIENT_LIST` as the list of managed windows
- A window hidden on another workspace is still managed
- Workspace switching uses `wm_unmap_window()` which tracks WM-initiated unmaps
- Only client-initiated unmaps (withdraw requests) trigger unmanagement
- This prevents unnecessary churn of `_NET_CLIENT_LIST` on workspace switches

**Implementation**:
- `update_ewmh_client_list()` is called from `manage_window()`, `unmanage_window()`,
  `manage_floating_window()`, `unmanage_floating_window()`
- NOT called from `switch_workspace()` or visibility toggle operations
- The ICCCM unmap tracking (`wm_unmapped_windows_`) distinguishes WM vs client unmaps

---

## 3. Sticky Window Scope

**Issue**: Traditional WMs make sticky windows visible on all desktops globally.

**Decision**: **Sticky windows are visible on all workspaces of their owning monitor only.**

**Rationale**:
- Per BEHAVIOR.md §1.5, sticky scope is per-monitor
- This matches the per-monitor workspace model (each monitor has independent workspaces)
- A sticky window on monitor A does NOT appear on monitor B
- `_NET_WM_DESKTOP = 0xFFFFFFFF` still indicates sticky per EWMH

**Implementation**:
- `is_window_visible()` checks sticky flag for visibility within the owning monitor
- `set_window_sticky()` sets the flag and updates `_NET_WM_DESKTOP` to `0xFFFFFFFF`
- Focus on a sticky window does NOT switch workspaces (confirmed in `focus_window_state()`)

---

## 4. Popup/Ephemeral Window Handling

**Issue**: How to handle `_NET_WM_WINDOW_TYPE_TOOLTIP`, `_NET_WM_WINDOW_TYPE_NOTIFICATION`, 
`_NET_WM_WINDOW_TYPE_POPUP_MENU`, `_NET_WM_WINDOW_TYPE_DROPDOWN_MENU`, etc.

**Decision**: **Map popups directly without full management.**

**Rationale**:
- These windows are short-lived and application-controlled
- They should not participate in tiling, workspace membership, or focus tracking
- They are not added to `_NET_CLIENT_LIST`
- The application is responsible for positioning and dismissing them

**Implementation**:
- In `handle_map_request()`, popup types are detected and mapped without management
- They do not enter `clients_` registry
- They do not receive focus automatically

---

## 5. Desktop Window Handling

**Issue**: How should `_NET_WM_WINDOW_TYPE_DESKTOP` windows be managed?

**Decision**: **Desktop windows are managed but excluded from normal workflow.**

**Rationale**:
- Desktop windows (like desktop backgrounds/file managers) are managed windows
- They should be below all other windows at all times
- They should not be focus-eligible
- They should appear on all workspaces

**Implementation**:
- Desktop windows are added to `desktop_windows_` list and `clients_` registry
- They are included in `_NET_CLIENT_LIST` (per EWMH)
- They have `skip_taskbar=true` and `skip_pager=true`
- They are stacked at the bottom (`XCB_STACK_MODE_BELOW`)
- They are not focus-eligible (`is_focus_eligible()` returns false)

---

## 6. WM_STATE and Workspace Visibility

**Issue**: Should `WM_STATE` change when hiding windows for workspace switches?

**Decision**: **WM_STATE is NOT changed for workspace visibility.**

**Rationale**:
- Per ICCCM, `WM_STATE=NormalState` means the window is managed and logically visible
- A window on a non-visible workspace is still "normal" - just not currently displayed
- Only explicit iconification (minimize) changes `WM_STATE` to `IconicState`
- Workspace visibility is expressed via `_NET_WM_DESKTOP` and physical mapping state

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
- They inherit workspace from their parent
- They stack above their parent when both are visible
- This behavior matches common WM conventions

**Implementation**:
- In `manage_floating_window()`, transient windows get `skip_taskbar` and `skip_pager` set
- Workspace inheritance is handled via `resolve_window_desktop()` checking parent
- Stacking is handled via `restack_transients()`

---

## Version History

- 2026-01-18: Initial document created during production-ready implementation work
