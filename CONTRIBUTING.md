# Contributing

This file is the operational guide for changing LWM safely.

## Fast Commands

```bash
make          # release build
make debug    # debug build
make test     # build and run the full test suite
make install
make uninstall # currently removes /usr/local/bin/lwm only
make clean
```

Useful direct invocations:

```bash
ctest --test-dir build --output-on-failure
./build/tests/lwm_tests "focus"
./scripts/preview.sh
```

Tests require `xcb-xtest` in addition to the runtime XCB dependencies.

## Code Map

- `src/lwm/wm.cpp`: core state transitions, visibility sync, layout, fullscreen, stacking
- `src/lwm/wm_events.cpp`: X event dispatch and client-message handling
- `src/lwm/wm_focus.cpp`: focus assignment and fallback
- `src/lwm/wm_workspace.cpp`: workspace switching and cross-workspace/monitor moves
- `src/lwm/wm_floating.cpp`: floating geometry and geometry-driven monitor reassignment
- `src/lwm/wm_drag.cpp`: drag state machine for floating move/resize and tiled reorder
- `src/lwm/wm_ewmh.cpp`: desktop, workarea, client-list, and root-property updates
- `src/lwm/wm_restart.cpp`: state serialization, restore, and graceful restart
- `src/lwm/wm_scratchpad.cpp`: named scratchpads and generic scratchpad pool (show/hide/cycle, restart preservation)
- `src/lwm/core/events.hpp`: IPC subscription event names, masks, and JSON escaping helpers
- `src/lwm/core/ipc.hpp`: IPC socket path helpers and root-property discovery helpers
- `src/lwm/core/types.hpp`: `Client`, `Workspace`, and `Monitor`
- `src/lwm/core/policy.hpp`: pure policy functions (focus selection, workspace manipulation, visibility, classification, hotplug)
- `src/lwm/core/invariants.hpp`: debug-build invariant assertions
- `src/app/lwmctl.cpp`: user-facing command-line wrapper around the IPC socket
- `scripts/lwm-notify.sh`: notification helper installed as `lwm-notify`
- `config.toml.example`: user-facing config surface

Start with [`ARCHITECTURE.md`](ARCHITECTURE.md) before editing any of the state-transition code.

## Coding Style

4-space indentation, braces on their own lines, concise comments only where logic is non-obvious. `PascalCase` for types, `snake_case` for functions and files, uppercase for compile-time constants and macros. No formatter or linter is enforced in-tree, so consistency with nearby code is the rule.

## Design Principles

Two rules to apply when changing the core:

**Make invalid states unrepresentable.** Model domain concepts as explicit types in `src/lwm/core/types.hpp` and pure functions in `src/lwm/core/policy.hpp`. Invariants live in `src/lwm/core/invariants.hpp` and are owned by the module that drives the transition (visibility, stacking, and fullscreen are owned by `src/lwm/wm.cpp`). Encode "after changing X, also do Y" inside one funnel rather than spreading the sequence across call sites. Treat repeated null checks, fallback branches, and "just in case" code in the core as smells that usually point at a missing type or a misplaced ownership boundary.

**Test through the real code path.** The boundaries are the X server, the IPC socket, and the filesystem config — fake those, run everything between for real:

- pure logic → `*_policy.cpp` against `src/lwm/core/policy.hpp`
- end-to-end behavior → `*_integration_*.cpp` against `tests/x11_test_harness.hpp` (Xvfb)

If a change is hard to test without faking something inside the WM, treat it as a design signal: restructure the production code rather than the test. Defensive code is for the boundaries, not the model.

**Client lookup convention.** `WindowManager` exposes two complementary lookups:

- `get_client(window) → Client*` is the **lookup** path: nullable, for X event dispatch boundaries where the window may not be managed (override-redirect, race with destroy, an EWMH client message naming an arbitrary window).
- `require_client(window) → Client&` is the **require** path: aborts if the window is not in `clients_`. Use it from internal funnels that already proved managed status — iterating `clients_` or `Workspace::windows` or `scratchpad_pool_`, post-`manage_*` finalization, hotplug-plan apply, restart-state apply, operations on `active_window_` or `monitors_[i].fullscreen_owner`.

