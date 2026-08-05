# LWM Simplification Program

_Date: 2026-08-04_

## Executive summary

LWM already has a deliberate small-state model: `Client` owns per-window runtime state, `clients_` is the unified managed registry, `Workspace::windows` owns tiled membership, and visibility, focus, fullscreen ownership, and stacking each have named transition authorities. The most valuable simplifications are therefore local cleanups that remove duplicated decisions without merging those authorities.

The recommended first code slice is the exact algebraic cleanup of `WindowManager::is_physically_visible()`. It removes a duplicated visible-scope computation, preserves the hidden-versus-policy-visible distinction, and does not alter an X request, protocol property, geometry decision, or focus transition. It should follow a clean baseline under an owned Xvfb.

The next useful slice is to route all already-visible floating-geometry realization through the existing `apply_visible_floating_geometry()` helper. Runtime ABOVE/BELOW handling and dead policy-code removal are smaller follow-on slices, but each needs targeted characterization first. Larger lifecycle, movement, restart, and representation refactors do not currently meet the bar for a small simplification program.

This report was produced from independent architecture, implementation, test, and documentation studies, followed by adversarial challenge passes focused on behavior, observability, and abstraction churn. No source files were changed.

## Study baseline and constraints

- `ARCHITECTURE.md` is the internal source of truth for state ownership and visibility/focus/fullscreen transition funnels.
- `COMPLIANCE.md` is normative for ICCCM/EWMH behavior; `IPC.md` owns the local socket and event contract; `CONTRIBUTING.md` defines the test boundary and change checklist.
- The policy baseline observed during the study passed 62 cases and 2,153 assertions.
- X11 integration availability is not a reliable implicit gate. The harness can skip when Xvfb/readiness is unavailable, and `tests/CMakeLists.txt` registers Catch2 return code 4 as `SKIP_RETURN_CODE`; an all-skipped integration selection can therefore appear green to CTest. Some study runs also encountered a pre-existing shared-display `:99` lock. Integration claims below mean behavioral coverage when run under a clean, owned Xvfb, not merely a successful CTest invocation.
- Existing integration coverage is broad around focus, workspaces, fullscreen, properties, scratchpads, restart, and classification, but it does not fully observe ICCCM `WM_STATE`, `_NET_CLIENT_LIST`, `_NET_WORKAREA`, withdrawal, all map/unmap event payloads, or two-output movement.

## Ordered program

### 0. Keep the contract map truthful (documentation-only hygiene)

This is supporting work, not a runtime refactor. It should be kept separate from code changes so documentation corrections cannot be mistaken for behavior changes.

**Current concrete complexity.** The authoritative documentation currently makes several searches and ownership decisions harder than necessary:

- `ARCHITECTURE.md` names `restack_monitor_layers(...)`, while the implementation's global authority is `apply_stacking()` (`ARCHITECTURE.md:134-137,164-165,271-274`; `src/lwm/wm.cpp:3386-3462`).
- `COMPLIANCE.md` describes initial maximize ordering generally, while tiled and floating manage paths intentionally differ (`COMPLIANCE.md:186-194`; `src/lwm/wm.cpp:2322-2347`; `src/lwm/wm_floating.cpp:216-242`).
- `mru_order` is documented as floating-only even though focus cycling updates and ranks tiled and floating clients; only floating ordering is serialized for restart (`src/lwm/core/types.hpp:253-258`; `src/lwm/wm_focus.cpp:94-125`; `src/lwm/core/policy.hpp:193-227`; `src/lwm/wm_restart.cpp:240-245`).

**Simpler design.** Correct those descriptions in place: name `apply_stacking()` as the global stacking authority; state that fullscreen is pre-map for both kinds, floating maximize is pre-map, tiled maximize is post-map because layout owns tiled geometry; and describe `mru_order` as in-memory focus recency with floating-specific restart persistence.

**Observable behavior to preserve.** None changes. These edits must not rename implementation symbols, change ordering, or alter restart atoms.

**Is existing behavioral integration coverage sufficient?** Yes for a documentation-only correction, provided the source is inspected and the stale names are removed with a repository-wide grep. The relevant policy and focus tests remain regression guards; no new runtime behavior is introduced.

**Tests/checks required before changing it.** Run:

- `git grep -n 'restack_monitor_layers' -- '*.md' 'src' 'tests'` and verify only intended stale references exist before editing;
- the stacking and focus-policy subsets;
- initial fullscreen/maximize integration cases under an owned Xvfb if available.

