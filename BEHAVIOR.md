# LWM Behavior Specification

This document describes what LWM does. Use it to verify implementation matches intent.

---

## Core Model

```
LWM
├── Monitor 0 (e.g., DP-0)
│   ├── Workspace 0 (current)
│   │   ├── Window A (focused)
│   │   └── Window B
│   ├── Workspace 1
│   │   └── Window C
│   └── ... (configurable workspaces total)
├── Monitor 1 (e.g., HDMI-0)
│   ├── Workspace 0
│   ├── Workspace 1 (current)
│   │   └── Window D (focused)
│   └── ...
└── Dock Windows (Polybar, etc.) - not managed, just tracked for struts
```

- Each monitor has **configurable independent workspaces** (default 0-9)
- Each workspace has its own **window list** and **remembered focused window** (last focused)
- One monitor is the **active monitor** at any time (drives placement and workspace actions)
- EWMH desktop index = `monitor_idx * workspaces_per_monitor + workspace_idx`

---

## Focus Behavior

### What Gets Focus
| Event | Result |
|-------|--------|
| Mouse enters managed window | Window gets focus (focus-follows-mouse) and its monitor becomes active |
| Mouse moves onto empty area on same monitor | Focus unchanged |
| Mouse moves onto empty area on different monitor | Active monitor changes; focused window is cleared |
| Click on empty area on a different monitor | Active monitor updates; no window is focused |
| New window opens | New window gets focus on the active monitor |
| Focused window closes | Last window in workspace gets focus, or none |
| Switch workspace | Previously focused window on that workspace gets focus |
| Switch monitor | Remembered focused window on target monitor's current workspace gets focus |

### Focus Tracking
- **Per-workspace**: Each workspace remembers its last focused window
- **Per-monitor**: The active monitor is always defined
- **Global**: `_NET_ACTIVE_WINDOW` reflects the focused window (or none)
- **Placement**: New windows are placed on the active monitor; pointer only updates monitor activity
- **Visual**: Focused window has colored border; all others have black border

### Focus Rules
1. Only ONE window has focus globally at any time (or none)
2. Focus can only be on a VISIBLE window (current workspace of any monitor)
3. If focus target is on different workspace, switch to that workspace first
4. Active monitor is independent of focus and never ambiguous

### Monitor Focus Semantics
This is the key behavior for multi-monitor setups:

1. **Mouse in gaps on SAME monitor**: Focus stays on current window
   - Moving to wallpaper/gaps within the same monitor doesn't change focus
   - The previously focused window retains focus and visual indication

2. **Mouse crosses to DIFFERENT monitor**: Active monitor updates immediately
   - This happens on pointer movement alone (no click required)
   - New windows will spawn on the newly active monitor

3. **Mouse on empty area of DIFFERENT monitor**: No window is focused
   - The previously focused window loses focus (border becomes black)
   - This is true even if the target monitor has windows

4. **Mouse enters a window on another monitor**: That window is focused immediately
   - The monitor is already active from (2), but focus must still be set

---

## Floating Windows

### Managed Floating Windows
LWM manages floating windows for:
- Dialogs, utility windows, and splash windows
- Any window with `WM_TRANSIENT_FOR` (e.g., file pickers)

Menus, tooltips, notifications, and other short-lived popups are mapped but not managed.

### Placement
- Floating windows appear on the **same monitor** as their transient parent if one exists
- Otherwise, they appear on the **active monitor**
- Initial placement centers the window within the monitor working area (or over the parent)
- Client configure requests for floating windows are honored

### Focus
- Floating windows are focusable (focus-follows-mouse)
- Focusing a floating window updates the active monitor
- Closing a floating window restores focus to the workspace's remembered window or another floating window

### Workspace and Monitor Association
- Floating windows belong to a monitor and workspace
- They are mapped/unmapped when that monitor switches workspaces
- Moving a floating window across monitors reassigns it to the new monitor and its current workspace

### Move/Resize
- **Super + Left Click drag** → move floating window
- **Super + Right Click drag** → resize floating window

---

## Workspace Behavior

### Visibility
- Only ONE workspace per monitor is visible at a time
- Windows on non-visible workspaces are unmapped (hidden)

### Switching Workspaces (Super+1-9)
1. If already on target workspace → do nothing
2. Unmap all windows on current workspace
3. Change `current_workspace` to target
4. Map and arrange windows on new workspace
5. Focus the workspace's remembered `focused_window`

