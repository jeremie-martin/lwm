# Testing

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

Ensure the matching `ctest` is on `PATH`. For multi-configuration generators, use
`--config Debug` when building and `-C Debug` with CTest.

Focused runs:

```sh
LWM_TEST_REQUIRE_X11=1 build/debug/tests/lwm_tests '[integration][focus]'
LWM_TEST_REQUIRE_X11=1 build/debug/tests/lwm_tests '[integration][watch]'
build/debug/tests/lwm_logging_tests
```

Use a separate sanitizer build:

```sh
cmake -S . -B build/sanitize -DBUILD_TESTING=ON -DCMAKE_BUILD_TYPE=Debug -DLWM_SANITIZERS=ON
cmake --build build/sanitize --parallel
LWM_TEST_REQUIRE_X11=1 ctest --test-dir build/sanitize --output-on-failure --no-tests=error
```

There is no GitHub Actions pipeline; validation runs locally.

## Integration harness

The harness starts a private Xvfb and the binaries from the selected CMake build.
Readiness requires a live supporting window that owns `WM_S0` and answers `version`.
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

`send_ipc_command()` speaks the IPC protocol from a private X connection, as `lwmctl`
does. Its reply follows the command's effects on the server, but not events the test
sent on its own connection.

`Watcher` in `tests/state_watch.hpp` runs the real `lwmctl watch` and retains partial
lines; its first line is the state at attachment.

Use `wm_instance()` and `wait_for_wm_restart()` to distinguish WM lifetimes by their
`WM_S0` owner window. `PausedRestart` in `tests/restart_handoff.hpp` allows topology or
window changes during handoff. Corrupt a current-format snapshot for malformed-adoption tests, with an intact
control; an obsolete version does not test current decoding. Codec round trips
and malformed-input tests complement actual adoption/exec tests. Tag composed
restart tests with `[restart]` wherever they live.

`lwm_observation_probe` links the real WM and interposes XCB property delivery.
It writes through a separate X connection after the first read but before its reply
is delivered, testing admission subscription order without sleeps or production hooks.
The fixture also covers separate user-time windows and both live and cold admission.

Use `TestFd` and the bounded process helpers for cleanup on assertion
failure. `LwmProcess::wait_for_exit()` tests normal shutdown; `stop()` may force
termination. Use the CLI only when its process or output behavior is the contract.
Tests use private log destinations, never the host journal. Direct
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
python3 tests/performance/restart_resources.py build/release/src/app/lwm --check
python3 tests/performance/logging_bench.py --binary build/release/src/app/lwm
```

- `transition_counts.py` enforces X-request budgets for metadata, rules, placement,
  layout, workspaces, docks, and drag batching. Budgets live in the script. The script
  settles on the published `_LWM_STATE`, which costs the WM nothing, so each IPC
  command it counts costs exactly the read of its request. Synthetic
  motion controls batching; XTEST integration tests cover real grabs.
- `focus_cycle.py` measures completed IPC focus calls and distinct targets. Keep
  direction and client count fixed; `--transients chain|cycle` adds ancestry work.
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
and seeds `~/.cache/lwm-preview/config.toml` from the example if absent. Edit its
commands for installed applications. Diagnostics go to the terminal; press Enter to
stop, and everything the script started exits with it.
Start applications with `DISPLAY=:100 <program>`.

When available, Polybar uses [config/polybar.ini](config/polybar.ini), whose `lwm`
module follows `lwmctl watch` through `scripts/lwm-status.sh` (requires `jq`). Adjust
its PulseAudio and battery settings (`BAT0`/`ACA0`). `scripts/launch-polybar.sh` is for
your desktop session: it replaces existing bars and starts one per output.
