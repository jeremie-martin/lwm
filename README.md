# LWM

LWM is a minimal tiling window manager for X11 written in C++23.

## What You Get

- Master/stack tiling with floating windows.
- Per-monitor workspaces (each monitor keeps its own current workspace).
- Focus-follows-mouse with explicit focus-stealing checks.
- EWMH/ICCCM support for normal desktop tooling (`wmctrl`, panels, pagers).
- RANDR hotplug handling.
- TOML configuration for keybinds, mousebinds, programs, and workspace names.

## Build

### Dependencies

- `cmake` 3.20+
- C++23 compiler
- X11/XCB libs: `xcb`, `xcb-keysyms`, `xcb-randr`, `xcb-ewmh`, `xcb-icccm`, `xcb-sync`, `x11`

Arch Linux:

```bash
sudo pacman -S cmake gcc libxcb xcb-util-keysyms xcb-util-ewmh xcb-util-wm libx11
```

Ubuntu/Debian:

```bash
sudo apt install cmake g++ libxcb1-dev libxcb-keysyms1-dev libxcb-ewmh-dev libxcb-icccm-dev libxcb-randr0-dev libxcb-sync0-dev libx11-dev
```

### Compile

```bash
make          # release
make debug    # debug
make test     # build + run tests
```

Direct CMake is also supported:

```bash
mkdir build && cd build
cmake ..
make -j"$(nproc)"
```

Binary path: `build/src/app/lwm`

### Install

```bash
make install
```

Default install path: `/usr/local/bin/lwm`

## Configure

```bash
export XDG_CONFIG_HOME="${XDG_CONFIG_HOME:-$HOME/.config}"
mkdir -p "$XDG_CONFIG_HOME/lwm"
cp config.toml.example "$XDG_CONFIG_HOME/lwm/config.toml"
```

LWM checks `$XDG_CONFIG_HOME/lwm/config.toml` when `XDG_CONFIG_HOME` is set.
Run with explicit config path if needed:

```bash
lwm /path/to/config.toml
```

The config file is the source of truth for bindings and behavior toggles. Keep `config.toml.example` nearby while editing.

### Keybinding Actions (Quick List)

Available actions include:

- `spawn`, `kill`
- `switch_workspace`, `toggle_workspace`, `move_to_workspace`
- `focus_monitor_left`, `focus_monitor_right`
- `move_to_monitor_left`, `move_to_monitor_right`
- `toggle_fullscreen`
- `focus_next`, `focus_prev`

See `config.toml.example` for exact syntax and default bindings.

## Run and Test in Xephyr

```bash
./scripts/preview.sh
```

Launch test apps into the nested server:

```bash
DISPLAY=:100 xterm
```

## Behavior Notes That Matter

- Workspace switching is per monitor, not global.
- Hidden windows are moved off-screen, not unmapped. This avoids redraw issues in some GPU apps.
- Sticky windows are sticky within their owning monitor (not across all monitors).
- Popups/tooltips/notifications are mapped but not fully managed.

## Documentation

- [`BEHAVIOR.md`](BEHAVIOR.md): user-visible behavior contract
- [`IMPLEMENTATION.md`](IMPLEMENTATION.md): architecture, event flow, invariants, pitfalls
- [`COMPLIANCE.md`](COMPLIANCE.md): ICCCM/EWMH support matrix, limits, and test checklist
- [`CLAUDE.md`](CLAUDE.md): contributor quick reference for local development
- [`FEATURE_IDEAS.md`](FEATURE_IDEAS.md): prioritized backlog

## License

MIT
