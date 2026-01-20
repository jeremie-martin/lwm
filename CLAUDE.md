# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Quick Commands

**Build:**
```bash
mkdir -p build && cd build && cmake .. && make -j$(nproc)
```

**Test:**
```bash
cd build && cmake -DBUILD_TESTS=ON .. && make -j$(nproc) && ./tests/lwm_tests
```

**Run single test:**
```bash
./tests/lwm_tests "test name pattern"
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

## Architecture

LWM is a minimal tiling window manager for X11 written in C++23. It follows a centralized architecture with the WindowManager orchestrating all components.

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

**Key source files:**
- `wm.cpp`: Core logic (~3500 lines) - window management, workspace/monitor operations, EWMH updates
- `wm_events.cpp`: Event handlers (~1300 lines) - all XCB event handlers in switch on response_type
- `wm_drag.cpp`: Mouse drag state machine (~250 lines) - tiled reorder and floating move/resize

**Key data structures** (in `core/types.hpp`):
- `Client`: Authoritative source of truth for all window state (kind, monitor, workspace, state flags, geometry)
- `Workspace`: Ordered list of tiled windows + focused window tracking
- `Monitor`: RANDR output with geometry, workspaces, struts, bar window

## Adding Features

**New keybind action:**
1. Handle action string in `wm_events.cpp` handle_key_press
2. Parse in `config.cpp`
3. Document in `config.toml.example`

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

Uses `.clang-format` (WebKit-based). Key settings:
- 4-space indent, 120 column limit
- Allman braces (opening brace on new line)
- `snake_case` for functions/variables, `PascalCase` for types, trailing underscore for private members
- Prefer `std::ranges` algorithms

## Debugging

Debug builds enable:
- `LWM_DEBUG(msg)` - logs to stderr with file:line
- `LWM_DEBUG_KEY(msg)` - keybinding debugging
- `LWM_ASSERT_INVARIANTS()` - validates state consistency (see `core/invariants.hpp`)

## Testing

Tests use Catch2. Test files in `tests/`:
- `test_focus_policy.cpp`, `test_focus_restoration_policy.cpp`, `test_focus_visibility_policy.cpp`
- `test_workspace.cpp`, `test_workspace_policy.cpp`
- `test_floating_policy.cpp`, `test_layout_policy.cpp`
- `test_ewmh_classification.cpp`, `test_ewmh_policy.cpp`
- `test_integration_focus.cpp` - X11 integration tests

## Related Documentation

- **BEHAVIOR.md**: High-level behavior specification (focus policy, workspace model, drag semantics)
- **COMPLIANCE.md**: ICCCM/EWMH protocol compliance requirements
- **SPEC_CLARIFICATIONS.md**: Design decisions on ambiguous spec points