**Risks and documentation implications.** Do not fold the hotplug contract into this documentation slice. `ARCHITECTURE.md` currently promises a comprehensive final stale-index sweep, while the implementation resets monitor-indexed fullscreen geometry, performs named rebinds, reconciles, and relies on debug invariants without that release-path sweep (`ARCHITECTURE.md:251-265`; `src/lwm/wm_events.cpp:1683-1687,1741-1856`; `src/lwm/core/invariants.hpp:25-30`). Deciding whether to restore that behavior or intentionally revise the invariant is a separate design decision, not a factual wording cleanup.

### 1. Derive physical visibility from the canonical scope predicate — **recommended first code slice**

**Current concrete complexity.** `should_be_visible()` delegates to `visibility_policy::is_window_visible()`. `is_physically_visible()` separately checks `hidden` and repeats the complete policy call (`src/lwm/wm.cpp:3278-3303`). This duplicates the visible-scope decision and creates two bodies that can drift.

**Simpler design.** Keep the hidden check first and express the predicate directly:

```cpp
return !client.hidden && should_be_visible(client);
```

`hidden` remains the physical off-screen state; `should_be_visible()` remains the visible-scope authority.

**Observable behavior to preserve.** Preserve short-circuit behavior for a hidden client, plus all existing iconic, sticky, showing-desktop, workspace, fullscreen-suppression, and invalid-monitor policy behavior. This must not change focus eligibility, stacking, off-screen movement, EWMH state, or geometry realization.

**Is existing behavioral integration coverage sufficient?** Sufficient for this isolated algebraic cleanup when combined with the policy suite and focused X11 regressions. The policy tests cover visible-scope decisions, and integration tests exercise hidden floating windows, workspace visibility, sticky behavior, fullscreen suppression, and focus fallback. There is no reason to add a symbol-level unit test for a private wrapper, but the black-box transitions must run under a real X server.

**Tests required before changing it.** Capture a clean baseline and rerun:

- `./build/tests/lwm_tests '[policy]'`;
- workspace visibility and hidden-floating integration cases;
- focus/focus-follows-mouse, fullscreen, scratchpad, and property-reclassification cases;
- the full CTest suite under an uncontended, owned Xvfb.

If the integration environment is unavailable, record that as an unavailable behavioral baseline rather than treating skipped cases as passing coverage.

**Risks and documentation implications.** The risk is scope creep: do not replace every manual `!hidden && should_be_visible` or policy-only guard in the same change. Map-request and reconciliation paths intentionally distinguish desired visibility from physical visibility (`ARCHITECTURE.md:37-70`; `src/lwm/wm.cpp:4143-4184`; `src/lwm/wm_events.cpp:1365-1499`). No public documentation change is required.

### 2. Make visible floating geometry realization one authoritative funnel

**Current concrete complexity.** `apply_visible_floating_geometry()` already contains the correct guard and priority (`src/lwm/wm_floating.cpp:332-343`), but equivalent fullscreen/maximize/ordinary-floating branches remain in restart scanning, rule application, classification re-evaluation, and visibility reconciliation (`src/lwm/wm.cpp:1421-1441,1730-1752,2150-2166,4163-4174`). The same decision is consequently maintained in several places.

**Simpler design.** Replace only those realization branches with calls to `apply_visible_floating_geometry(Client&)`. Keep state mutation (`set_fullscreen()`, maximize state, kind conversion, restore geometry) in its current owners. Keep tiled geometry and tiled fullscreen handling in `rearrange_monitor()`; the floating helper should only realize a decision for a policy-visible, non-hidden floating client.

**Observable behavior to preserve.** Preserve the exact priority `fullscreen > maximized > stored floating geometry`. Hidden, iconic, off-workspace, showing-desktop, suppressed, non-floating, and not-yet-visible clients must not receive an ordinary visible configure. Preserve workspace/sticky visibility, fullscreen-owner suppression, monitor relocation, restart restoration, placement rules, stacking, and caller ordering.

**Is existing behavioral integration coverage sufficient?** Not yet for a broad multi-call-site replacement. Existing tests cover visible floating rule geometry, hidden floating `ConfigureRequest` behavior, fullscreen/maximize interactions, scratchpad and restart geometry, workspace visibility, and property reclassification. They do not cover the full hide/show, iconify/deiconify, showing-desktop, suppression, monitor-relocation, fullscreen/maximize, and restart matrix in one behavioral contract.

