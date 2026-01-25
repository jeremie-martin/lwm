# LWM - Lightweight Window Manager

A minimal tiling window manager for X11 written in modern C++23.

## Features

- Master-stack tiling layout
- Per-monitor workspaces with independent desktops
- Floating window support with drag-and-drop reordering
- Comprehensive EWMH/ICCCM compliance
- TOML configuration file
- External bar support (Polybar via EWMH struts)
- Keybinding and mouse binding support
- Focus-follows-mouse with focus stealing prevention
- Multi-monitor support with hotplug handling
- Window states: fullscreen, maximize, sticky, above/below, shaded, modal, iconified
- Smart window classification (dock, desktop, dialog, popup types)
- Window rules for automatic window configuration (floating, workspace, monitor, etc.)

## Dependencies

- CMake 3.20+
- C++23 compiler (GCC 13+ or Clang 17+)
- XCB libraries: `xcb`, `xcb-keysyms`, `xcb-randr`, `xcb-ewmh`, `xcb-icccm`, `xcb-sync`, `x11`
- toml++ (fetched automatically via CMake)
- spdlog (fetched automatically via CMake)

**Arch Linux:**
```bash
sudo pacman -S cmake gcc libxcb xcb-util-keysyms xcb-util-ewmh xcb-util-wm libx11
```

**Ubuntu/Debian:**
```bash
sudo apt install cmake g++ libxcb1-dev libxcb-keysyms1-dev libxcb-ewmh-dev libxcb-icccm-dev libxcb-randr0-dev libxcb-sync0-dev libx11-dev
```

## Building

**Using the Makefile wrapper (recommended):**
```bash
make            # Release build
make debug      # Debug build
make test       # Build and run all tests
```

**Using CMake directly:**
```bash
mkdir build && cd build
cmake ..
make -j$(nproc)
```

The executable will be at `build/src/app/lwm`.

**Development builds:**
```bash
cmake -DCMAKE_BUILD_TYPE=Debug ..
make -j$(nproc)
```

Or using the Makefile:
```bash
make debug
```

**Tests:**
```bash
cmake -DBUILD_TESTS=ON ..
make -j$(nproc)
./tests/lwm_tests
```

Or using the Makefile:
```bash
make test
```

**Installation:**
```bash
make install
```

Installs to `/usr/local/bin` by default.

## Configuration

Copy `config.toml.example` to `~/.config/lwm/config.toml`:
```bash
mkdir -p ~/.config/lwm
cp config.toml.example ~/.config/lwm/config.toml
```

Or specify a config path as argument:
```bash
./lwm /path/to/config.toml
```

### Configuration Options

**Appearance:**
- `padding`: Gap between windows and edges (default: 10)
- `border_width`: Border width in pixels (default: 2)
- `border_color`: RGB hex for focused window (default: 0xFF0000)

**Focus:**
- `warp_cursor_on_monitor_change`: Move cursor to monitor center on focus (default: false)

**Programs:**
- `terminal`: Terminal emulator command (default: `/usr/local/bin/st`)
- `browser`: Browser command (default: `/usr/bin/firefox`)
- `launcher`: App launcher command (default: `rofi -show drun`)

**Workspaces:**
- `count`: Number of workspaces per monitor (default: 10, min 1)
- `names`: Optional array of workspace names (defaults to "1".."10")

## Keybinding Actions

LWM supports the following keybinding actions:

| Action | Parameters | Description |
|--------|------------|-------------|
| `spawn` | `command` | Launch a program (use preset names like "terminal", "browser", "launcher" or full paths) |
| `kill` | None | Close focused window gracefully (force kills hung windows after 5s timeout) |
| `switch_workspace` | `workspace` | Switch to workspace index (0..count-1) on current monitor |
| `toggle_workspace` | None | Toggle between current and previous workspace |
| `move_to_workspace` | `workspace` | Move focused window to workspace index |
| `focus_monitor_left` | None | Focus monitor to the left |
| `focus_monitor_right` | None | Focus monitor to the right |
| `move_to_monitor_left` | None | Move window to monitor on left, follow it |
| `move_to_monitor_right` | None | Move window to monitor on right, follow it |
| `toggle_fullscreen` | None | Toggle fullscreen state on focused window |
| `focus_next` | None | Focus next eligible window |
| `focus_prev` | None | Focus previous eligible window |

