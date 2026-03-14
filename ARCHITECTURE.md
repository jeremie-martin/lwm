# LWM Architecture

This is the maintainer-oriented source of truth for how LWM is supposed to work. It explains the model, the state authority boundaries, and the transition funnels that should stay authoritative when behavior changes.

Use the docs as follows:

- [`README.md`](README.md): build, install, run, configure
- `ARCHITECTURE.md`: model, invariants, and high-value event flow
- [`COMPLIANCE.md`](COMPLIANCE.md): ICCCM/EWMH surface and limits
- [`CONTRIBUTING.md`](CONTRIBUTING.md): workflow, code map, tests, and debugging

## 1. Core Model

LWM manages one or more monitors. Each monitor owns its own workspace set, and exactly one workspace per monitor is current at any time.

Important scope rules:

- `focused monitor` is the command target
- workspace switching is per monitor, not global
- sticky windows are sticky within their owning monitor, not across all monitors
- popup and ephemeral X11 window types are mapped directly and are not part of the normal workspace/layout model

Managed window classes:

- `Tiled`: belongs to one workspace layout
- `Floating`: keeps independent geometry but still belongs to one monitor/workspace. Managed transients live here too; they prefer parent monitor/workspace and may use parent geometry for initial placement.
- `Dock`: managed, strut-reserving, always visible, not normal-focus eligible
- `Desktop`: managed, bottom layer, not normal-focus eligible

## 2. State Authority

The single most important rule in this codebase is that different pieces of state have different owners.

- `Client` is the authoritative per-window runtime state.
- `clients_` is the full managed-window registry.
- `Workspace::windows` is the authoritative tiled membership for that workspace.
- `floating_windows_` is the authoritative floating MRU list and floating membership container.
- `active_window_` is the current focused managed window.
- `focused_monitor_` is the command target monitor.
- `Workspace::focused_window` is remembered focus for fallback on that workspace; it is not itself proof of current X focus.

High-value distinctions:

- `hidden` means the WM has physically moved the window off-screen.
- `iconic` means the window is logically minimized.
- `visible scope` means "should belong to the current monitor/workspace view" based on workspace, sticky, iconify, and showing-desktop policy.
- `actually visible` means "in visible scope and not physically hidden."

That distinction matters because LWM uses off-screen hiding rather than unmap/remap for normal workspace visibility.

## 3. Visibility Model

LWM hides managed windows by moving them off-screen, not by unmapping them.

Why this exists:

- avoids redraw issues seen with some GPU-heavy applications after unmap/remap
- keeps workspace switching immediate
- avoids turning routine workspace visibility changes into ICCCM map-state churn

Consequences:

- workspace changes usually mutate `hidden`, not `WM_STATE`
- received `UnmapNotify` is treated as client withdrawal, not as part of normal visibility control
- handlers must distinguish "in visible scope" from "already shown on screen"

Primary visibility funnels:

- `sync_visibility_for_monitor(...)`: applies the visible-scope decision for one monitor, including floating geometry restore for newly shown floating windows
- `rearrange_monitor(...)`: computes tiling geometry for the monitor's visible tiled set and keeps fullscreen windows out of normal layout

Do not reintroduce WM-driven unmap/map for normal workspace visibility without a deliberate design change.

## 4. Focus and Activation

`focus_any_window(...)` is the only normal entry point for assigning focus to a managed window.

What it is allowed to do before final focus assignment:

- reject focus during showing-desktop mode
- reject windows that are not focus-eligible
- deiconify the target first
- switch workspaces if the target belongs to another visible workspace on its monitor
- promote floating windows in MRU order
- abort stale finalization if a nested transition redirected focus elsewhere

Focus sources converge on the same policy:

- pointer enter and motion
- button press before drag/move operations
- `_NET_ACTIVE_WINDOW`
- explicit fallback after workspace, monitor, iconify, unmanage, or fullscreen transitions

Fallback order (`focus_or_fallback(...)`):

1. remembered workspace focus if still eligible
2. another eligible tiled window on the current workspace
3. eligible sticky tiled window from another workspace on the same monitor
4. eligible floating window in MRU order
5. clear focus

Important nuance:

- `active_window_` and X input focus must stay aligned
- focus changes may need to restack both the old and new monitor, not just the newly focused one
- hidden, iconified, suppressed, dock, and desktop windows are not focus-eligible

## 5. Fullscreen, Suppression, and Stacking

