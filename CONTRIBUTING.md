# Development

Build prerequisites and installation are in [README.md](README.md#build).
[ARCHITECTURE.md](ARCHITECTURE.md) explains the model and transition ownership;
[X11.md](X11.md) and [IPC.md](IPC.md) define external contracts.

## Build and test

```sh
make                                  # Release binaries
make test BUILD_DIR=build/debug        # Debug, all CTest checks, required X11
make test BUILD_DIR=build/release TEST_BUILD_TYPE=Release
```

CMake owns dependencies and target requirements; CTest owns test discovery and
60-second per-case timeouts. Tests are opt-in with `BUILD_TESTING`. The Makefile
wraps these tools and supports Make or Ninja build directories. For direct use:

```sh
cmake -S . -B build/debug -DBUILD_TESTING=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build build/debug --parallel
LWM_TEST_REQUIRE_X11=1 ctest --test-dir build/debug --output-on-failure --no-tests=error
```

Select a non-default compiler at first configuration with
`-DCMAKE_CXX_COMPILER=/path/to/g++` or `-DCMAKE_TOOLCHAIN_FILE=/path/to/toolchain.cmake`.
With `make`, pass these through `CMAKE_OPTIONS`; `CMAKE` selects the CMake executable.
Ensure the matching `ctest` is on `PATH`. For multi-configuration generators, use
`--config Debug` when building and `-C Debug` with CTest.

Run a focused selection before the full suite:

```sh
LWM_TEST_REQUIRE_X11=1 build/debug/tests/lwm_tests '[integration][focus]'
LWM_TEST_REQUIRE_X11=1 build/debug/tests/lwm_tests '[integration][subscribe]'
build/debug/tests/lwm_logging_tests
```

Use a separate sanitizer build:

```sh
cmake -S . -B build/sanitize -DBUILD_TESTING=ON -DCMAKE_BUILD_TYPE=Debug -DLWM_SANITIZERS=ON
cmake --build build/sanitize --parallel
LWM_TEST_REQUIRE_X11=1 ctest --test-dir build/sanitize --output-on-failure --no-tests=error
```

Dependencies are pinned in [CMakeLists.txt](CMakeLists.txt). Update a revision and
its version comment together. `FETCHCONTENT_SOURCE_DIR_<NAME>` can reuse a local
checkout. C++26 and sanitizer requirements are target-scoped; reflect-cpp exports
its reflection flag to consumers. Other dependencies retain their build settings.
There is no GitHub Actions pipeline; run validation locally.

## Writing tests

Choose tests by the credible failure they detect. Test public behavior or an
independent contract; avoid mirrored implementations, test-only production hooks,
and repeated assertions at layers that add no distinct risk. A regression test
should fail on the broken implementation for the intended reason. Use a temporary
mutation when useful, restore it, and rerun the relevant checks.

Use these boundaries and fixtures:

| Contract | Fixture or approach |
| --- | --- |
| State and pure policy | `tests/state_fixture.hpp`; construct reachable states through `State` operations |
| X11 behavior | `TestEnvironment::create(config)` in `tests/x11_test_harness.hpp` |
| IPC transport | Real Unix sockets, partial writes, stalled peers, and acknowledgement ordering |
| Subscription content | `Subscriber` in `tests/ipc_subscription.hpp`; it waits for acknowledgement and retains partial lines |
| JSON | The independent nlohmann/json test parser |
| Restart handoff | `PausedRestart` in `tests/restart_handoff.hpp` to stop before the successor starts |
| Logging lifecycle | `tests/log_probe.cpp`, with a private journal collector and isolated logger lifetime |
| Fatal exceptions | `tests/runtime_failure_probe.cpp`, which interposes the first X event poll without a production hook |

Assert observable results: geometry and resize boundaries, actual X input focus and
`WM_TAKE_FOCUS`, synthetic ConfigureNotify counts, process exit status, and complete
protocol values. Agreement between two callers of one policy is not an independent
oracle. Use explicit expected outcomes and fixtures that distinguish plausible
alternatives. Check cardinality before range assertions; start negative cases from
valid states and violate only the intended relationship.

The harness starts a private Xvfb and the binaries from the selected CMake build.
Readiness requires a live supporting window and a successful ping to its socket.
Startup failures fail with captured stderr. Direct runs may skip if Xvfb is missing;
`LWM_TEST_REQUIRE_X11=1` makes that fatal. Unsupported server capabilities may still
skip. `LWM_TEST_ALLOW_EXISTING_DISPLAY=1` permits fallback to `DISPLAY`; use it only
with a disposable display because tests launch a WM and inject input.

### Synchronization and lifetime

Wait for the result under test, not a fixed delay. A round trip on the test
client's X connection does not prove the WM processed an event. For negative
assertions, `observe_title_after_events()` in `tests/wm_observations.hpp` places a
marker on the same connection and observes it through IPC. First establish window
management, and use titles that cannot trigger rules. This orders earlier events
on that connection only. Do not repeat actions inside polling predicates.

Use `wm_instance()` and `wait_for_wm_restart()` to distinguish WM lifetimes; X IDs
can be reused. `PausedRestart` allows topology or window changes during handoff.
Corrupt a current-format snapshot for malformed-adoption tests, with an intact
control; an obsolete version does not test current decoding. Codec round trips
and malformed-input tests complement actual adoption/exec tests. Tag composed
restart tests with `[restart]` wherever they live.

Use `TestFd` and the bounded process/socket helpers for cleanup on assertion
failure. `LwmProcess::wait_for_exit()` tests normal shutdown; `stop()` may force
termination. Use the CLI only when its process or output behavior is the contract;
its hidden subscription acknowledgement means readiness must be established by a
real event. Tests use private log destinations, never the host journal. Direct
Catch runs lack CTest's outer timeout, so fixture waits must remain bounded.

### Displays and generated sequences

For nested Xephyr, set `LWM_TEST_XSERVER=Xephyr`; `DISPLAY` must name a parent X
server, which remains unmanaged by the test. For topology changes, install the
Xorg dummy driver and `xrandr`, then run the otherwise hidden multi-output cases:

```sh
LWM_TEST_REQUIRE_X11=1 LWM_TEST_XSERVER=Xorg build/debug/tests/lwm_tests '[multioutput]'
```

This uses [tests/xorg-dummy.conf](tests/xorg-dummy.conf), with no physical devices.
It covers output add/reorder/remove/return and state rebinding, not real drivers
or every cross-monitor interaction.

To reproduce generated state/protocol sequences:

```sh
LWM_TEST_REQUIRE_X11=1 LWM_TEST_SEQUENCE_SEED=89372 LWM_TEST_SEQUENCE_STEPS=3000 \
  build/debug/tests/lwm_tests '[sequence]'
```

Both generators report their seed and action trace, accept seed zero, and reject
zero steps. State sequences check invariants and independently tracked live IDs;
protocol sequences cover preferences, classification, and restart. Multi-client
geometry and focus still need their own interaction tests.

## Performance checks

Use Release binaries, a quiet machine, and identical workloads for comparisons.
The scripts start owned Xvfb servers and report JSON. They require Linux, Python 3,
and libX11; the request tracer also needs `cc` and XCB headers. Each script's
`--help` lists its options.

```sh
python3 tests/performance/transition_counts.py build/release/src/app/lwm --check
python3 tests/performance/focus_cycle.py build/release/src/app/lwm
python3 tests/performance/ipc_load.py build/release/src/app/lwm --x-flood --check
python3 tests/performance/restart_resources.py build/release/src/app/lwm --check
python3 tests/performance/logging_bench.py --binary build/release/src/app/lwm
```

- `transition_counts.py` enforces X-request budgets for metadata, rules, placement,
  layout, workspaces, docks, and drag batching. Budgets live in the script. Synthetic
  motion controls batching; XTEST integration tests cover real grabs.
- `focus_cycle.py` measures completed IPC focus calls and distinct targets. Keep
  direction and client count fixed; `--transients chain|cycle` adds ancestry work.
- `ipc_load.py` checks concurrent callers with incomplete requests, continuous X
  traffic, a slow subscriber, and signal reload. Omit `--x-flood` for the simpler
  caller/backpressure case.
- `restart_resources.py` uses libXRes to check that repeated restart and failed-exec
  recovery release predecessor X clients, including an empty-display restart.
- `logging_bench.py` measures workspace transitions, idle cost, blocked output, and
  exec. `--baseline PATH` compares another binary. It defaults to stderr redirected
  to `/dev/null`; `--target journal` writes to the host journal, so use a disposable
  environment. `--affinity WM WORKER XSERVER DRIVER`, `--include-off`, and
  `--switches` control placement and workload.

Request counts, latency, CPU, and memory answer different questions. Repeat timing
runs and report log overflow/output errors alongside latency. These probes and test
timeouts establish regression evidence, not hard real-time or hardware guarantees.

## Nested preview

Install Xephyr and run from an existing X session:

```sh
./scripts/preview.sh
```

The script builds Debug in `build`, starts display `:100` (which must be free),
and seeds `test-config/config.toml` from the example if absent. Edit its commands
for installed applications. Diagnostics go to the terminal; press Enter to stop.
Start applications with `DISPLAY=:100 <program>`.

When available, Polybar uses [config/polybar.ini](config/polybar.ini). Adjust its
PulseAudio and battery settings (`BAT0`/`ACA0`). `scripts/launch-polybar.sh` is for
your desktop session: it replaces existing bars and starts one per output.

## Changes and documentation

Use the repository `.clang-format` and nearby naming: `PascalCase` types,
`snake_case` functions/files, and uppercase constants/macros. Comments should
explain local contracts or non-obvious reasons, not narrate code or recount history.

Keep each explanation with its owner:

| Subject | Document |
| --- | --- |
| Installation, startup, diagnostics | [README.md](README.md) |
| Configuration syntax, defaults, reload | [config.toml.example](config.toml.example) |
| Ownership, invariants, internal data flow | [ARCHITECTURE.md](ARCHITECTURE.md) |
| X11 application contract | [X11.md](X11.md) |
| IPC wire contract and consumer recovery | [IPC.md](IPC.md) |
| Build, tests, development workflow | This guide |

Before finishing, run focused tests and the full suite (`make test`), update the
owning documentation, and inspect `git diff --check` and the final diff. Exercise
Xephyr for geometry, input, focus, visibility, or stacking changes, and the dummy
Xorg cases for topology changes. For documentation/comment-only changes, verify
links, commands, examples, and described behavior; executable changes need the
corresponding tests. Commit subjects are short and imperative. PR descriptions
state the problem, resulting behavior, validation, and material limitations.
