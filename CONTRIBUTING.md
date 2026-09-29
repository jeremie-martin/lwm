# Contributing

Read [ARCHITECTURE.md](ARCHITECTURE.md) before changing state transitions and
[X11.md](X11.md) before changing client-visible X11 behavior.

## Build and test

```sh
make                 # release build
make debug           # debug build with invariant checks
make test            # Debug build, required X11 integration, logging, install-manifest check
```

For a direct CMake workflow:

```sh
cmake -S . -B build -DBUILD_TESTS=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j
LWM_TEST_REQUIRE_X11=1 ctest --test-dir build --output-on-failure
```

Run a focused Catch2 selection before the full suite, for example:

```sh
./build/tests/lwm_tests "[integration][focus]"
./build/tests/lwm_tests "[integration][subscribe]"
./build/tests/lwm_logging_tests
```

The X11 integration harness starts a private Xvfb server and launches the exact
binaries from its CMake build, independently of the working directory. WM
readiness requires a live supporting window and a successful ping through that
process's own socket, so stale root properties cannot satisfy it. Startup/readiness
failures fail the test and include captured stderr; they do not skip. Direct test
runs may skip when Xvfb is unavailable; `make test` sets `LWM_TEST_REQUIRE_X11=1`
to make that a failure. Capability-specific tests can
still skip when the isolated server lacks that capability. Missing built binaries,
failed atom creation, and unmet fixture requirements fail rather than skip.
`LWM_TEST_ALLOW_EXISTING_DISPLAY=1` permits fallback to the current `DISPLAY`
if the private server cannot start. Use it only with a disposable test display:
these tests launch a WM and may inject input.

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

For real multi-output RandR changes, install the Xorg dummy video driver
(`xserver-xorg-video-dummy` on Debian/Ubuntu) and `xrandr`, then run:

```sh
LWM_TEST_REQUIRE_X11=1 LWM_TEST_XSERVER=Xorg ./build/tests/lwm_tests "[multioutput]"
```

This starts an owned, rootless server using `tests/xorg-dummy.conf`, without
physical input devices or GPUs. The test enables, reorders, removes, and restores
outputs. It is hidden from the default suite because the dummy driver is optional;
run it for topology changes. This covers the X protocol path, not hardware/driver
behavior. The harness uses the server binary directly on Debian to avoid its
console-only wrapper.

For transition request budgets on an owned Xvfb server:

```sh
python3 tests/performance/transition_counts.py build/release/src/app/lwm --check
```

This optional Linux check needs Python 3, `cc`, XCB headers, and libX11. It builds
an LD_PRELOAD tracer in a temporary directory and counts only the WM's requests.
For 200 title changes, metadata-only updates must perform no geometry writes or
visibility/stacking reconciliation; sticky and sticky+fullscreen rule changes
allow at most 200 crossing barriers and 200 QueryTree requests. Sticky-only
changes must not rewrite unchanged geometry. A 200-client workspace workload
also bounds flush calls to catch completion work repeated for each configure
notification. Startup workloads with 10 and 40 docks bound property reads to
catch repeated workarea scans during adoption. Omit `--check` to compare an older
binary. These are protocol-work budgets, not latency measurements; measure
uninstrumented Release builds separately on an otherwise idle machine.

For independent IPC callers, run:

```sh
python3 tests/performance/ipc_load.py build/release/src/app/lwm --check
```

This uses an owned Xvfb server and 16 concurrent callers, both normally and with
an incomplete request already connected. All 512 pings must succeed in each
case. Reported latency is descriptive, not a pass/fail threshold. Omit `--check`
when comparing an older binary with the former single-client limit.

## Nested preview

For an interactive check, install Xephyr (`xorg-server-xephyr` on Arch Linux,
`xserver-xephyr` on Debian/Ubuntu) and run from an existing X session:

```sh
./scripts/preview.sh
```

The script builds Debug, uses display `:100` (which must be free), and seeds
`test-config/config.toml` from `config.toml.example` if absent. Edit that test
config to choose installed applications. It also starts `config/polybar.ini`
when Polybar is installed. Launch applications with `DISPLAY=:100 <program>`;
press Enter in the script's terminal to stop the preview.

The sample bar uses PulseAudio and battery names `BAT0`/`ACA0`; adjust its
modules for your machine. `scripts/launch-polybar.sh` starts a bar on each
connected output, replacing existing Polybar processes. It is intended for
your desktop session, not the isolated preview.

## Change model

Keep state changes in the funnels described by [ARCHITECTURE.md](ARCHITECTURE.md):
mutations update domain state and accumulate effects; `complete_transition()`
owns their ordered completion. Invalidate affected monitors for visibility/layout
changes, request geometry for rectangle-only changes, and use `focus_any_window()`
for focus intent. Do not add local reconciliation, property flushes, or crossing
barriers to feature handlers. Prefer explicit domain state and pure policy
functions over duplicated guard logic.

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
