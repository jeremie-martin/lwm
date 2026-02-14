# LWM Feature Backlog

Only ideas with clear user value and bounded complexity stay in this file.

## Prioritization Rules

- Keep core behavior predictable.
- Prefer features that improve daily workflow over novelty.
- Reject ideas that introduce hidden state or protocol regressions.

## Near-Term (High Value / Low Risk)

- Per-workspace layout toggle (`master-stack` <-> `monocle`).
- Master area ratio grow/shrink.
- Keyboard window reordering (swap/nudge without mouse).
- Config flag for "focus new windows" behavior.
- Runtime workspace rename with `_NET_DESKTOP_NAMES` update.
- Better urgency presentation policy (focus vs visual hint only).

## Mid-Term (Moderate Complexity)

- Binding modes (temporary keymap layers for resize/move).
- Scratchpad workspace with toggle behavior.
- `lwmctl` IPC for querying and mutating WM state.
- Config reload without restart.
- Per-monitor layout parameters (ratio, gaps).

## Needs Design First

- True tree-based layouts (bspwm-style split graph).
- Global marks/bookmarks for jump/swap.
- Policy for tiled-window client moveresize requests.
- Optional animation system that does not block event handling.

## Deferred / Not Recommended Right Now

- Plugin system.
- Full session-management integration.
- Large UI surface (integrated panels/widgets).

## Open Decisions

1. Should workspace model remain strict single-assignment, or support tag-like multi-assignment?
2. Should focus-follows-mouse be globally toggleable or per-monitor?
3. Should IPC be Unix socket only, or also X11 client-message based?
