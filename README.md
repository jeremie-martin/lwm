# LWM - Lightweight Window Manager

A minimal tiling window manager for X11 written in modern C++23.

## Features

- Master-stack tiling layout
- Tag-based workspace system (configurable count)
- TOML configuration file
- Status bar with tag and window indicators
- Keybinding support with modifier handling (NumLock/CapsLock aware)

## Dependencies

- CMake 3.20+
- C++23 compiler (GCC 13+ or Clang 17+)
- XCB libraries: `xcb`, `xcb-keysyms`
- toml++ (fetched automatically via CMake)

### Arch Linux

```bash
sudo pacman -S cmake gcc libxcb xcb-util-keysyms
```

### Ubuntu/Debian

```bash
sudo apt install cmake g++ libxcb1-dev libxcb-keysyms1-dev
```

## Building

```bash
mkdir build && cd build
cmake ..
make -j$(nproc)
```

The executable will be at `build/src/app/lwm`.

## Configuration

Copy `config.toml.example` to `~/.config/lwm/config.toml` and customize:

```bash
mkdir -p ~/.config/lwm
cp config.toml.example ~/.config/lwm/config.toml
```

Or specify a config path as argument:

```bash
./lwm /path/to/config.toml
```

## Default Keybindings (AZERTY)

| Key | Action |
|-----|--------|
| Super + Return | Launch terminal |
| Super + f | Launch browser |
| Super + d | Launch dmenu |
| Super + q | Close focused window |
| Super + [1-0] | Switch to workspace (default 1-10; configurable) |
| Super + Shift + m | Move window to next tag |

## Development

Test with Xephyr:

```bash
./scripts/preview.sh
```

This builds the project and runs it in a nested X server.

## License

MIT