**Tests required before changing it.** Add an Xvfb integration matrix that records a floating client's geometry, then exercises workspace hide/show, iconify/deiconify, showing-desktop, fullscreen and maximize enter/exit, suppression by a visible fullscreen owner, monitor relocation when available, and restart restoration. Assert final geometry and off-screen/hidden state; observe configure ordering where practical. Include a tiled fullscreen control case. Migrate one realization site at a time and rerun the focused suite after each site.

**Risks and documentation implications.** Do not collapse policy visibility and physical visibility, broaden the helper's monitor-index contract, or hold a stale `Client&` across nested transitions. `ARCHITECTURE.md` should continue to describe visibility reconciliation separately from geometry arrangement; `IPC.md` and `COMPLIANCE.md` should not change if behavior is preserved.

### 3. Centralize runtime ABOVE/BELOW preference transitions

**Current concrete complexity.** `handle_wm_state_change()` has nearly identical ABOVE and BELOW branches (`src/lwm/wm_events.cpp:1110-1137`). Each computes ADD/REMOVE/TOGGLE, mutates one preference and clears the opposite, calls `reevaluate_managed_window()`, reacquires the client, and publishes effective atoms.

**Simpler design.** Add one small helper parameterized by the requested layer atom. It should update mutually exclusive `app_prefs`, invoke the existing re-evaluation funnel, reacquire the client afterward, and echo effective `layer_hint`. Keep `app_prefs` (requested application state) separate from `layer_hint` (effective policy state), and process two atoms in the existing order.

**Observable behavior to preserve.** Preserve ADD, REMOVE, and TOGGLE semantics; mutual exclusion; rule overrides; modal clearing of BELOW; fullscreen interactions; and authoritative effective-atom replies. Re-evaluation may reclassify or relocate the client, so pointer reacquisition is mandatory.

**Is existing behavioral integration coverage sufficient?** Partial, not sufficient as a precondition. Existing tests cover individual add/remove/toggle requests, mutual exclusion, initial conflicts, modal precedence, and reclassification. They do not cover both layer atoms in one message or an overridden effective reply after a runtime request.

**Tests required before changing it.** Add integration tests for a client message containing both ABOVE and BELOW, and for a rule-overridden layer request that checks both effective atoms after ADD/REMOVE/TOGGLE. Include modal and fullscreen combinations. Run the WM-state and property-reclassification groups under a clean Xvfb.

**Risks and documentation implications.** A helper that merges requested and effective state would be a regression, not a simplification. `COMPLIANCE.md` already documents the public semantics; leave it unchanged unless observable precedence changes.

### 4. Remove the uncalled `workspace_policy::move_tiled_window`

**Current concrete complexity.** `workspace_policy::move_tiled_window()` is implemented and substantially unit-tested (`src/lwm/core/policy.hpp:387-409`; `tests/test_workspace_policy.cpp:53-200`) but has no production caller. The live authority is `WindowManager::move_tiled_client_to_workspace()` (`src/lwm/wm.cpp:3900-3942`), which additionally handles EWMH desktop projection, insertion, focus-history repair, cross-monitor movement, and visibility finalization. The repository therefore has a second, narrower movement implementation that looks authoritative but is not runtime behavior.

**Simpler design.** After a production-reference check, delete only the uncalled policy function and its dedicated tests. Retain `remove_tiled_window()`, `fixup_workspace_focus()`, and the live `WindowManager` movement path. Do not replace the live path with the narrower helper.

**Observable behavior to preserve.** No runtime behavior should change. Live moves must retain workspace membership, insertion/reordering, `_NET_WM_DESKTOP`, source/target visibility, focus fallback, hidden-workspace behavior, and any cross-monitor handling.

**Is existing behavioral integration coverage sufficient?** No. Existing integration coverage includes a focused-window move, but the current unit tests mostly exercise the dead helper rather than the production path. This deletion should wait until the live path is characterized.

**Tests required before changing it.** Add live-path integration cases for focused and non-focused tiled windows, moves to hidden and visible workspaces, the configured keybind path via XTEST, same-workspace/reorder behavior if supported, and cross-monitor behavior when a two-output harness exists. Assert membership, `_NET_WM_DESKTOP`, focus, visibility, and event ordering. Confirm with a repository-wide reference search that only the intended dead function and tests are removed; there is no public move IPC command to test.

**Risks and documentation implications.** The main risk is deleting `remove_tiled_window()` or treating policy coverage as production coverage. Keep the live movement helpers and update `CONTRIBUTING.md` only if its test map names removed tests.

### 5. Share only the pure hotplug monitor-name resolver — conditional later slice

