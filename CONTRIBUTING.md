# Contributing

Read [ARCHITECTURE.md](ARCHITECTURE.md) before changing state transitions and
[X11.md](X11.md) before changing client-visible X11 behavior.

## Build and test

```sh
make                 # release build
make debug           # debug build with invariant checks
make test            # build and run both test executables
```

For a direct CMake workflow:

```sh
cmake -S . -B build -DBUILD_TESTS=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j
ctest --test-dir build --output-on-failure
```

Run a focused Catch2 selection before the full suite, for example:

```sh
./build/tests/lwm_tests "[integration][focus]"
./build/tests/lwm_tests "subscription filter"
./build/tests/lwm_logging_tests
```

The X11 integration harness starts a private Xvfb server. Integration cases
skip when the harness cannot start an isolated server. Set
`LWM_TEST_ALLOW_EXISTING_DISPLAY=1` only when deliberately testing against the
current `DISPLAY`; the default protects a live desktop from test input.

Use `./scripts/preview.sh` for manual testing in Xephyr. It creates a debug
build, uses display `:100`, and seeds `test-config/config.toml` from
`config.toml.example` when the test config is absent.

## Change model

Keep state changes in the funnels described by [ARCHITECTURE.md](ARCHITECTURE.md):
visibility reconciliation owns physical visibility and fullscreen ownership,
`apply_stacking()` owns stacking, and `focus_any_window()` owns normal focus
changes. Prefer explicit domain state and pure policy functions over duplicated
guard logic.

At X event boundaries, use `get_client(window)` because the window may be
unmanaged or already destroyed. Inside a path that has established managed
ownership, use `require_client(window)` so an impossible missing client fails
at the actual invariant boundary.

Test through the real boundary:

- pure decisions belong in a `test_*_policy.cpp` or subsystem unit test;
- observable WM behavior belongs in an integration test using
  `tests/x11_test_harness.hpp`;
- logging lifecycle cases use `tests/log_probe.cpp` so each case has isolated
  process-global logger state.

If a behavior is difficult to test without mocking an internal WM component,
move the decision into a pure policy function and keep XCB, filesystem, and IPC
handling at the boundary.

## Documentation ownership

Update the one surface that owns the changed contract:

| Change | Source of truth |
| --- | --- |
| startup, install, or common usage | `README.md` and CLI help |
| configuration key, action, rule, or example | `config.toml.example` |
| runtime state or transition ownership | `ARCHITECTURE.md` |
| ICCCM, EWMH, or `_LWM_*` behavior | `X11.md` |
| socket command, response, JSON, or event | `IPC.md` |
| verified unfinished work | `ROADMAP.md` |

Tests are the executable specification. Source comments should explain only
local invariants or non-obvious rationale; do not duplicate an external
contract in comments or several documents.

## Style and review

Format C++ with the repository `.clang-format` and follow nearby naming:
`PascalCase` types, `snake_case` functions and files, and uppercase constants
and macros. Keep comments literal and local.

Before finishing a change:

1. Run the narrow relevant tests, then `make test`.
2. Exercise X11 behavior in Xephyr when geometry, input, focus, visibility, or
   stacking changed.
3. Update the owning documentation and remove superseded explanations.
4. Run `git diff --check` and inspect the final diff for unrelated changes.

Commit subjects are short and imperative. A pull request should state the
problem, externally observable behavior, tests run, and any manual Xephyr
reproduction.