### Default Keybindings

Common keybindings: Super+Return (terminal), Super+d (launcher), Super+q (close), Super+1..0 (switch workspace), Super+Shift+1..0 (move to workspace), Super+f (fullscreen), Super+j/k (focus next/prev).

See `config.toml.example` for complete default configuration including AZERTY support.

### Modifier Keys

Modifiers can be combined with `+`:
- `super`, `shift`, `ctrl`/`control`, `alt`
- Examples: `super+shift`, `super+alt+ctrl`

NumLock and CapsLock are handled automatically - no need to include them in bindings.

## Mouse Bindings

**Mouse binding actions:**

| Action | Description |
|--------|-------------|
| `drag_window` | Drag tiled windows to reorder; drag floating windows to move |
| `resize_floating` | Resize floating windows |

**Tiled window drag-reorder**: Windows follow cursor during drag, then drop into nearest tile slot on release. Works across monitors.

## Window Classification

LWM classifies windows by type: Desktop (below all), Dock (panels), Dialog/Utility/Splash (floating), Tooltip/Popup/Notification (unmanaged), Normal (tiled by default, floating if transient). See [BEHAVIOR.md](BEHAVIOR.md#13-window-classes-behavioral) for details.

## Window States

Supported states: fullscreen, maximize (floating only), sticky, above/below, modal, shaded, iconified. Toggle via keybinds or external tools (`xdotool`, `wmctrl`). See [BEHAVIOR.md](BEHAVIOR.md) for details.

## Multi-Monitor Behavior

Each monitor has independent workspaces. Desktop switching affects only the focused monitor. Hotplug support via RANDR. See [BEHAVIOR.md](BEHAVIOR.md#1-concepts-and-model) for details.

## Focus Behavior

Focus-follows-mouse always enabled with click-to-focus. Focus stealing prevention via `_NET_WM_USER_TIME`. Empty space behavior preserves focus on same monitor, clears when crossing to different monitor. See [BEHAVIOR.md](BEHAVIOR.md#2-focus-and-active-monitor-policy).

## Development

### Xephyr Testing

Run in nested X server:
```bash
./scripts/preview.sh
```

This builds the project, starts Xephyr, runs LWM, and optionally launches Polybar.

### Polybar Integration

The `scripts/launch-polybar.sh` script and `config/polybar.ini` provide Polybar configuration for LWM:
- Uses `xworkspaces` module for workspace switching
- Supports EWMH workspace actions via `_NET_CURRENT_DESKTOP`

Run applications in the nested X server:
```bash
DISPLAY=:100 <application>
```

## Project Documentation

For complete documentation navigation, see **[DOCS_INDEX.md](DOCS_INDEX.md)** (Documentation roadmap)

**User documentation:**
- **[BEHAVIOR.md](BEHAVIOR.md)** - User-facing behavior: focus, workspaces, monitors, window rules

**Contributor documentation:**
- **[CLAUDE.md](CLAUDE.md)** - Development guide and code conventions
- **[DOCS_INDEX.md](DOCS_INDEX.md)** - Documentation roadmap and quick reference
- **[IMPLEMENTATION.md](IMPLEMENTATION.md)** - Architecture, data structures, invariants
- **[STATE_MACHINE.md](STATE_MACHINE.md)** - Window states and state transitions
- **[EVENT_HANDLING.md](EVENT_HANDLING.md)** - Event-by-event handling specifications
- **[COMPLETE_STATE_MACHINE.md](COMPLETE_STATE_MACHINE.md)** - Legacy complete specification (superseded by split docs)
- **[COMPLIANCE.md](COMPLIANCE.md)** - ICCCM/EWMH protocol requirements
- **[SPEC_CLARIFICATIONS.md](SPEC_CLARIFICATIONS.md)** - Design decisions on ambiguous specs
- **[FEATURE_IDEAS.md](FEATURE_IDEAS.md)** - Feature backlog and design principles

## License

MIT
