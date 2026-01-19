# LWM Development Guide

## Quick Commands

**Build:**
```bash
mkdir -p build && cd build && cmake .. && make -j$(nproc)
```

**Test:**
```bash
cmake -DBUILD_TESTS=ON && make -j$(nproc) && ./tests/lwm_tests
```

**Test with Xephyr:**
```bash
./scripts/preview.sh
```

**Format code:**
```bash
find src -name '*.cpp' -o -name '*.hpp' | xargs clang-format -i
```

**Debug build:**
```bash
cmake -DCMAKE_BUILD_TYPE=Debug ..
```

## Project Structure

```
lwm/
├── CMakeLists.txt           # Root CMake
├── config.toml.example      # Example config
├── config/polybar.ini       # Polybar configuration
├── scripts/
│   ├── preview.sh           # Xephyr testing
│   └── launch-polybar.sh    # Polybar launcher
└── src/
    ├── app/
    │   └── main.cpp                 # Entry point
    └── lwm/                        # Library (liblwm)
        ├── core/
        │   ├── connection.hpp/cpp         # RAII XCB wrapper
        │   ├── types.hpp                  # Client, Workspace, Monitor, Geometry
        │   ├── invariants.hpp              # Debug invariant assertions
        │   ├── log.hpp                    # Zero-cost debug logging
        │   ├── ewmh.hpp/cpp               # EWMH protocol implementation
        │   ├── focus.hpp/cpp               # Focus state management
        │   └── floating.hpp/cpp            # Floating window placement
        ├── layout/
        │   └── layout.hpp/cpp              # Master-stack tiling
        ├── config/
        │   └── config.hpp/cpp              # TOML config parsing
        ├── keybind/
        │   └── keybind.hpp/cpp             # Key/mouse binding resolution
        ├── bar/
        │   └── bar.hpp/cpp                 # Internal status bar
        ├── wm.hpp                          # WindowManager class declaration
        ├── wm.cpp                          # Core logic (~3500 lines)
        ├── wm_events.cpp                   # Event handlers (~1300 lines)
        └── wm_drag.cpp                     # Mouse drag operations (~250 lines)
```

## Architecture

LWM follows a centralized architecture with the WindowManager orchestrating all components:

```
┌─────────────────────────────────────────────────────────────────┐
│                     WindowManager (wm.cpp)                      │
│                                                                  │
│  Core State:                                                     │
│  • clients_: unordered_map<window, Client>                     │
│  • monitors_: vector<Monitor>                                   │
│  • floating_windows_: vector<FloatingWindow>                     │
│  • dock_windows_, desktop_windows_                              │
└─────────────────────────────────────────────────────────────────┘
         ↓                    ↓                    ↓
  ┌────────────┐    ┌──────────┐    ┌──────────┐
  │   Ewmh     │    │  Layout   │    │ StatusBar │
  │  Protocol  │    │  Tiling   │    │ (opt)    │
  └────────────┘    └──────────┘    └──────────┘
         ↑                    ↑                ↑
  ┌────────────┐    ┌──────────┐    ┌──────────┐
  │ Connection │    │KeybindMgr│    │  Focus    │
  │ XCB wrapper│    │          │    │  utils    │
  └────────────┘    └──────────┘    └──────────┘
         ↓
  ┌────────────┐
  │  Config    │
  │ (TOML)     │
  └────────────┘
```

**Components:**
- **Connection**: RAII wrapper for XCB connection, screen, key symbols, RANDR detection
- **Ewmh**: Implements Extended Window Manager Hints (properties, state, client lists)
- **Layout**: Master-stack tiling with size hints and sync request support
- **KeybindManager**: Parses key names to keysyms, grabs keys, resolves actions
- **StatusBar**: Optional per-monitor bar showing workspaces and window titles
- **Focus**: Monitor focus state machine and pointer-based focus
- **Floating**: Floating window placement with centering logic
- **WindowManager**: Orchestrator, runs event loop, manages windows

## Key Data Structures

**Client** (`core/types.hpp`): Authoritative source of truth for all window state
```cpp
struct Client {
    xcb_window_t id;
    Kind kind = Kind::Tiled;           // Tiled, Floating, Dock, Desktop
    size_t monitor, workspace;

    // Identification
    std::string name, wm_class, wm_class_name;

    // State flags (synced with _NET_WM_STATE)
    bool mapped, fullscreen, above, below, iconic, sticky;
    bool maximized_horz, maximized_vert, shaded, modal;
    bool skip_taskbar, skip_pager, demands_attention;

    // Floating-specific
    Geometry floating_geometry;
    xcb_window_t transient_for;

    // Restore points
    std::optional<Geometry> fullscreen_restore, maximize_restore;
    std::optional<FullscreenMonitors> fullscreen_monitors;

    // Sync and focus prevention
    uint32_t sync_counter, sync_value;
    uint32_t user_time;
    xcb_window_t user_time_window;

    // Client list ordering
    uint64_t order;
};
```

**Workspace** (`core/types.hpp`):
```cpp
struct Workspace {
    std::vector<xcb_window_t> windows;    // Tiled windows only
    xcb_window_t focused_window;

    auto find_window(xcb_window_t id);
};
```

**Monitor** (`core/types.hpp`):
```cpp
struct Monitor {
    xcb_randr_output_t output;
    std::string name;
    int16_t x, y;
    uint16_t width, height;
    std::vector<Workspace> workspaces;
    size_t current_workspace, previous_workspace;
    xcb_window_t bar_window;
    Strut strut;

    Workspace& current();
    Geometry geometry() const;
    Geometry working_area() const;
};
```

## File Organization

