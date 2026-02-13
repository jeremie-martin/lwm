# Focus/Input Architecture Refactor Rationale

## Why
This refactor targets structural issues, not just one bug symptom.

- Focus handling is coupled with geometry/state work (for example fullscreen/maximize reapply), so focus events can trigger unintended reconfiguration.
- `focused_monitor_` currently carries multiple meanings (pointer location, command target, focus context), which creates ambiguity and edge-case inconsistencies.
- Equivalent behavior is implemented across multiple event paths, so correctness depends on event ordering/path rather than one policy.

## Benefits
- Prevents regressions where focus changes cause visible stalls or side effects.
- Makes behavior across monitors/workspaces deterministic and easier to reason about.
- Reduces hidden coupling and side effects, improving maintainability.
- Improves testability via explicit state transitions and centralized policy logic.
- Enables safer future feature work (new focus policies, monitor behavior, input modes).

## How
1. Split state into explicit concepts:
- `pointer_monitor`
- `command_monitor`
- `focused_window`

2. Introduce a pure `FocusEngine`:
- Input: current WM state + normalized domain event.
- Output: intents (`SetFocus(window)`, `ClearFocus`, `SetCommandMonitor`, `NoOp`).

3. Introduce an effect executor:
- Applies intents in one place (X focus calls, `_NET_ACTIVE_WINDOW`, border updates).
- Keeps fullscreen/maximize geometry handling out of focus transitions.

4. Normalize event pipeline:
- Convert raw X events (`EnterNotify`, `MotionNotify`, button events, client messages) into internal domain events first.
- Route all focus decisions through the same policy path.

5. Add scenario/state-machine tests:
- Cross-monitor empty-space transitions.
- Strict no-focus behavior.
- Fullscreen windows while unfocused.
- Workspace and monitor transitions with deterministic expected intents.
