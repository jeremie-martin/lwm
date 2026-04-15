Leveraging the split tree infrastructure:
- More layout strategies — dwindle/spiral (like bspwm), centered master, columns, monocle/tabbed. Each is just a new build_* function in strategy.cpp.
- Window swapping — keyboard shortcut to swap two tiled windows in the tree (Super+Shift+J/K). The reorder drag already moves windows; keyboard swap is the missing complement.
- Per-workspace layout switching — lwmctl layout set monocle already parses; just needs more strategies wired up.

User-facing polish:
- Scratchpad — a hidden workspace you toggle a floating terminal in/out of with a hotkey. Very popular in i3/sway.
- Urgency hints — flash the border or mark workspaces when a window sets _NET_WM_STATE_DEMANDS_ATTENTION.
- Better multi-monitor — warp cursor on focus-monitor-left/right (config flag exists but may not be wired), drag floating windows across monitors seamlessly.

Deeper X11 work:
- XSync resize — upgrade from fire-and-forget to blocking sync with timeout during interactive resize, eliminating the last bit of flicker for slow clients.
- Compositing awareness — _NET_WM_WINDOW_OPACITY, working nicely with picom/compton.
- EWMH strut partial — proper handling of panels that only reserve space on part of an edge.

Infrastructure:
- More window rules — auto-move to workspace, auto-float by size, opacity rules.
