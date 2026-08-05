# Simplification Test Workflow Result

## Scope

Established black-box X11 coverage for the approved first slice in `SIMPLIFICATION_PROGRAM.md`: workspace-hidden clients must remain logically normal and physically hidden off-screen without unmap/remap churn, then become visible and focused when their workspace is selected.

The dynamic workflow independently inspected the architecture, existing integration coverage, and X11 harness, then challenged the proposed assertions for private-structure coupling, false confidence, timing dependence, and invalid property assumptions. It selected one focused regression rather than duplicating the existing fullscreen, showing-desktop, scratchpad, and property-transition cases.

## Test change

Updated `tests/test_integration_workspace.cpp` only:

- strengthened the existing focused-window-to-hidden-workspace integration case;
- observes only external X11 behavior: root-relative geometry intersection, `MapState`, validated ICCCM `WM_STATE`, `_NET_WM_STATE_HIDDEN`, `_NET_CLIENT_LIST` membership, `_NET_ACTIVE_WINDOW`, and actual `xcb_get_input_focus()`;
- selects `StructureNotify` and drains the initial map event, then verifies workspace transitions emit no `MapNotify`/`UnmapNotify` for the managed client;
- uses deadline-based polling rather than fixed sleeps;
- treats an absent `_NET_WM_STATE` property as valid absence of `HIDDEN`;
- verifies both test windows are present in the baseline client list before comparing membership after transitions.

No production, build-system, harness, or documentation source was modified. `git diff --check` is clean. The pre-existing `SIMPLIFICATION_PROGRAM.md` remains the companion study report.

## Verification

- `cmake --build build --target lwm_tests -j2` — passed.
- Focused case — passed: **1 test case, 23 assertions**.
- `./build/tests/lwm_tests '[integration][workspace]'` — passed: **16 cases, 125 assertions; 2 environment-dependent skips**.
- `./build/tests/lwm_tests '[integration][focus]'` — passed: **46 cases, 311 assertions; 3 environment-dependent skips**.
- `ctest --test-dir build --output-on-failure` — passed: **322/322, 0 failures**, 24.79 seconds; 13 tests reported skipped by their existing environment/feature conditions.

The suite output includes non-fatal XKB warnings from Xvfb. No test failure remained after correcting valid absence of `_NET_WM_STATE` and separating initial-event draining from transition-event assertions.

## Review disposition

The independent review found no private-helper or internal-field coupling, no fixed sleeps, and no production changes. It identified and the final diff fixed the deterministic absent-property failure. It also noted that the new workspace case alone does not characterize every policy-visible/physically-hidden combination; existing fullscreen-suppression integration cases already cover that axis, so no duplicate matrix was added. The final test is intentionally a focused protocol/visibility characterization, not a complete lifecycle or fullscreen test suite.
