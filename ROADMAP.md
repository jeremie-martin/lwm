# Roadmap

This file records verified gaps in the current implementation. It is not a
release promise; completed work belongs in Git history.

## Correctness and consistency

- Decide whether config reload should restore state left by a removed or
  no-longer-matching rule. Current reload applies the first rule that now
  matches but does not undo earlier rule effects when nothing matches.

## Protocol and tooling

- Decide and document a policy for tiled-window `_NET_WM_MOVERESIZE` requests;
  tiled geometry is currently layout-owned and these requests are ignored.
- Upgrade `_NET_WM_SYNC_REQUEST` from notification-only behavior if waiting on
  client counters can be made safe without blocking the event loop.

## Test coverage

- Extend the owned Xorg dummy-server tests to cover more cross-monitor input,
  scratchpad, and dock interactions. Output add/reorder/remove/return, workspace
  preservation, floating rebind, and fullscreen migration are covered. Physical
  monitor/driver hotplug behavior still needs hardware validation.