**Current concrete complexity.** `hotplug_policy::plan_hotplug()` creates a monitor-name map and fallback resolver, while `handle_randr_screen_change()` creates another map and fallback for dock/desktop rebinding (`src/lwm/core/policy.hpp:533-604`; `src/lwm/wm_events.cpp:1812-1827`). The relocation records must remain different, but the name-to-index rule is duplicated.

**Simpler design.** Extract a tiny pure resolver or shared name-map utility that preserves last-entry-wins duplicate semantics and fallback to monitor zero. Reuse it only for lookup; keep tiled/floating relocation planning, dock/desktop rebinding, workspace clamping, and geometry application separate.

**Observable behavior to preserve.** Monitor names, not old indices, remain the relocation key. Missing names still resolve to monitor zero; empty-monitor handling, workspace clamping, floating restoration, and dock/desktop best-effort semantics remain unchanged. No fullscreen or visibility ownership should move into this utility.

**Is existing behavioral integration coverage sufficient?** No. Pure tests cover ordinary name retention and missing-name fallback, but not duplicate names, empty names, or the dock/desktop apply loop. The current integration coverage does not fully exercise dock/desktop clients through a RandR rebuild.

**Tests required before changing it.** Add pure cases for duplicate names, missing names, empty monitor sets, and empty names. Add an X11/RandR integration check that dock and desktop clients retain their kinds, client-list membership, and valid rebinding. Do not make this a justification for a broader hotplug transaction.

**Risks and documentation implications.** This is optional because a very small shared utility can cost more conceptual surface than the two duplicated maps. Do not combine the relocation data structures or revise the unresolved hotplug contract in the same change. `ARCHITECTURE.md` remains the contract authority until the separate release-safety decision is made.

### 6. Narrow dock/desktop registration helper — conditional later slice

**Current concrete complexity.** `map_desktop_window()` and `map_dock_window()` duplicate absent-client construction, kind assignment, skip-taskbar/pager defaults, order allocation, and class publication (`src/lwm/wm_events.cpp:298-343`). Their genuinely different event masks, mapping, stacking, strut, arrangement, and flushing behavior is interleaved with that common registry work.

**Simpler design.** Add a deliberately narrow helper for constructing and registering an absent container `Client`, assigning its kind/defaults/order, and publishing `_LWM_WINDOW_CLASS`. Leave event-mask selection, map and stack operations, `update_struts()`, arrangement, `_NET_CLIENT_LIST` refresh, and flushing at the existing kind-specific call sites so order remains visible.

**Observable behavior to preserve.** Dock and desktop windows remain in `clients_` and `_NET_CLIENT_LIST`, retain distinct kinds and order values, remain non-focus-eligible, and preserve duplicate-registration no-op behavior. Dock struts/workarea and desktop-below stacking must remain unchanged.

**Is existing behavioral integration coverage sufficient?** No. The dock class path has coverage, but there is no paired desktop/class, client-list, non-focus, and dock-workarea contract.

**Tests required before changing it.** Add integration assertions for both container kinds in `_LWM_WINDOW_CLASS` and `_NET_CLIENT_LIST`, non-focus eligibility, and dock `_NET_WORKAREA`/strut recomputation. Run container classification and EWMH tests under a clean Xvfb before extracting only the common registration lines.

**Risks and documentation implications.** A callback-heavy generic map helper would replace duplication with indirection. Keep the helper narrow and preserve insertion/publication order. `COMPLIANCE.md` already owns client-list semantics; no public documentation change is expected.

### 7. Conditional lifecycle bootstrap extraction — not an initial refactor

**Current concrete complexity.** Tiled and floating management paths duplicate `Client` construction, initial property reads, event-mask and passive-grab setup, ICCCM/EWMH publication, client-list updates, key grabs, and mapping (`src/lwm/wm.cpp:2243-2348`; `src/lwm/wm_floating.cpp:9-242`). Their geometry, transient placement, initial-state ordering, visibility, urgency, and focus timing differ. Teardown has similar-looking tails, but `WM_STATE=Withdrawn`, membership removal, and focus fallback have observable ordering constraints.

**Simpler design if justified later.** First characterize the lifecycle. Then consider only two explicit common phases: a record factory for common metadata/property reads, and a narrowly specified registration phase for common bookkeeping. Leave mapping, classification, rule placement, tiled membership, floating geometry, focus, IPC events, and visibility finalization in their existing paths. Do not build a mode-flagged lifecycle framework or combine teardown into the first extraction.