When you find yourself writing `auto* c = get_client(w); if (!c) return;` in code that did not just receive `w` from an X event, the right fix is usually `require_client` — the null branch is dead code that hides where the real ownership boundary should sit.

## Test Map

Tests use Catch2. File names follow `tests/test_<area>.cpp`. Integration cases use `TEST_CASE("Integration: ...", "[integration]...")` so they group cleanly in filtered runs. Use the smallest relevant suite first, then run the broader suite before finishing.

- focus, fullscreen, stacking, activation: `tests/test_integration_focus.cpp`
- focus-follows-mouse and input models: `tests/test_integration_focus_input.cpp`
- focus fallback selection and cycling: `tests/test_focus_policy.cpp`, `tests/test_focus_cycling_policy.cpp`, `tests/test_focus_restoration_policy.cpp`
- workspace visibility and monitor/workspace moves: `tests/test_integration_workspace.cpp`
- config parsing and live reload: `tests/test_config_parser.cpp`, `tests/test_integration_config_reload.cpp`
- workspace policy (focus history, tiled membership): `tests/test_workspace_policy.cpp`
- WM_STATE transitions (manage, iconify, unmanage): `tests/test_integration_wm_state.cpp`
- overlay windows above fullscreen: `tests/test_integration_overlay.cpp`
- scratchpads: `tests/test_integration_scratchpad.cpp`
- monitor hotplug and window relocation: `tests/test_monitor_hotplug.cpp`
- property-driven reclassification and hint changes: `tests/test_integration_property_notify.cpp`
- keybind behavior and config parsing: `tests/test_keybind_policy.cpp`
- split-tree layout and ratio math: `tests/test_split_tree.cpp`
- floating and stacking pure policy: `tests/test_floating_policy.cpp`, `tests/test_stacking_policy.cpp`
- LWM-specific compositor classification property: `tests/test_integration_lwm_window_class.cpp`
- notification attention helper path: `tests/test_integration_notify_attention.cpp`
- IPC event subscriptions and `lwmctl subscribe`: `tests/test_integration_subscribe.cpp`
- EWMH/root-property behavior: `tests/test_ewmh_policy.cpp`, `tests/test_ewmh_classification.cpp`, `tests/test_integration_client_message.cpp`
- client state and window rules: `tests/test_client_state_policy.cpp`, `tests/test_window_rules.cpp`
- basic workspace data structure behavior: `tests/test_workspace.cpp`

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
- [`SHADERS.md`](SHADERS.md): compositor-facing visual recipes and LWM-specific properties useful to compositors
- [`ROADMAP.md`](ROADMAP.md): open work and open questions

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
- New IPC or `lwmctl` command:
  - implement the WM command in `WindowManager::run_ipc_command`
  - expose the CLI shape in `src/app/lwmctl.cpp`
  - add integration coverage when the command affects real WM state
  - update `README.md` if it is user-facing
- New compositor-facing `_LWM_*` property:
  - publish it from the authoritative state owner
  - add integration coverage that reads the X property
  - document stability and intended use in `COMPLIANCE.md` and `SHADERS.md`

## Debugging Hooks

- `LOG_TRACE(...)` and `LOG_DEBUG(...)`
- `LOG_KEY(...)`
- `LWM_ASSERT_INVARIANTS(...)` in debug builds

Use the existing funnels and assertions before introducing new ad hoc state transitions.

## Commits & PRs

Short imperative subjects (`Stabilize fullscreen focus and restacking`); `docs:` prefix for doc-only changes. Keep commits focused by subsystem. A PR should describe the problem, the behavior impact, the exact test commands run, and Xephyr reproduction steps for window-management changes.
