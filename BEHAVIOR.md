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
│   └── ... (10 workspaces total)
├── Monitor 1 (e.g., HDMI-0)
│   ├── Workspace 0
│   ├── Workspace 1 (current)
│   │   └── Window D (focused)
│   └── ...
└── Dock Windows (Polybar, etc.) - not managed, just tracked for struts
```

- Each monitor has **10 independent workspaces** (0-9)
- Each workspace has its own **window list** and **focused window**
- One monitor is the **focused monitor** at any time
- EWMH desktop index = `monitor_idx * 10 + workspace_idx`

---

## Focus Behavior

### What Gets Focus
| Event | Result |
|-------|--------|
| Mouse enters window | Window gets focus (focus-follows-mouse) |
| Mouse enters different monitor (even empty) | Focus moves to that monitor |
| Click on empty monitor area | Focus moves to that monitor |
| New window opens | New window gets focus (on focused monitor) |
| Focused window closes | Last window in workspace gets focus, or none |
| Switch workspace | Previously focused window on that workspace gets focus |
| Switch monitor | Focused window on target monitor's current workspace gets focus |

### Focus Tracking
- **Per-workspace**: Each workspace remembers its `focused_window`
- **Per-monitor**: `focused_monitor_` tracks which monitor is active
- **Global**: `_NET_ACTIVE_WINDOW` reflects the currently focused window
- **Visual**: Focused window has colored border; all others have black border

### Focus Rules
1. Only ONE window has focus globally at any time
2. Focus can only be on a VISIBLE window (current workspace of any monitor)
3. If focus target is on different workspace, switch to that workspace first

### Monitor Focus Semantics
This is the key behavior for multi-monitor setups:

1. **Mouse in gaps on SAME monitor**: Focus stays on current window
   - Moving to wallpaper/gaps within the same monitor doesn't change focus
   - The previously focused window retains focus and visual indication

2. **Mouse crosses to DIFFERENT monitor**: Focus moves immediately
   - Even if the target monitor is empty (no windows)
   - Previous window loses focus (border becomes black)
   - New windows will spawn on the newly focused monitor

3. **Click on empty area**: Focus moves to that monitor
   - Clicking anywhere on the root window updates monitor focus
   - Useful when entering an empty monitor without crossing from a window

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
- Monitors are independent, each with their own 10 workspaces
- Monitors are sorted left-to-right by X coordinate
- Focus can be on any monitor

### Switching Monitor Focus (Super+Left/Right)
1. Change `focused_monitor` to adjacent monitor (wraps around)
2. Warp cursor to center of new monitor
3. Focus the new monitor's current workspace's focused window

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
Is dialog/menu/etc? → Map without managing, DONE
    ↓
Add to focused monitor's current workspace
    ↓
Map and arrange
    ↓
Focus the new window
```

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

### Client Messages Handled
| Message | Action |
|---------|--------|
| `_NET_CURRENT_DESKTOP` | Switch to requested desktop (Polybar clicks) |
| `_NET_ACTIVE_WINDOW` | Focus requested window, switch workspace if needed |

---

## Keybindings

### Default Actions
| Key | Action |
|-----|--------|
| Super+Return | Launch terminal |
| Super+d | Launch launcher (rofi) |
| Super+q | Kill focused window |
| Super+1-9 | Switch to workspace N on focused monitor |
| Super+Shift+1-9 | Move focused window to workspace N |
| Super+Left/Right | Focus adjacent monitor |
| Super+Shift+Left/Right | Move window to adjacent monitor |

---

## Open Questions / Design Decisions

1. **Focus-follows-mouse vs click-to-focus**: Currently uses focus-follows-mouse. Change?

2. **Window placement on monitor change**: Currently ALL windows move to first monitor. Preserve placement where possible?

3. **Workspace naming**: Currently `MonitorName:N`. Custom names?

4. **Floating windows**: Dialogs float but aren't tracked. Should they be?

5. **Fullscreen**: Not implemented. Add `_NET_WM_STATE_FULLSCREEN` support?
