# Client Model Migration Note

**Date**: January 2026
**Status**: COMPLETE
**Scope**: Unified Client record as single source of truth for all per-window state

---

## Purpose

Replace scattered state stored across multiple structures with a single, authoritative `Client` struct per managed window.

**Problems solved:**
- State synchronization bugs between duplicate data structures
- O(n) lookups replaced with O(1) via `clients_` map
- Simplified invariant reasoning (all state in one place)
- Eliminated redundant fields in FloatingWindow and Window structs

---

## What Changed

### Authoritative `Client` struct (`src/lwm/core/types.hpp`)

```cpp
struct Client {
    xcb_window_t id;
    enum class Kind { Tiled, Floating, Dock, Desktop };
    Kind kind;
    size_t monitor, workspace;           // Authoritative location

    // State flags (replaces 11 unordered_sets)
    bool fullscreen, iconic, sticky, above, below;
    bool maximized_horz, maximized_vert, shaded, modal;
    bool skip_taskbar, skip_pager, demands_attention;

    // Restore geometries
    std::optional<Geometry> fullscreen_restore;
    std::optional<Geometry> maximize_restore;
    std::optional<FullscreenMonitors> fullscreen_monitors;

    // Metadata - authoritative source
    std::string name, wm_class, wm_class_name;
    Geometry floating_geometry;          // Authoritative for floating windows
    xcb_window_t transient_for;
    uint32_t sync_counter;
    uint64_t sync_value;
    uint32_t user_time;
    xcb_window_t user_time_window;
    uint64_t order;
};
```

### Simplified `FloatingWindow` struct (`src/lwm/wm.hpp`)

```cpp
struct FloatingWindow {
    xcb_window_t id = XCB_NONE;
    Geometry geometry;  // Runtime geometry for rendering
};
```

All other fields (monitor, workspace, name, transient_for) now come from Client.

### Simplified `Workspace` struct (`src/lwm/core/types.hpp`)

```cpp
struct Workspace {
    std::vector<xcb_window_t> windows;   // Just window IDs
    xcb_window_t focused_window = XCB_NONE;

    auto find_window(xcb_window_t id) { return std::ranges::find(windows, id); }
    auto find_window(xcb_window_t id) const { return std::ranges::find(windows, id); }
};
```

### Removed Structures

| Removed | Replaced By |
|---------|-------------|
| `Window` struct | Client fields + xcb_window_t in Workspace |
| `FloatingWindow::monitor` | `client->monitor` |
| `FloatingWindow::workspace` | `client->workspace` |
| `FloatingWindow::name` | `client->name` |
| `FloatingWindow::transient_for` | `client->transient_for` |
| `fullscreen_windows_` set | `client->fullscreen` |
| `above_windows_` set | `client->above` |
| `below_windows_` set | `client->below` |
| `iconic_windows_` set | `client->iconic` |
| `sticky_windows_` set | `client->sticky` |
| `maximized_*_windows_` sets | `client->maximized_*` |
| `shaded_windows_` set | `client->shaded` |
| `modal_windows_` set | `client->modal` |
| `skip_taskbar_windows_` set | `client->skip_taskbar` |
| `skip_pager_windows_` set | `client->skip_pager` |
| `fullscreen_restore_` map | `client->fullscreen_restore` |
| `maximize_restore_` map | `client->maximize_restore` |
| `client_order_` map | `client->order` |
| `sync_counters_`, `sync_values_` maps | `client->sync_counter/value` |
| `user_times_`, `user_time_windows_` maps | `client->user_time/user_time_window` |
| `fullscreen_monitors_` map | `client->fullscreen_monitors` |

### Retained (not part of Client model)

| Structure | Reason |
|-----------|--------|
| `wm_unmapped_windows_` | ICCCM unmap tracking (transient counter) |
| `pending_kills_`, `pending_pings_` | Process management runtime state |

---

## Client Field Authority

All Client fields are now authoritative and kept in sync:

| Client Field | Synced When |
|--------------|-------------|
| `monitor` | Window moves, floating drag, randr changes, workspace operations |
| `workspace` | Window moves, tiled drag, workspace operations |
| `floating_geometry` | Floating drag, configure requests, randr changes |
| `name` | Title updates via WM_NAME/_NET_WM_NAME |
| `transient_for` | Set on manage only |
| State flags | All set_* functions update both Client and EWMH |

---

## Performance Optimizations

| Function | Complexity |
|----------|------------|
| `monitor_index_for_window()` | O(1) via `client->monitor` |
| `workspace_index_for_window()` | O(1) via `client->workspace` |
| `is_window_visible()` | O(1) via Client lookup |
| `get_active_window_info()` | O(1) via Client lookup |
| `build_bar_state()` | O(1) per floating window |

---

## Helper Functions (`wm.cpp`)

### Query helpers (return `false` for unmanaged windows)

```cpp
bool is_client_fullscreen(xcb_window_t window) const;
bool is_client_iconic(xcb_window_t window) const;
bool is_client_sticky(xcb_window_t window) const;
bool is_client_above(xcb_window_t window) const;
bool is_client_below(xcb_window_t window) const;
bool is_client_maximized_horz(xcb_window_t window) const;
bool is_client_maximized_vert(xcb_window_t window) const;
bool is_client_shaded(xcb_window_t window) const;
bool is_client_modal(xcb_window_t window) const;
bool is_client_skip_taskbar(xcb_window_t window) const;
bool is_client_skip_pager(xcb_window_t window) const;
bool is_client_demands_attention(xcb_window_t window) const;
```

---

## Verification Checklist

### 1. No legacy set/map uses remain

```bash
grep -E 'fullscreen_windows_\.|iconic_windows_\.|sticky_windows_\.' src/lwm/wm.cpp
grep -E 'above_windows_\.|below_windows_\.|shaded_windows_\.' src/lwm/wm.cpp
grep -E 'modal_windows_\.|maximized_horz_windows_\.|maximized_vert_windows_\.' src/lwm/wm.cpp
grep -E 'skip_taskbar_windows_\.|skip_pager_windows_\.' src/lwm/wm.cpp
grep 'client_order_\.' src/lwm/wm.cpp
```

All should return **no matches**.

### 2. Window struct removed

```bash
grep 'struct Window' src/lwm/core/types.hpp
```

Should return **no matches**.

### 3. Build and tests pass

```bash
cd build && make -j$(nproc)
./tests/lwm_tests  # All tests pass
```

---

## Files Modified

| File | Changes |
|------|---------|
| `src/lwm/core/types.hpp` | Client struct, removed Window struct, simplified Workspace |
| `src/lwm/wm.hpp` | Simplified FloatingWindow struct |
| `src/lwm/wm.cpp` | All state management uses Client |
| `src/lwm/wm_events.cpp` | Uses Client for all window state |
| `src/lwm/wm_drag.cpp` | Syncs Client on geometry/location changes |
| `src/lwm/layout/layout.cpp` | Takes `vector<xcb_window_t>` |
| `src/lwm/core/invariants.hpp` | Updated for xcb_window_t windows |
| `tests/test_workspace.cpp` | Updated for xcb_window_t windows |
| `tests/test_focus_policy.cpp` | Updated for xcb_window_t windows |
