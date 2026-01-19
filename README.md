# LWM - Lightweight Window Manager

A minimal tiling window manager for X11 written in modern C++23.

## Features

- Master-stack tiling layout
- Per-monitor workspaces with independent desktops
- Floating window support with drag-and-drop reordering
- Comprehensive EWMH/ICCCM compliance
- TOML configuration file
- Internal status bar (optional) or external bar support (Polybar)
- Keybinding and mouse binding support
- Focus-follows-mouse with focus stealing prevention
- Multi-monitor support with hotplug handling
- Window states: fullscreen, maximize, sticky, above/below, shaded, modal
- Smart window classification (dock, desktop, dialog, popup types)

## Dependencies

- CMake 3.20+
- C++23 compiler (GCC 13+ or Clang 17+)
- XCB libraries: `xcb`, `xcb-keysyms`, `xcb-randr`, `xcb-ewmh`, `xcb-icccm`, `xcb-sync`, `x11`
- toml++ (fetched automatically via CMake)

### Arch Linux

```bash
sudo pacman -S cmake gcc libxcb xcb-util-keysyms xcb-util-ewmh xcb-util-wm libx11
```

### Ubuntu/Debian

```bash
sudo apt install cmake g++ libxcb1-dev libxcb-keysyms1-dev libxcb-ewmh-dev libxcb-icccm-dev libxcb-randr0-dev libxcb-sync0-dev libx11-dev
```

## Building

```bash
mkdir build && cd build
cmake ..
make -j$(nproc)
```

The executable will be at `build/src/app/lwm`.

### Development Builds

```bash
cmake -DCMAKE_BUILD_TYPE=Debug ..
make -j$(nproc)
```

### Tests

```bash
cmake -DBUILD_TESTS=ON ..
make -j$(nproc)
./tests/lwm_tests
```

### Installation

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
- `status_bar_height`: Height of internal bar (default: 30)
- `status_bar_color`: RGB hex for internal bar (default: 0x808080)
- `enable_internal_bar`: Use internal bar vs. external Polybar (default: false)

**Focus:**
- `warp_cursor_on_monitor_change`: Move cursor to monitor center on focus (default: false)

**Programs:**
- `terminal`: Terminal emulator command (default: `/usr/local/bin/st`)
- `browser`: Browser command (default: `/usr/bin/firefox`)
- `launcher`: App launcher command (default: `dmenu_run`)

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

### Default Keybindings

Common default keybindings:

| Action | Key |
|--------|------|
| Launch terminal | Super+Return |
| Launch app launcher | Super+d |
| Close window | Super+q |
| Switch workspace 1-10 | Super+1..0 (QWERTY) or Super+&..= (AZERTY) |
| Move window to workspace 1-10 | Super+Shift+1..0 or Super+Shift+&..= |
| Focus monitor left/right | Super+Left/Right |
| Move window to adjacent monitor | Super+Shift+Left/Right |

See `config.toml.example` for the complete default configuration including AZERTY support.

### Modifier Keys

Modifiers can be combined with `+`:
- `super`, `shift`, `ctrl`/`control`, `alt`
- Examples: `super+shift`, `super+alt+ctrl`

NumLock and CapsLock are handled automatically - no need to include them in bindings.

## Mouse Bindings

Mouse bindings support these actions:

| Action | Description |
|--------|-------------|
| `drag_window` | Drag tiled windows to reorder; drag floating windows to move |
| `resize_floating` | Resize floating windows |

Tiled window drag-reorder: windows follow cursor during drag, then drop into nearest tile slot on release. Works across monitors.

## Window Classification

LWM automatically classifies windows based on `_NET_WM_WINDOW_TYPE`:

- **Desktop**: Below all windows, never focused, always visible (e.g., wallpaper)
- **Dock**: Panel/bar windows, reserve screen edges via struts, always visible
- **Dialog/Utility/Splash**: Floating, centered, auto get `skip_taskbar`/`skip_pager`
- **Tooltip/Popup/Notification**: Not managed, mapped directly, don't participate in tiling
- **Normal**: Tiled by default (or floating if transient)

Transients (child windows) automatically become floating and inherit parent's workspace.

## Window States

LWM supports EWMH window states via external tools or keybinds:

- **Fullscreen**: Covers monitor geometry, can span multiple monitors
- **Maximize**: Fills workarea (horizontal, vertical, or both) - floating windows only
- **Sticky**: Visible on all workspaces of owning monitor (does not switch workspace when focused)
- **Above/Below**: Stacking order relative to other windows
- **Modal**: Modal dialog behavior
- **Shaded**: Iconify (minimize) - implemented as window unmap
- **Iconified**: Minimized to taskbar (set `_NET_WM_STATE_HIDDEN`)

Use `xdotool` or external tools to toggle these states:

```bash
# Toggle fullscreen
xdotool key --window $WINDOW_ID super+f

# Set maximize (via EWMH)
wmctrl -r :ACTIVE: -b add,maximized_vert,maximized_horz
```

## Multi-Monitor Behavior

- Each monitor has **independent workspaces** (not global workspaces)
- Desktop switching affects only the focused monitor
- Monitor detection via RANDR (fallback to single monitor if unavailable)
- Hotplug support: windows moved to first monitor on reconfiguration
- EWMH desktop index = `monitor_index * workspaces_per_monitor + workspace_index`

## Focus Behavior

- **Focus-follows-mouse**: Always enabled
- **Click-to-focus**: Clicking any managed window focuses it
- **Focus stealing prevention**: Compares `_NET_WM_USER_TIME` timestamps; older requests set urgency instead
- **Empty space**: Moving to empty space on same monitor preserves focus; crossing to different monitor clears focus

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

## Internal Status Bar

When `enable_internal_bar = true`, LWM draws a minimal status bar at the top of each monitor:

- Workspace indicators (e.g., `[1] 2 3 ·` showing occupied/empty)
- Monitor name
- Focused window title

Disable this and use Polybar for more advanced features.

## Project Documentation

- **CLAUDE.md**: Development guide, architecture, code conventions
- **BEHAVIOR.md**: High-level behavior specification
- **COMPLIANCE.md**: ICCCM/EWMH protocol compliance requirements
- **SPEC_CLARIFICATIONS.md**: Design decisions on ambiguous spec points
- **FEATURE_IDEAS.md**: Feature backlog and design principles

## License

MIT
