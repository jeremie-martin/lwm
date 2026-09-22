# Roadmap

This file records verified gaps in the current implementation. It is not a
release promise; completed work belongs in Git history.

## Correctness and consistency

- Decide whether config reload should restore state left by a removed or
  no-longer-matching rule. Current reload applies the first rule that now
  matches but does not undo earlier rule effects when nothing matches.
- Make `make uninstall` remove `lwmctl` and `lwm-notify` as well as `lwm`.

## Protocol and tooling

- Expose the existing raw scratchpad commands through `lwmctl`.
- Model `_NET_WM_STRUT_PARTIAL` coordinate ranges instead of using edge extents
  only.
- Decide and document a policy for tiled-window `_NET_WM_MOVERESIZE` requests;
  tiled geometry is currently layout-owned and these requests are ignored.
- Upgrade `_NET_WM_SYNC_REQUEST` from notification-only behavior if waiting on
  client counters can be made safe without blocking the event loop.

## Test coverage

- Add real multi-output integration coverage for cross-monitor moves, floating
  geometry, and RANDR rebind. Pure hotplug planning is covered today, while the
  integration harness exposes a single Xvfb screen.
