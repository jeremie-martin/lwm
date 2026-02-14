# CLAUDE.md

> **Documentation Navigation**
> - Previous: [README.md](README.md) (Quick start) | [BEHAVIOR.md](BEHAVIOR.md) (User-facing behavior)
> - Related: [DOCS_INDEX.md](DOCS_INDEX.md) (Documentation roadmap) | [COMPLIANCE.md](COMPLIANCE.md) (Protocol requirements)

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Quick Reference

**Core files** (~5792 lines total):
- `wm.cpp` (~2309) - Main window management, workspace/monitor operations, EWMH updates
- `wm_events.cpp` (~1604) - XCB event handlers (switch on response_type)
- `wm_floating.cpp` (~556) - Floating window management
- `wm_focus.cpp` (~487) - Focus handling logic
- `wm_workspace.cpp` (~329) - Workspace switching, toggling, moving windows
- `wm_ewmh.cpp` (~256) - EWMH protocol handling
- `wm_drag.cpp` (~251) - Mouse drag state machine

**Key types** (`src/lwm/core/types.hpp`):
- `Client` - Authoritative state for all windows
- `Workspace` - Tiled window list + focus tracking
- `Monitor` - RANDR output with workspaces, struts

---

## Quick Commands

**Build:**
```bash
make            # Release build
make debug      # Debug build
```

**Install:**
```bash
sudo make install    # Install to /usr/local/bin
sudo make uninstall  # Remove from /usr/local/bin
```

**Test:**
```bash
make test                              # Build and run all tests
./build/tests/lwm_tests "pattern"      # Run single test
```

**Test with Xephyr:**
```bash
./scripts/preview.sh
```

**Format code:**
```bash
find src -name '*.cpp' -o -name '*.hpp' | xargs clang-format -i
```

**Clean:**
```bash
make clean      # Remove build directory
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
│  • floating_windows_: vector<xcb_window_t>  (MRU order)          │
│  • dock_windows_, desktop_windows_                              │
└─────────────────────────────────────────────────────────────────┘
         ↓                    ↓                    ↓
  ┌────────────┐    ┌──────────┐    ┌──────────┐
  │   Ewmh     │    │  Layout   │    │  Focus    │
  │  Protocol  │    │  Tiling   │    │  utils    │
  └────────────┘    └──────────┘    └──────────┘
         ↑                    ↑
  ┌────────────┐    ┌──────────┐
  │ Connection │    │KeybindMgr│
  │ XCB wrapper│    │          │
  └────────────┘    └──────────┘
         ↓
  ┌────────────┐
  │  Config    │
  │ (TOML)     │
  └────────────┘
```

**Key data structures** (in `core/types.hpp`):
- `Client`: Authoritative source of truth for all window state (kind, monitor, workspace, state flags, geometry)
- `Workspace`: Ordered list of tiled windows + focused window tracking
- `Monitor`: RANDR output with geometry, workspaces, struts, bar window

For detailed specifications, see:
- **[DOCS_INDEX.md](DOCS_INDEX.md)** - Documentation roadmap and quick reference
- **[IMPLEMENTATION.md](IMPLEMENTATION.md)** - Architecture, data structures, invariants
- **[STATE_MACHINE.md](STATE_MACHINE.md)** - Window states and transitions
- **[EVENT_HANDLING.md](EVENT_HANDLING.md)** - Event-by-event handling specifications

## Adding Features

**New keybind action:**
1. Handle action string in `wm_events.cpp` handle_key_press
2. Parse in `config/config.cpp`
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
- `test_client_state_policy.cpp`, `test_window_rules.cpp`
- `test_focus_policy.cpp`, `test_focus_restoration_policy.cpp`, `test_focus_visibility_policy.cpp`
- `test_focus_cycling_policy.cpp`
- `test_workspace.cpp`, `test_workspace_policy.cpp`
- `test_floating_policy.cpp`, `test_layout_policy.cpp`
- `test_multimonitor_policy.cpp`
- `test_ewmh_classification.cpp`, `test_ewmh_policy.cpp`
- `test_integration_focus.cpp` - X11 integration tests

## Related Documentation

- **BEHAVIOR.md**: High-level behavior specification (focus policy, workspace model, drag semantics)
- **COMPLETE_STATE_MACHINE.md**: Complete implementation specification (states, transitions, invariants)
- **COMPLIANCE.md**: ICCCM/EWMH protocol compliance requirements
- **SPEC_CLARIFICATIONS.md**: Design decisions on ambiguous spec points