LWM treats fullscreen as an exclusive visible mode within one monitor's visible scope.

Visible scope means:

- the monitor's current workspace
- plus sticky windows on that same monitor
- excluding iconified windows
- disabled entirely when showing-desktop mode is active

Fullscreen ownership rules:

- at most one managed tiled or floating fullscreen owner is effective on a monitor's visible scope
- when multiple visible fullscreen windows conflict, one owner is elected and the other visible fullscreen windows lose fullscreen
- hidden or off-workspace fullscreen windows may keep their fullscreen state until they re-enter visible scope

Suppression rules while an owner exists:

- non-owner managed tiled/floating siblings in that visible scope are suppressed from focus, fallback, normal layout, and managed restacking
- suppressed managed windows are physically hidden through the normal visibility path
- overlay-layer windows are exempt
- managed transients whose `transient_for` points to the owner are exempt

Stack authority rules:

- `restack_monitor_layers(...)` is the normal managed stacking authority
- `apply_fullscreen_if_needed(...)` is geometry-only; it should not make independent stacking decisions
- tiled/floating clients are layered by overlay, fullscreen, above/normal/below, and active-window preference
- transient restacking happens relative to visible, unsuppressed parents

Important limit:

- `_NET_WM_FULLSCREEN_MONITORS` is currently geometry-only; it does not create a multi-monitor fullscreen ownership model

## 6. Workspace and Monitor Transitions

The goal is not to let every caller mutate monitor/workspace state ad hoc. State changes should pass through a small number of funnels.

Primary transition funnels:

- `perform_workspace_switch(...)`
- `focus_any_window(...)`
- `reconcile_fullscreen_for_monitor(...)`
- `sync_visibility_for_monitor(...)`
- `update_floating_monitor_for_geometry(...)`
- `restack_monitor_layers(...)`

`perform_workspace_switch(...)` contract:

1. update monitor current and previous workspace
2. update `_NET_CURRENT_DESKTOP`
3. reconcile fullscreen ownership for the new visible scope
4. sync visibility for the monitor
5. rearrange tiled layout for the monitor
6. restore focus in the caller

Window movement rules:

- moving a window to another workspace must update membership, visibility, and focus fallback
- moving a window to another monitor must update both source and destination monitor state
- floating monitor changes triggered by geometry or drag must run the same ownership and visibility reconciliation as explicit monitor-move commands

## 7. Window Lifecycle and Property Changes

Nominal manage path:

1. receive `MapRequest`
2. ignore override-redirect windows
3. classify the window type
4. apply rules
5. create `Client`
6. read initial state from properties and hints
7. compute initial geometry / placement
8. map the window
9. hide it off-screen if it is not currently supposed to be visible
10. apply post-map state such as fullscreen, maximize, above/below, sticky, and EWMH bookkeeping

Unmanage path:

- write `WM_STATE=Withdrawn`
- remove the client from all authoritative containers
- reconcile focus and visibility
- update EWMH lists

Property changes that matter at runtime:

- `_NET_WM_WINDOW_TYPE`
- `WM_TRANSIENT_FOR`
- `WM_HINTS`
- `WM_NORMAL_HINTS`
- `_NET_WM_USER_TIME_WINDOW`
- strut-related properties for dock windows

Those updates are easy to regress because they can change classification, focus eligibility, stacking exceptions, geometry policy, or workarea computation after manage-time.

## 8. Hotplug Contract

RANDR changes are handled as a structural rebuild, not as a small patch to old monitor indices.

Required behavior:

- clear fullscreen state before monitor-graph rebuild so stale geometry does not survive topology changes
- remap windows by monitor name, not old index
- fall back to monitor `0` when a previous monitor name disappears
- recompute struts and workareas
- restore focus target and rearrange all monitors

Using monitor names rather than indices is what prevents unplug/replug index churn from turning into workspace placement bugs.

## 9. Common Regression Traps

- Treating sticky as global across all monitors.
- Updating `_NET_CLIENT_LIST` on workspace switch instead of on manage/unmanage.
- Applying `ConfigureRequest` geometry directly to tiled windows.
- Reintroducing WM-driven unmap/map for normal workspace visibility.
- Letting a stack mutation bypass `restack_monitor_layers(...)`.
- Letting focus be finalized after a nested workspace/fullscreen transition has already redirected it.
- Forgetting that visible-scope decisions and physical visibility are different things.
- Adding new state transitions without routing them through the existing visibility, fullscreen, and focus funnels.
