# LWM

LWM is a minimal tiling window manager for X11 written in C++23.

It is intentionally small, but it is not policy-free. The codebase has a specific runtime model:

- per-monitor workspaces rather than one global workspace set
- focus-follows-mouse with explicit focus-stealing checks
- off-screen hiding for workspace visibility instead of WM-driven unmap/remap
- managed fullscreen with a single effective owner per monitor-visible scope
- enough ICCCM/EWMH support for normal desktop tooling

## What You Get

- master/stack tiling with floating windows
- monocle layout and live master-ratio adjustment
- per-monitor workspaces
- sticky windows scoped to a single monitor
- managed overlay windows above fullscreen
- focus-follows-mouse
- Unix-socket IPC via `lwmctl`
- RANDR hotplug handling
- named and generic scratchpads
- TOML configuration for commands, autostart, bindings, rules, scratchpads, layout, focus behavior, and workspace names

## Build

### Dependencies

- `cmake` 3.20+
- C++23 compiler
- X11/XCB libs: `xcb`, `xcb-keysyms`, `xcb-randr`, `xcb-ewmh`, `xcb-icccm`, `xcb-sync`, `x11`
- test-only XCB lib: `xcb-xtest`

Arch Linux:

```bash
sudo pacman -S cmake gcc libxcb xcb-util-keysyms xcb-util-ewmh xcb-util-wm libx11
```

Ubuntu/Debian:

```bash
sudo apt install cmake g++ libxcb1-dev libxcb-keysyms1-dev libxcb-ewmh-dev libxcb-icccm-dev libxcb-randr0-dev libxcb-sync0-dev libxcb-xtest0-dev libx11-dev
```

### Compile

```bash
make          # release
make debug    # debug build
make test     # build + run tests
```

Direct CMake is also supported:

```bash
mkdir build && cd build
cmake ..
make -j"$(nproc)"
```

Main binary paths: `build/src/app/lwm` and `build/src/app/lwmctl`

## Install

```bash
make install
```

Default install paths: `/usr/local/bin/lwm`, `/usr/local/bin/lwmctl`, and `/usr/local/bin/lwm-notify`.

For X startup, prefer an explicit path in `.xinitrc` or your display-manager session entry instead of relying on `PATH`. That avoids accidentally starting a stale shadow copy from `~/.local/bin` or another earlier install location.

## Configure

```bash
export XDG_CONFIG_HOME="${XDG_CONFIG_HOME:-$HOME/.config}"
mkdir -p "$XDG_CONFIG_HOME/lwm"
cp config.toml.example "$XDG_CONFIG_HOME/lwm/config.toml"
```

LWM reads `$XDG_CONFIG_HOME/lwm/config.toml` when `XDG_CONFIG_HOME` is set. You can also pass an explicit config path:

```bash
lwm /path/to/config.toml
```

`config.toml.example` is the reference for binding syntax, workspace names, commands, autostart, layout settings, focus settings, rules, scratchpads, and action syntax.

## Runtime Control

LWM exposes a local control socket and ships `lwmctl` for explicit runtime commands:

```bash
lwmctl ping
lwmctl version
lwmctl reload-config
lwmctl restart
lwmctl layout set monocle
lwmctl layout set master-stack
lwmctl ratio set 0.60
lwmctl ratio adjust -0.05
lwmctl ratio reset
lwmctl workspace switch 2
lwmctl workspace next
lwmctl workspace list
lwmctl focus next
lwmctl focus window=0x3600007
lwmctl window list
lwmctl subscribe focus_change,workspace_switch
lwmctl notify-attention window=0x3600007
```

`workspace list` and `window list` return JSON. `subscribe` streams JSON lines; an empty filter subscribes to all event types, while a comma-separated filter may include `window_map`, `window_unmap`, `focus_change`, `workspace_switch`, `layout_change`, `config_reload`, and `key_action`.

`lwmctl` discovers the socket via `--socket`, `LWM_SOCKET`, the root-window `_LWM_IPC_SOCKET` property, then the default runtime path.

Config reload is explicit by design. LWM does not watch the config file automatically. Reload can update appearance, key and mouse bindings, rules, commands, autostart entries, layout defaults, and focus settings; changing `[workspaces].count` still requires a restart.

## Run in Xephyr

```bash
./scripts/preview.sh
```

The preview script runs LWM in Xephyr on `:100` and starts the sample polybar config when `polybar` is installed.

Launch test apps into the nested server:

```bash
DISPLAY=:100 xterm
```

## Mental Model

- Managed windows are usually tiled, floating, dock, or desktop. Popup and ephemeral types are mapped directly and are not part of normal workspace/layout management.
- Workspace visibility is implemented by moving managed windows off-screen, not by unmapping them.
- Sticky means "visible on every workspace of this monitor", not "visible on all monitors".
- Fullscreen is exclusive within a monitor's visible scope. One visible managed fullscreen owner wins; other visible managed siblings on that monitor are suppressed.
- Overlay windows and owner-owned transients are the normal exceptions above fullscreen.
- Scratchpads are managed clients. Hidden scratchpads are iconified and moved off-screen through the same visibility machinery as ordinary hidden windows.

## Documentation

- [`ARCHITECTURE.md`](ARCHITECTURE.md): runtime model, invariants, state ownership, and transition funnels
- [`COMPLIANCE.md`](COMPLIANCE.md): ICCCM/EWMH surface, protocol behavior, and known limits
- [`CONTRIBUTING.md`](CONTRIBUTING.md): build/test workflow, code map, and change checklist
- [`SHADERS.md`](SHADERS.md): using picom + GLSL shaders on top of LWM
- [`ROADMAP.md`](ROADMAP.md): open work and open questions

## License

MIT
