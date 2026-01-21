# Agent 4: switch_workspace Analysis

## Executive Summary

Analyzed the switch_workspace and toggle_workspace functions. Found that the operation sequence is well-ordered with proper guards against redundant switches.

## toggle_workspace Function (wm_workspace.cpp:30-42)

```cpp
void WindowManager::toggle_workspace()
{
    auto& monitor = focused_monitor();
    size_t workspace_count = monitor.workspaces.size();
    if (workspace_count <= 1)
        return;

    size_t target = monitor.previous_workspace;
    if (target >= workspace_count || target == monitor.current_workspace)
        return;

    switch_workspace(static_cast<int>(target));
}
```

## switch_workspace Operation Sequence (wm_workspace.cpp:7-28)

1. `workspace_policy::apply_workspace_switch(monitor, ws)` - validates and updates state
2. Iterate old_workspace.windows → `wm_unmap_window(window)` for non-sticky windows
3. `update_ewmh_current_desktop()` - EWMH property update
4. `rearrange_monitor(monitor)` - layout visible windows
5. `update_floating_visibility(focused_monitor_)` - handle floating windows
6. `focus_or_fallback(monitor)` - select and set focus
7. `update_all_bars()` - update status bars
8. `conn_.flush()` - send all pending X11 requests

## Guards Against Redundant Switches

### Guard 1: apply_workspace_switch (policy.hpp:160-172)
```cpp
if (target == monitor.current_workspace)
    return std::nullopt;  // Reject identical target
```

### Guard 2: toggle_workspace (wm_workspace.cpp:38-39)
```cpp
if (target >= workspace_count || target == monitor.current_workspace)
    return;  // Reject if previous == current
```

## Previous Workspace Tracking - Safe from Loops

Only TWO workspaces involved: current and previous. State machine:
```
WS_A ⇄ WS_B ⇄ WS_C only creates pairs, not cycles
```

Example:
```
Start: current=0, previous=0
Switch to 1: current=1, previous=0
Toggle: current=0, previous=1
Toggle: current=1, previous=0  ← Oscillation, not infinite loop
```

## The Actual Flickering Mechanism (Before Fix)

With auto-repeat causing rapid A→B→A→B toggling:
- Each toggle succeeds because `target ≠ monitor.current_workspace`
- Previous and current swap on each call
- Result: visible flicker as windows appear/disappear rapidly

## Fix Assessment

The previous 150ms debounce was a **symptom-level fix** (suppress effect).
The key release tracking is a **root-cause fix** (prevent cause).

## No Redundant Operations Found

- Window operations are idempotent
- Guards prevent same-workspace switches
- Layout only called once per switch_workspace
