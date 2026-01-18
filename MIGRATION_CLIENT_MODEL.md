# Client Model Migration Note

**Date**: January 2026  
**Scope**: Unified Client record for per-window state management

---

## Purpose

Replace scattered state stored across 11+ `unordered_set` and `unordered_map` structures with a single, authoritative `Client` struct per managed window.

**Problems solved:**
- State synchronization bugs between duplicate data structures
- O(n) lookups replaced with O(1) via `clients_` map
- Simplified invariant reasoning (all state in one place)
- Foundation for future workspace unification

---

## What Changed

### New: `Client` struct (`src/lwm/core/types.hpp`)

```cpp
struct Client {
    xcb_window_t id;
    enum class Kind { Tiled, Floating, Dock, Desktop };
    Kind kind;
    size_t monitor, workspace;
    
    // State flags (replaces 11 unordered_sets)
    bool fullscreen, iconic, sticky, above, below;
    bool maximized_horz, maximized_vert, shaded, modal;
    bool skip_taskbar, skip_pager, demands_attention;
    
    // Restore geometries (replaces 2 unordered_maps)
    std::optional<Geometry> fullscreen_restore;
    std::optional<Geometry> maximize_restore;
    
    // Metadata and tracking
    std::string name, wm_class, wm_class_name;
    Geometry floating_geometry;
    xcb_window_t transient_for;
    xcb_sync_counter_t sync_counter;
    uint64_t sync_value;
    uint32_t user_time;
    xcb_window_t user_time_window;
    uint64_t order;
};
```

### New: Central registry (`src/lwm/wm.hpp`)

```cpp
std::unordered_map<xcb_window_t, Client> clients_;
```

### Removed from `wm.hpp`

| Removed Structure | Replaced By |
|-------------------|-------------|
| `fullscreen_windows_` | `client->fullscreen` |
| `above_windows_` | `client->above` |
| `below_windows_` | `client->below` |
| `iconic_windows_` | `client->iconic` |
| `sticky_windows_` | `client->sticky` |
| `maximized_horz_windows_` | `client->maximized_horz` |
| `maximized_vert_windows_` | `client->maximized_vert` |
| `shaded_windows_` | `client->shaded` |
| `modal_windows_` | `client->modal` |
| `skip_taskbar_windows_` | `client->skip_taskbar` |
| `skip_pager_windows_` | `client->skip_pager` |
| `fullscreen_restore_` | `client->fullscreen_restore` |
| `maximize_restore_` | `client->maximize_restore` |

### Retained (not part of Client model)

| Structure | Reason |
|-----------|--------|
| `wm_unmapped_windows_` | ICCCM unmap tracking (transient counter) |
| `fullscreen_monitors_` | Per-window monitor span config |
| `sync_counters_`, `sync_values_` | Sync protocol (could migrate to Client) |
| `pending_kills_`, `pending_pings_` | Process management |
| `user_times_`, `user_time_windows_` | Focus stealing (could migrate to Client) |

### Also Removed (consolidated into Client)

| Structure | Replaced By |
|-----------|-------------|
| `client_order_` | `client->order` (used for `_NET_CLIENT_LIST`) |

### Performance Optimizations

| Function | Before | After |
|----------|--------|-------|
| `monitor_index_for_window()` | O(monitors × workspaces) | O(1) via `client->monitor` |
| `workspace_index_for_window()` | O(monitors × workspaces) | O(1) via `client->workspace` |
| `update_ewmh_client_list()` | Used `client_order_` map | Uses `clients_` directly |

---

## Helper Functions Added (`wm.cpp`)

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

### Setter helpers (update Client and EWMH)

```cpp
void set_client_skip_taskbar(xcb_window_t window, bool enabled);
void set_client_skip_pager(xcb_window_t window, bool enabled);
void set_client_demands_attention(xcb_window_t window, bool enabled);
```

---

## Verification Checklist

### 1. No legacy set/map uses remain

Run these searches - all should return **no matches**:

```bash
# State sets
grep -E 'fullscreen_windows_\.|iconic_windows_\.|sticky_windows_\.' src/lwm/wm.cpp
grep -E 'above_windows_\.|below_windows_\.|shaded_windows_\.' src/lwm/wm.cpp
grep -E 'modal_windows_\.|maximized_horz_windows_\.|maximized_vert_windows_\.' src/lwm/wm.cpp
grep -E 'skip_taskbar_windows_\.|skip_pager_windows_\.' src/lwm/wm.cpp

# Restore maps
grep -E 'fullscreen_restore_\.|maximize_restore_\.' src/lwm/wm.cpp

# Order map
grep 'client_order_\.' src/lwm/wm.cpp
```

### 2. Client populated on manage

In `manage_window()` and `manage_floating_window()`:
- `Client` struct created with correct `Kind`
- Added to `clients_[window]`
- State flags initialized (e.g., `client.iconic = start_iconic`)

### 3. Client removed on unmanage

In `unmanage_window()` and `unmanage_floating_window()`:
- `clients_.erase(window)` called
- No erases from removed legacy sets

### 4. State mutations use Client directly

Each function gets `Client*` and modifies it:

| Function | Client fields modified |
|----------|----------------------|
| `set_fullscreen()` | `fullscreen`, `fullscreen_restore`, `above`, `below` |
| `set_window_above()` | `above`, `below` |
| `set_window_below()` | `below`, `above` |
| `set_window_sticky()` | `sticky` |
| `set_window_maximized()` | `maximized_horz`, `maximized_vert`, `maximize_restore` |
| `set_window_shaded()` | `shaded` |
| `set_window_modal()` | `modal` |
| `set_client_skip_taskbar()` | `skip_taskbar` |
| `set_client_skip_pager()` | `skip_pager` |
| `set_client_demands_attention()` | `demands_attention` |
| `iconify_window()` | `iconic` |
| `deiconify_window()` | `iconic` |

### 5. State queries use helpers

All state checks use `is_client_*()` helpers, not direct set access.

### 6. Build and tests pass

```bash
cd build && make clean && make -j$(nproc)
./tests/lwm_tests  # All 58 assertions pass
```

---

## Commit History

```
17e5179 Complete Client migration: skip_taskbar, skip_pager, demands_attention
1dffcf6 Update documentation for unified Client model
72f6366 Phase 5: Remove scattered state unordered_sets
3545d34 Phase 4b: Complete migration of state query uses
35fba41 Phase 4: Continue migrating state queries to helpers
c887abf Phase 4: Add state query helpers and start migration
b7fced3 Introduce unified Client record (Phases 1-3)
```

---

## Future Work (Optional)

1. **Phase 6**: Change `Workspace.windows` to store `xcb_window_t` IDs only (layout looks up Client)
2. **Unify `floating_windows_`** vector into workspace storage

---

## Files Modified

| File | Changes |
|------|---------|
| `src/lwm/core/types.hpp` | Added `Client` struct with all state flags |
| `src/lwm/wm.hpp` | Added `clients_` map, removed 13 legacy structures |
| `src/lwm/wm.cpp` | All state management uses Client |
| `BEHAVIOR.md` | Implementation note about unified Client |
