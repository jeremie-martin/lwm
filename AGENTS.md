# Working on LWM

Every decision has exactly one home. `State` decides; the shell observes X as plain
values and applies effects; completion derives what to publish. When a fix or feature
needs the same decision in two places, or a flag that keeps derived data in sync, treat
that as the defect: move the decision to its owner or derive the value instead.

Simplify by removing mechanisms, not by compressing code. Line count measures how many
places knowledge lives: deleting a duplicated decision, a parallel cache, a translation
layer or a hand-rolled substitute for a kernel or library feature shrinks the code
because something real is gone. Adding explicit boundary types can be worth lines when
it gives a decision one home. Report the net production-line change for simplification
work; deleting tests, comments or documentation does not count toward it. Prefer
deletion, consolidation, derivation and established libraries over new layers.

Window-management behavior has been refined through many iterations. Preserve geometry,
focus, placement and fullscreen semantics unless a behavior change is requested.
Configuration and parsing conventions can be challenged when they cause
disproportionate complexity; describe any user-facing change in X11.md or IPC.md.

[ARCHITECTURE.md](ARCHITECTURE.md) describes the current design only; keep it that way.
[TESTING.md](TESTING.md) documents isolated displays, synchronization, restart fixtures
and performance probes. Never run WM tests against a live desktop.