**Observable behavior to preserve.** The documented manage order (`ARCHITECTURE.md:202-223`), floating-versus-tiled fullscreen/maximize ordering (`COMPLIANCE.md:186-194`), transient placement, `_NET_WM_DESKTOP`, initial `WM_STATE`/hidden state, `_NET_CLIENT_LIST`, `_LWM_WINDOW_CLASS`, passive grabs, startup/restart focus suppression, map/unmap events, and destruction fallback must remain identical.

**Is existing behavioral integration coverage sufficient?** No. Existing focus, state, workspace, property, class, and restart tests do not form a lifecycle matrix covering event masks, passive grabs, initial iconic state, client-list publication/removal, first-map geometry, and map/unmap payloads. The file named `test_integration_wm_state.cpp` primarily tests `_NET_WM_STATE`, not ICCCM `WM_STATE` values.

**Tests required before changing it.** Before any extraction, add real-X cases for normal tiled, dialog/floating, transient, initially iconic, fullscreen, and maximized clients. Check `WM_STATE`, `_NET_WM_STATE`, `_NET_WM_DESKTOP`, `_NET_CLIENT_LIST`, `_LWM_WINDOW_CLASS`, allowed actions, focus, geometry, and map/unmap subscription events. Add explicit withdrawal and list-removal checks. Only then extract a phase whose inputs and ordering can be stated without mode flags.

**Risks and documentation implications.** This is the final and most conditional item. If the extracted phase becomes a genuine authority, update the lifecycle funnel in `ARCHITECTURE.md`; otherwise do not add an abstraction merely to move duplication. Do not combine it with teardown, the restart codec, or a Client representation change.

## Ideas rejected or deliberately deferred

The challenge pass rejected the following as broad churn, moved complexity, or insufficiently characterized behavior:

- A `ClientLocation`/`MoveResult` transaction: tiled membership, floating placement, rule moves, transient rehosting, hotplug, and caller-specific focus/warp/drain semantics are intentionally different; the existing `assign_window_workspace()` already owns the common location write (`src/lwm/wm.cpp:3884-3977`).
- A typed restart codec: the positional arrays are a private compatibility wire format; a second representation would add risk before fixtures cover payload lengths, truncation, signed geometry, unknown bits, and legacy versions (`src/lwm/src/lwm/wm_restart.cpp:26-85,102-155,453-576`; `tests/test_integration_workspace.cpp:494-577`; `tests/test_integration_scratchpad.cpp:404-686`).
- An `InitialWindowState` snapshot: pre-map, post-map, rule, and restart reads are intentionally staged; a snapshot could change timing or make state stale.
- A single four-way `Client` discriminant replacing `Client::Kind` plus the tiled/floating variant: it touches nearly every branch, invariant, restart path, fixture, and container path, and is a representation migration rather than a small cleanup.
- Batch affected-monitor finalization: `finalize_move_visibility()` already preserves reconcile-before-arrange ordering and same-monitor handling (`src/lwm/wm.cpp:4102-4122`); a collection abstraction adds indirection before true two-output integration exists.
- Broad replacement of all manual visibility guards: it would erase the deliberate desired-versus-actual distinction and alter map-request/iconify ordering.
- Removing `sync_visibility_for_monitor()`: it is an exact forwarding wrapper, but its name documents the no-arrange phase alongside `reconcile_visibility_for_monitor()` and `finalize_visibility_on_monitor()` (`src/lwm/wm.cpp:4143-4201`; `src/lwm/wm.hpp:487-496`; `ARCHITECTURE.md:62-68`). Keep it unless the phase vocabulary is intentionally revised as a separate documentation/API cleanup.
- A shared integration session or global required-X11 gate: the fixture duplication is real, but per-test configuration, `owns_display`, specialized atom setup, and raw subscribe/socket behavior differ. CI skip policy is infrastructure work, not a runtime simplification program.
- A hotplug contract rewrite: the documentation/code mismatch requires an explicit release-safety decision; do not silently weaken a normative invariant while “correcting” prose.

## Recommended first slice

1. Capture the clean baseline under an owned Xvfb; record unavailable integration prerequisites explicitly.
2. Make only the `is_physically_visible()` body the conjunction of `!client.hidden` and `should_be_visible(client)`.
3. Rerun the policy suite, focused visibility/focus/fullscreen/scratchpad/property integration cases, and the full CTest suite.
4. Land the three factual documentation corrections separately: `apply_stacking()` authority, per-kind initial maximize ordering, and `mru_order`'s dual focus use.

This first slice removes duplicated policy knowledge while preserving all existing transition funnels and observable X11/IPC behavior. Do not combine it with floating realization, lifecycle, movement, restart, or test-harness changes.
