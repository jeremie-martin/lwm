# Contributing

Read [ARCHITECTURE.md](ARCHITECTURE.md) before changing state transitions and
[X11.md](X11.md) before changing client-visible X11 behavior.

## Build and test

```sh
make                 # release build
make debug           # debug build with invariant checks
make test            # Debug build, required X11 integration, both test executables
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

The X11 integration harness starts a private Xvfb server and launches the exact
binaries from its CMake build, independently of the working directory. WM
startup/readiness failures fail the test and include captured stderr; they do
not skip. Direct test runs may skip when Xvfb is unavailable; `make test` sets
`LWM_TEST_REQUIRE_X11=1` to make that a failure. Capability-specific tests can
still skip when the isolated server lacks that capability. Set
`LWM_TEST_ALLOW_EXISTING_DISPLAY=1` only when deliberately testing against the
current `DISPLAY`; the default protects a live desktop from test input.

To check undefined behavior and memory safety, use a separate build:

```sh
cmake -S . -B build/sanitize -DBUILD_TESTS=ON -DCMAKE_BUILD_TYPE=Debug -DLWM_SANITIZERS=ON
cmake --build build/sanitize -j
LWM_TEST_REQUIRE_X11=1 ./build/sanitize/tests/lwm_tests
./build/sanitize/tests/lwm_logging_tests
```

Validate Release separately with `make test BUILD_DIR=build/release TEST_BUILD_TYPE=Release`.
The default `make test` explicitly selects Debug so invariant checks cannot be
silently disabled by an earlier Release configuration.

Use `TestEnvironment::create(config)` for integration fixtures, and the shared
bounded process/socket helpers in `x11_test_harness.hpp`. Synchronize on the
observable result under test; a round trip on the test client's X connection
does not prove that the WM handled an event. Test layout rectangles and resize
boundaries rather than the shape of an internal data structure. IPC transport
tests use real Unix sockets, including partial writes and stalled clients. IPC
JSON is checked with the test-only nlohmann/json parser, not a local parser or
production serialization helpers.

Set `LWM_TEST_XSERVER=Xephyr` to run the same integration tests in an owned
nested Xephyr server; `DISPLAY` must point to its parent X server. This does not
manage the parent display. The parent may itself be a private Xvfb server.

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