### Moving Window to Workspace (Super+Shift+1-9)
1. Remove window from current workspace
2. Add window to target workspace
3. Unmap the window (it's now on a hidden workspace)
4. Focus next window in current workspace

---

## Monitor Behavior

### Multi-Monitor Model
- Monitors are independent, each with their own configurable workspaces
- Monitors are sorted left-to-right by X coordinate
- The active monitor can be any monitor

### Switching Active Monitor (Super+Left/Right)
1. Change the active monitor to the adjacent monitor (wraps around)
2. Warp cursor to center of new monitor
3. Focus the new monitor's current workspace's remembered focused window

### Moving Window to Monitor (Super+Shift+Left/Right)
1. Remove window from source monitor's current workspace
2. Add window to target monitor's current workspace
3. Rearrange both monitors
4. Focus next window on source monitor

---

## Window Lifecycle

### New Window (MapRequest)
```
MapRequest received
    ↓
Is dock window? → Track in dock_windows_, map, update struts, DONE
    ↓
Is managed floating? → Track as floating, place, map, focus, DONE
    ↓
Is popup/menu/etc? → Map without managing, DONE
    ↓
Add to active monitor's current workspace
    ↓
Map and arrange
    ↓
Focus the new window
```

### Startup Window Adoption
On startup, LWM scans existing mapped windows and manages them using the same rules as MapRequest:
1. Docks are tracked for struts
2. Floating/transient windows are managed as floating
3. Normal windows are added to the active monitor’s current workspace

Focus is resolved after adoption based on the pointer’s current monitor.

### Window Destroyed (DestroyNotify) or Client-Initiated Unmap
```
DestroyNotify or client-initiated UnmapNotify received
    ↓
Find window in any workspace (searches ALL)
    ↓
Remove from workspace
    ↓
If was focused → focus next window in workspace
    ↓
Rearrange monitor
    ↓
Update _NET_CLIENT_LIST
```

---

## Event Handling Model

### UnmapNotify vs DestroyNotify

The WM must distinguish between unmaps it initiated (workspace switching) and client-initiated unmaps (app hiding itself).

| Event | Source | WM Response |
|-------|--------|-------------|
| `UnmapNotify` (WM-initiated) | WM switched workspace | **Ignore** - window is just hidden |
| `UnmapNotify` (client-initiated) | App hiding itself | Remove from workspace |
| `DestroyNotify` | App closing | **Always** remove from workspace |

### Implementation

The WM tracks windows it has unmapped in `wm_unmapped_windows_`. When an `UnmapNotify` arrives:
- If window is in the tracking set → WM-initiated, ignore
- If window is NOT in tracking set → client-initiated, remove window

When windows become visible again (via `rearrange_monitor`), they are removed from the tracking set.

### Iconify / Deiconify (ICCCM)
- `WM_CHANGE_STATE` with `IconicState` requests that a window be iconified
- Iconified windows are unmapped but remain tracked in their workspace
- Deiconify (MapRequest) restores the window and its layout position
- `WM_STATE` is maintained as `NormalState` or `IconicState`

### Window Lifecycle
```
                    ┌─────────────────────────────┐
                    │                             │
                    ▼                             │
Mapped (visible) ───→ Unmapped by WM (hidden) ───┘
        │              (still tracked in workspace)
        │
        │ DestroyNotify or
        │ Client UnmapNotify
        ▼
    REMOVED from workspace

---

## Layout

### Tiling Algorithm (Master-Stack)
| Windows | Layout |
|---------|--------|
| 1 | Full screen (with padding) |
| 2 | Side-by-side, equal width |
| 3+ | Master on left (50%), stack on right (50%, divided equally) |

### Working Area
- Full monitor geometry minus struts (reserved space for docks)
- Polybar reserves top strut → windows tile below it

---

## EWMH Integration

### Properties Maintained
| Property | When Updated |
|----------|--------------|
| `_NET_CURRENT_DESKTOP` | Workspace or monitor switch |
| `_NET_ACTIVE_WINDOW` | Focus change |
| `_NET_CLIENT_LIST` | Window add/remove |
| `_NET_WM_DESKTOP` | Window created or moved |
| `_NET_NUMBER_OF_DESKTOPS` | Startup, monitor change |
| `_NET_WM_STATE` | Fullscreen/above/hidden updates |

### Client Messages Handled
| Message | Action |
|---------|--------|
| `_NET_CURRENT_DESKTOP` | Switch to requested desktop (Polybar clicks) |
| `_NET_ACTIVE_WINDOW` | Focus requested window, switch workspace if needed |
| `_NET_WM_STATE` | Toggle fullscreen/above state |

---

## Keybindings

### Default Actions
| Key | Action |
|-----|--------|
| Super+Return | Launch terminal |
| Super+d | Launch launcher (rofi) |
| Super+q | Kill focused window |
| Super+1-9 | Switch to workspace N on active monitor (default) |
| Super+Shift+1-9 | Move focused window to workspace N (default) |
| Super+Left/Right | Focus adjacent monitor |
| Super+Shift+Left/Right | Move window to adjacent monitor |
| Super+Left Click (drag) | Move floating window |
| Super+Right Click (drag) | Resize floating window |

---

## Open Questions / Design Decisions

1. **Focus-follows-mouse vs click-to-focus**: Currently uses focus-follows-mouse. Change?

2. **Window placement on monitor change**: Currently ALL windows move to first monitor. Preserve placement where possible?

3. **Workspace naming**: Currently `MonitorName:N`. Custom names?

4. **Floating windows**: Dialogs float but aren't tracked. Should they be?

5. **Fullscreen**: Not implemented. Add `_NET_WM_STATE_FULLSCREEN` support?
