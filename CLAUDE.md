# CLAUDE.md

Contributor quick reference for this repository.

## Fast Commands

```bash
make          # release build
make debug    # debug build
make test     # run all tests
make clean
```

Single test pattern:

```bash
./build/tests/lwm_tests "focus"
```

Xephyr sandbox:

```bash
./scripts/preview.sh
```

## Primary Code Areas

- `src/lwm/wm.cpp`: core window/state transitions
- `src/lwm/wm_events.cpp`: X event handling
- `src/lwm/wm_focus.cpp`: focus policy
- `src/lwm/wm_workspace.cpp`: workspace operations
- `src/lwm/wm_floating.cpp`: floating behavior
- `src/lwm/wm_drag.cpp`: drag/reorder state machine
- `src/lwm/wm_ewmh.cpp`: EWMH desktop/workarea/client-list updates
- `src/lwm/core/types.hpp`: core data model
- `config.toml.example`: user-facing config and bindings

## If You Add/Change Behavior

1. Update tests in `tests/`.
2. Update config docs/examples if keybinds or options changed.
3. Update markdown docs where behavior contracts changed.

### Common Change Recipes

- New keybind action: add handling in `src/lwm/wm_events.cpp` and update defaults/docs in `src/lwm/config/config.cpp` and `config.toml.example`.
- New window state: add `Client` flag in `src/lwm/core/types.hpp`, apply transitions in `src/lwm/wm.cpp`, and wire client-message handling in `src/lwm/wm_events.cpp`.
- New EWMH atom support: add atom wiring in `src/lwm/core/ewmh.hpp` / `src/lwm/core/ewmh.cpp`, then update compliance notes in `COMPLIANCE.md`.

## Debugging Hooks

- `LOG_TRACE(...)` / `LOG_DEBUG(...)`
- `LOG_KEY(...)`
- `LWM_ASSERT_INVARIANTS(...)` (debug builds)

Use these before introducing larger structural changes.

## Documentation Ownership

- [`README.md`](README.md): setup, run, high-level behavior
- [`BEHAVIOR.md`](BEHAVIOR.md): user-visible behavior contract
- [`IMPLEMENTATION.md`](IMPLEMENTATION.md): internal flow/invariants/pitfalls
- [`COMPLIANCE.md`](COMPLIANCE.md): protocol support and limits
- [`FEATURE_IDEAS.md`](FEATURE_IDEAS.md): backlog only
