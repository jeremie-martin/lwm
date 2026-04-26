# CLAUDE.md

Compatibility entry point for Claude Code and similar repository-aware agents.

This file should remain present even when the human-facing documentation is reorganized.

## Start Here

- [`CONTRIBUTING.md`](CONTRIBUTING.md): workflow, code map, test map, and change checklist
- [`ARCHITECTURE.md`](ARCHITECTURE.md): runtime model, invariants, and transition funnels
- [`COMPLIANCE.md`](COMPLIANCE.md): ICCCM/EWMH behavior and limits

## Fast Commands

```bash
make          # release build
make debug    # debug build
make test     # build and run the full test suite
ctest --test-dir build --output-on-failure
./build/tests/lwm_tests "focus"
./scripts/preview.sh
```

## Code Areas

- `src/lwm/wm.cpp`: core state transitions, visibility sync, layout, fullscreen, stacking
- `src/lwm/wm_events.cpp`: X event dispatch and client-message handling
- `src/lwm/wm_focus.cpp`: focus assignment and fallback
- `src/lwm/wm_workspace.cpp`: workspace switching and cross-workspace/monitor moves
- `src/lwm/wm_floating.cpp`: floating geometry and geometry-driven monitor reassignment
- `src/lwm/wm_drag.cpp`: drag state machine for floating move/resize and tiled reorder
- `src/lwm/wm_ewmh.cpp`: desktop, workarea, client-list, and root-property updates
- `src/lwm/wm_restart.cpp`: state serialization, restore, and graceful restart
- `src/lwm/wm_scratchpad.cpp`: named scratchpads and generic scratchpad pool (show/hide/cycle, restart preservation)
- `src/lwm/core/types.hpp`: `Client`, `Workspace`, and `Monitor`
- `src/lwm/core/policy.hpp`: pure policy functions (focus selection, workspace manipulation, visibility, classification, hotplug)
- `src/lwm/core/invariants.hpp`: debug-build invariant assertions

## If You Change Behavior

1. Update the relevant tests in `tests/`.
2. Update `config.toml.example` if the user-facing config surface changed.
3. Update the owning docs:
   `README.md` for entry-level usage,
   `ARCHITECTURE.md` for runtime model/invariants,
   `COMPLIANCE.md` for protocol behavior.
