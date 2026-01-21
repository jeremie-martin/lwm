# Agent 2: Flickering Source Search

## Executive Summary

Searched for potential sources of visual flickering beyond the key auto-repeat issue. Found 7 significant sources that could contribute to flickering.

## Critical Findings

### Finding 1: Potential Double Layout Execution (HIGH SEVERITY)

**Location:** wm_focus.cpp lines 30-40 and wm_workspace.cpp line 23

A single `toggle_workspace()` call can trigger `layout.arrange()` twice:
1. First: `switch_workspace()` → `rearrange_monitor()` → `layout.arrange()`
2. Second: `focus_or_fallback()` → `focus_window()` → conditional `rearrange_monitor()`

**Impact:** Windows configured, mapped, then immediately reconfigured creates visible flicker.

### Finding 2: Multiple XCB Flush Calls (MEDIUM SEVERITY)

Three separate `conn_.flush()` calls execute within a single toggle:
1. Inside `layout_.arrange()` after mapping windows
2. Inside `focus_window()` after focus operations
3. At end of `switch_workspace()`

**Impact:** Three distinct refresh cycles instead of one atomic update.

### Finding 3: Window State Thrashing (MEDIUM SEVERITY)

Windows transition through multiple states rapidly:
1. MAPPED (old workspace)
2. UNMAPPED
3. CONFIGURED but unmapped
4. MAPPED (new workspace)
5. Potentially RECONFIGURED again

### Finding 4: Synthetic ConfigureNotify Event Storm (MEDIUM SEVERITY)

Each window reconfigured receives:
- Real XCB configure operation
- Synthetic ConfigureNotify event

For N windows: N×2 geometry change notifications in rapid succession.

### Finding 5: Layout Two-Phase Structure (MEDIUM SEVERITY)

```cpp
// Phase 1: Configure all windows
for (window : windows) configure_window(window);

// Phase 2: Map all windows
for (window : windows) xcb_map_window(window);

conn_.flush();
```

Between phases, windows are configured but unmapped, potentially causing artifacts.

### Finding 6: Multiple update_all_bars() Calls (LOW-MEDIUM)

Bar updates called multiple times during single toggle.

### Finding 7: EWMH Property Updates (LOW)

Property updates generate additional events adding to event stream.

## Root Cause Assessment

**Most probable cause of flickering:** Double-layout scenario combined with three separate flush points creating multiple refresh cycles.

## Key Question for Phase 2

Can `change->workspace_changed` be true when `focus_window()` is called from `focus_or_fallback()` after we've already switched workspaces?
