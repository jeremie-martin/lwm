# Working on LWM

Production-code simplification should improve ownership and invariants while
reducing code. Report the net production-line change for that work; deleting
tests, comments, or documentation does not count toward that goal. Prefer deletion,
consolidation, derivation, and established libraries over new layers.

Window-management behavior has been refined through many iterations. Preserve
geometry, focus, placement, and fullscreen semantics unless a behavior change is
requested. Configuration and parsing conventions can be challenged when they
cause disproportionate complexity; describe any user-facing change.

[TESTING.md](TESTING.md) documents isolated displays, synchronization, restart
fixtures, and performance probes. Never run WM tests against a live desktop.