**wm.cpp**: Core logic (~3500 lines)
- Window management (manage/unmanage)
- Workspace operations (switch, move)
- Monitor operations
- State helpers (is_window_visible, etc.)
- EWMH property updates

**wm_events.cpp**: Event handlers (~1300 lines)
- All XCB event handlers in switch on response_type
- MapRequest, UnmapNotify, DestroyNotify
- EnterNotify, MotionNotify, ButtonPress/Release
- KeyPress, ClientMessage
- ConfigureRequest, PropertyNotify, Expose, SelectionClear

**wm_drag.cpp**: Mouse drag state machine (~250 lines)
- begin_drag(), update_drag(), end_drag()
- Handles both tiled (reorder) and floating (move/resize) drags

**core/*.cpp**: Reusable utilities
- `connection.cpp`: XCB RAII wrapper
- `ewmh.cpp`: EWMH protocol implementation
- `focus.cpp`: Focus state machine
- `floating.cpp`: Floating placement logic
- `config.cpp`: TOML parsing
- `keybind.cpp`: Key/mouse binding resolution
- `layout.cpp`: Master-stack tiling algorithm

## Adding Features

**New keybind action:**
1. Add action type enum in `keybind.hpp`
2. Parse in `config.cpp`
3. Handle in `wm_events.cpp` handle_key_press
4. Document in `config.toml.example` and `README.md`

**New layout:**
1. Add layout mode to Layout class
2. Implement calculate() method
3. Add config option
4. Switch layout based on config or keybind

**New window state:**
1. Add flag to Client struct
2. Implement set_* function in wm.cpp
3. Update EWMH _NET_WM_STATE
4. Handle in wm_events.cpp ClientMessage

**New EWMH property:**
1. Add atom to Ewmh class
2. Set on startup in wm.cpp
3. Update when state changes
4. Document in COMPLIANCE.md

## Code Style

- **Indent**: 4 spaces (no tabs)
- **Column limit**: 120 characters
- **Braces**: Allman style (opening brace on new line)
- **Naming**:
  - Functions/variables: `snake_case`
  - Types/classes: `PascalCase`
  - Private members: `snake_case_` (trailing underscore)
- **Algorithms**: Prefer `std::ranges` algorithms
- **Resources**: RAII for all resources (no manual cleanup)
- **Logging**: Use `LWM_DEBUG()` macro for debug builds (expands to nothing in release)
- **Assertions**: Use `LWM_ASSERT_INVARIANTS()` for debug builds

## Debugging

**Enable debug logging:**
```bash
cmake -DCMAKE_BUILD_TYPE=Debug ..
```

Debug builds enable:
- `LWM_DEBUG(msg)` - logs to stderr with file:line
- `LWM_DEBUG_KEY(msg)` - for keybinding debugging
- `LWM_ASSERT_INVARIANTS()` - validates state consistency

**Invariant checking:**
Defined in `core/invariants.hpp`:
- Workspace consistency
- Focus validity
- Client state integrity
- Client list accuracy

**Common issues:**
- Window not appearing: Check classification (is it a popup/tooltip?)
- Focus issues: Verify `WM_HINTS.input` and `WM_PROTOCOLS`
- Layout not updating: Check struts and workarea
- Multi-monitor: Verify RANDR detection and monitor geometry

## Testing

**Run tests:**
```bash
mkdir -p build && cd build
cmake -DBUILD_TESTS=ON ..
make -j$(nproc)
./tests/lwm_tests
```

**Test structure:**
- `tests/test_focus_policy.cpp` - Focus behavior
- `tests/test_workspace.cpp` - Workspace operations
- `tests/test_floating_policy.cpp` - Floating window behavior

Tests use Catch2 framework. Add new tests in corresponding files.

## Protocol Compliance

LWM implements comprehensive ICCCM and EWMH compliance:

- **ICCCM**: WM_S0 selection, SubstructureRedirect, WM_STATE, WM_NORMAL_HINTS, WM_HINTS, WM_PROTOCOLS, focus management, unmap tracking
- **EWMH**: Root properties (_NET_SUPPORTED, _NET_CLIENT_LIST, _NET_ACTIVE_WINDOW, etc.), window types, states, client messages, focus stealing prevention, ping/sync protocols

See **COMPLIANCE.md** for complete specification and conformance checklist.

## Behavior Specification

High-level user-visible behavior is defined in **BEHAVIOR.md**:
- Monitor/workspace model
- Focus policy
- Window classification rules
- Multi-monitor behavior
- Drag-reorder semantics

See **BEHAVIOR.md** for behavior specification.

## Design Decisions

Where EWMH/ICCCM specifications allowed interpretation, LWM made specific decisions documented in **SPEC_CLARIFICATIONS.md**:
- Dock windows included in _NET_CLIENT_LIST
- _NET_CLIENT_LIST updates on manage/unmanage (not visibility)
- Per-monitor sticky window scope
- Popup window handling (unmanaged)
- WM_STATE vs workspace visibility
- Transient window state inheritance

## Performance Considerations

- **O(1) lookups**: `clients_` unordered_map for all window state
- **Workspace vectors**: Only contain tiled window IDs, used for layout
- **EWMH updates**: Throttled where possible, avoid property spam
- **Event loop**: Efficient poll() with timeout for ping/kill deadlines
- **Invariant checks**: Debug-only, zero-cost in release

## Limitations

- Single layout mode (master-stack only)
- Window size increments not enforced (smooth resizing prioritized)
- _NET_WM_SYNC_REQUEST uses blocking wait (50ms timeout)
- Fullscreen monitors limited to 4-monitor span
- Shade and modal are state-only (no visual effect implemented)
- Floating windows cannot toggle workspace via drag
- Only one previous workspace tracked (no full history)
