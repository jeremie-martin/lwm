# Contributing

This file is the operational guide for changing LWM safely.

## Fast Commands

```bash
make          # release build
make debug    # debug build
make test     # build and run the full test suite
make clean
```

Useful direct invocations:

```bash
ctest --test-dir build --output-on-failure
./build/tests/lwm_tests "focus"
./scripts/preview.sh
```

## Code Map

- `src/lwm/wm.cpp`: core state transitions, visibility sync, layout, fullscreen, stacking
- `src/lwm/wm_events.cpp`: X event dispatch and client-message handling
- `src/lwm/wm_focus.cpp`: focus assignment and fallback
- `src/lwm/wm_workspace.cpp`: workspace switching and cross-workspace/monitor moves
- `src/lwm/wm_floating.cpp`: floating geometry and geometry-driven monitor reassignment
- `src/lwm/wm_drag.cpp`: drag state machine for floating move/resize and tiled reorder
- `src/lwm/wm_ewmh.cpp`: desktop, workarea, client-list, and root-property updates
- `src/lwm/core/types.hpp`: `Client`, `Workspace`, and `Monitor`
- `config.toml.example`: user-facing config surface

Start with [`ARCHITECTURE.md`](ARCHITECTURE.md) before editing any of the state-transition code.

## Test Map

Use the smallest relevant suite first, then run the broader suite before finishing.

- focus, fullscreen, stacking, activation: `tests/test_integration_focus.cpp`
- workspace visibility and monitor/workspace moves: `tests/test_integration_workspace.cpp`
- property-driven reclassification and hint changes: `tests/test_integration_property_notify.cpp`
- keybind behavior and config parsing: `tests/test_keybind_policy.cpp`, `tests/test_config.cpp`
- EWMH/root-property behavior: `tests/test_ewmh_policy.cpp`, `tests/test_ewmh_classification.cpp`, `tests/test_integration_client_message.cpp`

When a change crosses visibility, focus, and fullscreen boundaries, treat it as integration work and add or update an integration test.

## Change Checklist

If you change behavior:

1. update the relevant tests in `tests/`
2. update `config.toml.example` if the config surface changed
3. update the docs that own the changed behavior

Doc ownership:

- [`README.md`](README.md): setup, install, run, and a short mental model
- [`ARCHITECTURE.md`](ARCHITECTURE.md): runtime model, invariants, and transition funnels
- [`COMPLIANCE.md`](COMPLIANCE.md): ICCCM/EWMH behavior and limits
- [`FEATURE_IDEAS.md`](FEATURE_IDEAS.md): backlog only

## Common Change Recipes

- New keybind action:
  - add handling in `src/lwm/wm_events.cpp`
  - update defaults and parsing in `src/lwm/config/config.cpp`
  - update `config.toml.example` and `README.md` if the user-facing surface changed
- New window state:
  - add `Client` state in `src/lwm/core/types.hpp`
  - apply transitions in `src/lwm/wm.cpp`
  - wire protocol handling in `src/lwm/wm_events.cpp`
  - update `ARCHITECTURE.md` and `COMPLIANCE.md` if semantics changed
- New EWMH atom support:
  - add atom wiring in `src/lwm/core/ewmh.hpp` and `src/lwm/core/ewmh.cpp`
  - handle the message/property in the relevant WM path
  - document support and limits in `COMPLIANCE.md`

## Debugging Hooks

- `LOG_TRACE(...)` and `LOG_DEBUG(...)`
- `LOG_KEY(...)`
- `LWM_ASSERT_INVARIANTS(...)` in debug builds

Use the existing funnels and assertions before introducing new ad hoc state transitions.
