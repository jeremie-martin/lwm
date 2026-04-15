# Repository Guidelines

## Project Structure & Module Organization

LWM is a C++23 X11 window manager. Core code lives in `src/lwm/`: `wm.cpp` owns major state transitions, `wm_events.cpp` handles X events, `wm_focus.cpp` covers focus/fallback, `wm_workspace.cpp` and `wm_floating.cpp` handle moves and geometry, and `src/lwm/core/` contains shared types and policy helpers. Tests live in `tests/`. Runtime/config assets live in `config/`, `config.toml.example`, and `scripts/preview.sh`. Treat `build/` and `build-worker/` as generated output.

Start with `ARCHITECTURE.md` before changing visibility, focus, fullscreen, or stacking logic. Use `COMPLIANCE.md` for ICCCM/EWMH-facing behavior and `CONTRIBUTING.md` for workflow details.

## Build, Test, and Development Commands

- `make` builds the release binary.
- `make debug` builds with debug settings and assertions.
- `make test` builds and runs the full test suite.
- `ctest --test-dir build --output-on-failure` reruns all built tests.
- `./build/tests/lwm_tests "focus"` runs a filtered Catch2 test subset.
- `./scripts/preview.sh` launches LWM inside Xephyr for manual verification.

## Coding Style & Naming Conventions

Match the existing C++ style: 4-space indentation, braces on their own lines, and concise comments only where logic is non-obvious. Follow current naming patterns: `PascalCase` for types, `snake_case` for functions/files, and uppercase for compile-time constants/macros. No formatter or linter is enforced in-tree, so consistency with nearby code is the rule. Prefer existing logging and invariant helpers such as `LOG_TRACE(...)` and `LWM_ASSERT_INVARIANTS(...)`.

## Testing Guidelines

Tests use Catch2. File names follow `tests/test_<area>.cpp`. Keep unit/policy tests near the subsystem they cover, and use `Integration: ...` names for X11 runtime scenarios. If a change crosses focus, visibility, fullscreen, or stacking, add an integration test, usually in `tests/test_integration_focus.cpp` or `tests/test_integration_workspace.cpp`.

## Commit & Pull Request Guidelines

Recent history uses short imperative subjects such as `Stabilize fullscreen focus and restacking`; doc-only changes may use a prefix like `docs:`. Keep commits focused by subsystem. PRs should include: the problem being solved, behavior impact, exact test commands run, and reproduction/Xephyr steps for window-management changes. Screenshots are optional; for visual changes, before/after captures help.
