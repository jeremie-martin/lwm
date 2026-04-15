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
- per-monitor workspaces
- sticky windows scoped to a single monitor
- managed overlay windows above fullscreen
- focus-follows-mouse
- Unix-socket IPC via `lwmctl`
- RANDR hotplug handling
- TOML configuration for commands, bindings, rules, and workspace names

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
make debug    # debug build
make test     # build + run tests
```

Direct CMake is also supported:

```bash
mkdir build && cd build
cmake ..
make -j"$(nproc)"
```

Main binary path: `build/src/app/lwm`

## Install

```bash
make install
```

Default install path: `/usr/local/bin/lwm`

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

`config.toml.example` is the reference for binding syntax, workspace names, rules, and action syntax.

## Runtime Control

LWM exposes a local control socket and ships `lwmctl` for explicit runtime commands:

```bash
lwmctl ping
lwmctl version
lwmctl reload-config
```

`lwmctl` discovers the socket via `--socket`, `LWM_SOCKET`, the root-window `_LWM_IPC_SOCKET` property, then the default runtime path.

Config reload is explicit by design. LWM does not watch the config file automatically.

## Run in Xephyr

```bash
./scripts/preview.sh
```

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

## Documentation

- [`ARCHITECTURE.md`](ARCHITECTURE.md): runtime model, invariants, state ownership, and transition funnels
- [`COMPLIANCE.md`](COMPLIANCE.md): ICCCM/EWMH surface, protocol behavior, and known limits
- [`CONTRIBUTING.md`](CONTRIBUTING.md): build/test workflow, code map, and change checklist
- [`FEATURE_IDEAS.md`](FEATURE_IDEAS.md): backlog only

## License

MIT
