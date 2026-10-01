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
cmake -S . -B build -DBUILD_TESTING=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j
LWM_TEST_REQUIRE_X11=1 ctest --test-dir build --output-on-failure
```

Run a focused Catch2 selection before the full suite, for example:

```sh
./build/tests/lwm_tests "[integration][focus]"
./build/tests/lwm_tests "[integration][subscribe]"
./build/tests/lwm_logging_tests
```

To check undefined behavior and memory safety, use a separate build:

```sh
cmake -S . -B build/sanitize -DBUILD_TESTING=ON -DCMAKE_BUILD_TYPE=Debug -DLWM_SANITIZERS=ON
cmake --build build/sanitize -j
LWM_TEST_REQUIRE_X11=1 ./build/sanitize/tests/lwm_tests
./build/sanitize/tests/lwm_logging_tests
```

Validate Release separately with `make test BUILD_DIR=build/release
TEST_BUILD_TYPE=Release`. The default `make test` explicitly selects Debug so invariant
checks cannot be silently disabled by an earlier Release configuration.

CMake owns target requirements and dependencies; CTest owns the test inventory and
timeouts. The Makefile is a convenience wrapper over those commands and works with
Make or Ninja build directories. Tests are opt-in with standard `BUILD_TESTING`
(replacing the former `BUILD_TESTS` option). For a multi-configuration generator, pass
`--config Debug` to the build command and `-C Debug` to CTest.

Dependency revisions are pinned in `FetchContent_Declare`; update the revision and
version comment together. CMake's `FETCHCONTENT_SOURCE_DIR_<NAME>` override can reuse
a local checkout without downloading it. C++26 and sanitizer flags are target usage
requirements for LWM and its test programs. reflect-cpp uses native reflection and
`std::expected`; its reflection flag propagates to consumers. Other third-party
targets keep their own build settings. Optimization flags come from the selected
toolchain/configuration and can be overridden with the usual CMake cache variables.

## Test fixtures and synchronization

The X11 integration harness starts a private Xvfb server and launches the exact binaries
from its CMake build, independently of the working directory. WM readiness requires a
live supporting window and a successful ping through that process's own socket, so stale
root properties cannot satisfy it. Startup/readiness failures fail the test and include
captured stderr; they do not skip. Direct test runs may skip when Xvfb is unavailable;
`make test` sets `LWM_TEST_REQUIRE_X11=1` to make that a failure. Capability-specific
tests can still skip when the isolated server lacks that capability. Missing built
binaries, failed atom creation, and unmet fixture requirements fail rather than skip.
`LWM_TEST_ALLOW_EXISTING_DISPLAY=1` permits fallback to the current `DISPLAY` if the
private server cannot start. Use it only with a disposable test display: these tests
launch a WM and may inject input.

Use `TestEnvironment::create(config)` for integration fixtures, and the shared bounded
process/socket helpers in `x11_test_harness.hpp`. Synchronize on the observable result
under test; a round trip on the test client's X connection does not prove that the WM
handled an event. Test layout rectangles and resize boundaries rather than the shape of
an internal data structure. IPC transport tests use real Unix sockets, including partial
writes and stalled clients. The subscription-order test fills the accepted socket through
the production poll interface until even a one-byte send returns `EAGAIN`, then checks
the real acknowledgement and events together after draining those disposable bytes.
IPC and restart JSON are checked with the test-only nlohmann/json parser, not a local parser or
production serialization helpers.

For negative X-event assertions, establish that the WM processed the request before
checking that state stayed unchanged. `observe_title_after_events()` in
`tests/wm_observations.hpp` writes a unique title on the same X connection and observes
it in the WM's IPC snapshot. First wait for management to be observable, for example
through `_LWM_WINDOW_CLASS`; a marker sent immediately after MapWindow can precede event
selection. Use fixtures whose titles do not participate in rules. It establishes
handling of earlier events on that connection, not completion of unrelated asynchronous
work. Prefer the actual changed outcome for positive assertions; do not issue actions
repeatedly inside polling predicates. Restart tests use `wm_instance()` and
`wait_for_wm_restart()` to observe a new IPC instance before checking restored state. X
resource IDs can be reused immediately after disconnect and cannot identify a distinct
WM lifetime.

Shared property readers validate X replies and property types. Use optional values for
legitimately absent properties and require values that are part of the contract; a
missing property must not silently become zero or an empty list. Use
`LwmProcess::wait_for_exit()` and inspect the wait status to test normal shutdown.
`stop()` is best-effort fixture cleanup and may force termination. `run_lwmctl()` clears
inherited socket overrides before selecting the fixture.

Subscription tests use `Subscriber` in `tests/ipc_subscription.hpp`: it waits for the
real server acknowledgement and retains coalesced and partial lines. Use the actual CLI
only when its process or output behavior is the contract. Since the CLI hides
acknowledgements, establish its readiness by receiving a real event; do not assume a
fixed sleep means it has subscribed. Test children and descriptors must be released even
when an assertion fails. `TestFd` in `tests/test_resources.hpp` also protects
descriptors acquired during fixture construction. CTest (including `make test`) enforces
a 60-second per-case timeout; direct Catch runs rely on bounded fixture operations.

## Display and topology validation

Set `LWM_TEST_XSERVER=Xephyr` to run the same integration tests in an owned nested
Xephyr server; `DISPLAY` must point to its parent X server. This does not manage the
parent display. The parent may itself be a private Xvfb server.

For real multi-output RandR changes, install the Xorg dummy video driver
(`xserver-xorg-video-dummy` on Debian/Ubuntu) and `xrandr`, then run:

```sh
LWM_TEST_REQUIRE_X11=1 LWM_TEST_XSERVER=Xorg ./build/tests/lwm_tests "[multioutput]"
```

This starts an owned, rootless server using `tests/xorg-dummy.conf`, without physical
input devices or GPUs. The test enables, reorders, removes, and restores outputs. It is
hidden from the default suite because the dummy driver is optional; run it for topology
changes. Current cases cover output add/reorder/remove/return, workspace preservation,
floating rebind, and fullscreen migration. They do not exhaust cross-monitor input,
scratchpad, or dock combinations, and do not validate physical hardware or driver
behavior. The harness uses the server binary directly on Debian to avoid its
console-only wrapper.

## Performance and load validation

For transition request budgets and focus-cycle latency on an owned Xvfb server:

```sh
python3 tests/performance/transition_counts.py build/release/src/app/lwm --check
python3 tests/performance/focus_cycle.py build/release/src/app/lwm
```

The request probe needs Linux, Python 3, `cc`, XCB headers, and libX11. It builds an
LD_PRELOAD tracer and counts WM-side requests after completed operations. `--check`
enforces the budgets in `tests/performance/transition_counts.py`:

| Workload | What the budget protects |
| --- | --- |
| Metadata, sticky, and fullscreen rule changes | No geometry work for metadata-only changes; bounded visibility and stacking reconciliation |
| Tiled and floating relocation | Geometry and membership update without fresh geometry reads or repeated reconciliation |
| Split ratios | Layout updates without property reads or stacking queries |
| Workspace changes | Flushes are bounded independently of per-window ConfigureNotify events |
| Dock adoption | Shared root-geometry read and bounded property reads as dock count grows |
| Individual and batched drag motions | Final preview geometry with bounded writes and reconciliation |

These probes use synthetic motion events for controlled batching; integration tests
separately exercise XTEST button grabs. Omit `--check` to record counters. Request
counts measure protocol work, not latency. Measure uninstrumented Release builds on an
otherwise idle machine when comparing latency.

Continuous X traffic, slow subscribers, incomplete requests, and SIGHUP are exercised
together with independent IPC callers:

```sh
python3 tests/performance/ipc_load.py build/release/src/app/lwm --x-flood --check
LWM_TEST_REQUIRE_X11=1 LWM_TEST_SEQUENCE_SEED=89372 LWM_TEST_SEQUENCE_STEPS=3000 build/tests/lwm_tests '[sequence]'
```

The load probe starts a separate continuous X producer, waits for actual subscription
acknowledgements, leaves one subscriber unread, and checks that requests and a signal
reload finish while the producer remains alive. It reports completed-call latency,
process CPU time and RSS; these are observations, not universal latency thresholds. The
generated sequence test reports its seed and complete action trace on failure, checks
one client's protocol state against an independent preference model, and detects
unexpected process-instance changes. It covers preferences, classification, and restart;
multi-client geometry, focus, and placement require their own interaction tests. A
separate unit-level sequence test drives thousands of `State` operations, varies output
and workspace counts, and checks model invariants and independently tracked live client
IDs after each. Both generated tests honor `LWM_TEST_SEQUENCE_SEED` and
`LWM_TEST_SEQUENCE_STEPS`, including seed zero, and reject zero steps. The unit test
defaults to seed 12345 and 4,000 operations, and reports the seed, step, and
operation/draw trace on invariant failure.

Tests and performance probes are run locally; this repository has no GitHub Actions
pipeline. These checks do not replace sustained desktop use with real drivers and
applications.

The focus-cycle benchmark reports completed IPC round-trip latency and distinct targets
on an owned Xvfb display, with 10, 100, and 500 floating clients. Compare Release builds
under similar load; this includes IPC, focus publication, and server interaction, not
just selection. Latency is reported rather than used as a machine-dependent test
threshold. A full traversal should visit every client. The default direction is `prev`;
use `--direction next` for forward traversal. Keep direction and workload identical when
comparing builds. `--transients chain` and `--transients cycle` add reverse-registration
parent relationships before measuring. For example, `--clients 10 40 100 --operations
200 --transients cycle` exercises malformed cyclic hints. Compare ordinary clients for
ordinary focus latency; transient workloads also exercise dependency ordering.

To detect retained X-server clients across repeated restart and failed-exec recovery
(also requires the libXRes runtime library):

```sh
python3 tests/performance/restart_resources.py build/release/src/app/lwm --check
```

This uses the X Resource extension to compare client counts before and after 20 WM
reconstructions. Completion is established through the IPC instance ID; resource counts
must return to the baseline. A final restart with the observer disconnected checks that
an otherwise empty X server preserves workspace state.

For independent IPC callers, run:

```sh
python3 tests/performance/ipc_load.py build/release/src/app/lwm --check
```

This uses an owned Xvfb server and 16 concurrent callers, both normally and with an
incomplete request already connected. All 512 pings must succeed in each case. Reported
latency is descriptive, not a pass/fail threshold. Omit `--check` to collect
measurements without enforcing the success-count requirement.

The logging comparison runs real workspace transitions with 40 clients, reports IPC
percentiles and WM CPU, measures idle worker cost, and fills stderr to test
backpressure, restart acknowledgement, and actual exec completion. Use Release builds
and an otherwise quiet machine:

```sh
python3 tests/performance/logging_bench.py --binary build/release/src/app/lwm
# Optional comparison against another saved executable:
python3 tests/performance/logging_bench.py --binary build/release/src/app/lwm --baseline /path/to/old/lwm
```

The default workload sends stderr to `/dev/null`. `--target journal` measures the native
journal path and writes to the system journal; use a disposable host or private mount
namespace with a draining journal socket for verbose runs. The blocked-output case
always uses a private stderr pipe.

Results are JSON Lines. `--affinity WM WORKER XSERVER DRIVER` accepts four Linux CPU IDs
to control placement (use separate physical cores); `--include-off` measures the
implementation without its worker. Increase `--switches` to reduce the effect of CPU
accounting's tick resolution. Compare repeated runs, report overflow/error status
alongside latency, and distinguish producer/library microbenchmarks from whole-WM
results. This comparison includes the logging policy and call-site changes; it cannot
isolate library overhead or establish a hard latency guarantee.

## Nested preview

For an interactive check, install Xephyr (`xorg-server-xephyr` on Arch Linux,
`xserver-xephyr` on Debian/Ubuntu) and run from an existing X session:

```sh
./scripts/preview.sh
```

The script builds Debug, uses display `:100` (which must be free), and seeds
`test-config/config.toml` from `config.toml.example` if absent. Edit that test config to
choose installed applications. WM diagnostics go to the preview terminal. It also starts
`config/polybar.ini` when Polybar is installed. Launch applications with `DISPLAY=:100
<program>`; press Enter in the script's terminal to stop the preview.

The sample bar uses PulseAudio and battery names `BAT0`/`ACA0`; adjust its modules for
your machine. `scripts/launch-polybar.sh` starts a bar on each connected output,
replacing existing Polybar processes. It is intended for your desktop session, not the
isolated preview.

## Changing behavior

Follow the [state and completion model](ARCHITECTURE.md#lifecycle-and-transitions).
Handlers change `State` through its named operations and do not publish: completion
projects the whole model onto X and writes only differences. Do not add requests to
republish something, and do not store a value that can be derived from state. When a
projection needs an input outside `State`, mark the presentation dirty. When something
else changes an output LWM owns, forget that field of the `Output` record. Use
`focus_window()` for focus intent. A user-triggerable operation is an `Action` executed
by `execute()`, so key bindings and IPC cannot diverge.

At X event boundaries, use `state_.find(window)` because the window may be unmanaged or
already destroyed. Inside a path that has established managed ownership, use
`state_.require(window)` so an impossible missing client fails at the actual invariant
boundary.

### Test contracts

Test through the real boundary:

- pure decisions belong in a `test_*_policy.cpp` or subsystem unit test; build
  domain states through `State`'s own operations (`tests/state_fixture.hpp`), so a
  test starts from a state the WM can reach;
- observable WM behavior belongs in an integration test using
  `tests/x11_test_harness.hpp`; cover composed geometry transitions without an
  explicit placement override that would mask the default-geometry decision.
  Focus coverage observes both actual X input focus and delivered `WM_TAKE_FOCUS`
  messages, including accepted application timestamps and explicit same-window activation.
  ConfigureRequest coverage counts synthetic replies after observable completion,
  including requests that do and do not change geometry;
- fatal exception handling uses `lwm_runtime_failure_probe`, which links the real
  application entry point and WM and substitutes the first X event-loop poll with
  a throwing function. Production has no injection hook. The test verifies failure
  exits and releases ownership; real failed-exec tests separately verify recovery;
- logging lifecycle cases use `tests/log_probe.cpp` so each case has isolated
  process-global logger state. That probe alone redirects libsystemd's journal
  socket address to a private collector; it uses the real library encoding and
  kernel I/O. Integration WMs use stderr files. No tests send diagnostics into
  the host journal. Tests cover copied arguments, metadata, level gates,
  standard escaping/color, oversized records, overflow, absent journal, closed
  pipes, descriptor inheritance, and failed exec. Saturated pipe and journal tests prove submission
  completes without a reader and shutdown resumes once the reader drains.
  A full-WM test verifies IPC/workspace responsiveness during a stalled console
  and actual exec completion while output remains blocked. A Debug-only probe
  verifies invariant failure aborts even with a blocked sink. Time bounds detect regressions; they
  are not hard real-time guarantees.

Choose tests by the failure they detect, not by test count. Policy tests and integration
tests may cover the same feature when they protect distinct risks; do not duplicate a
private helper's implementation when an observable contract already owns that behavior.
Start negative cases from valid state and violate only the intended relationship where
possible. Otherwise, a different validation check can hide the missing behavior.

The restart snapshot is private to one format version: test that every field
round-trips, that other formats and every truncation are rejected, and keep actual
adoption and exec tests, because codec tests cannot establish that the WM applies
decoded state. Malformed-handoff integration cases corrupt a snapshot produced by the
running WM, alongside an intact-handoff control; obsolete literal versions cannot
stand in for current-format truncation. Tag composed restart scenarios with `[restart]`
even when they live in another subsystem's test file. `PausedRestart` uses a test-only
exec replacement that stops before starting the successor. Use it to create windows or
change RandR outputs during the handoff; `waitpid` establishes the boundary and the test explicitly resumes startup.
Require nonempty/cardinality checks before range assertions. Choose policy fixtures that
distinguish competing outcomes: history order should differ from insertion order, and
invalid rules must actually be selected. Literal protocol examples should check decoded
argument values as well as command names. For important regressions, check the test
against the broken revision or a focused, temporary mutation of the relevant behavior.
Confirm it fails at the intended assertion, then restore production code and run the
normal checks. When two paths share a policy helper (such as live topology reconciliation
and restart rebinding), agreement between them is not an independent oracle. Anchor the
comparison with explicit expected outcomes for the shared contract.

If a decision is independently meaningful, a pure policy function can make it simpler to
test. Keep transport, ordering, and lifecycle tests at real boundaries; do not add
production hooks or abstractions solely to make a test convenient.

## Documentation ownership

Update the one surface that owns the changed contract:

| Change | Source of truth |
| --- | --- |
| startup, install, or common usage | `README.md` and CLI help |
| configuration key, action, rule, or example | `config.toml.example` |
| runtime state or transition ownership | `ARCHITECTURE.md` |
| ICCCM, EWMH, or `_LWM_*` behavior | `X11.md` |
| socket command, response, JSON, or event | `IPC.md` |

Tests are the executable specification. Source comments should explain only local
invariants or non-obvious rationale; do not duplicate an external contract in comments
or several documents.

## Style and review

Format C++ with the repository `.clang-format` and follow nearby naming: `PascalCase`
types, `snake_case` functions and files, and uppercase constants and macros. Keep
comments literal and local.

Before finishing a change:

1. Run the narrow relevant tests, then `make test`.
2. Exercise X11 behavior in Xephyr when geometry, input, focus, visibility, or
   stacking changed.
3. Update the owning documentation and remove superseded explanations.
4. Run `git diff --check` and inspect the final diff for unrelated changes.

Commit subjects are short and imperative. A pull request should state the problem,
externally observable behavior, tests run, and any manual Xephyr reproduction.
