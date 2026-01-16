# LWM Development Guide

## Quick Commands

**Build:**
```bash
mkdir -p build && cd build && cmake .. && make -j$(nproc)
```

**Test with Xephyr:**
```bash
./scripts/preview.sh
```

**Format code:**
```bash
find src -name '*.cpp' -o -name '*.hpp' | xargs clang-format -i
```

## Project Structure

```
lwm/
├── CMakeLists.txt           # Root CMake
├── config.toml.example      # Example config
├── scripts/
│   └── preview.sh           # Xephyr testing
└── src/
    ├── lwm/                  # Library (liblwm)
    │   ├── core/
    │   │   ├── connection.hpp/cpp   # XCB connection RAII
    │   │   └── types.hpp            # Window, Tag, KeyBinding
    │   ├── config/
    │   │   └── config.hpp/cpp       # TOML config loading
    │   ├── layout/
    │   │   └── layout.hpp/cpp       # Tiling algorithm
    │   ├── keybind/
    │   │   └── keybind.hpp/cpp      # Key binding management
    │   ├── bar/
    │   │   └── bar.hpp/cpp          # Status bar
    │   └── wm.hpp/cpp               # WindowManager orchestrator
    └── app/
        └── main.cpp                 # Entry point
```

## Architecture

- **Connection**: RAII wrapper for XCB connection, screen, and key symbols
- **Config**: TOML parsing with defaults, loads appearance/programs/keybinds
- **KeybindManager**: Parses key names to keysyms, handles modifier masks
- **Layout**: Master-stack tiling algorithm
- **StatusBar**: Minimal XCB text rendering for tags and window name
- **WindowManager**: Orchestrates all components, runs event loop

## Adding Features

**New keybind action:**
1. Add action type to `keybind.cpp` resolution
2. Handle in `wm.cpp` `handle_key_press`
3. Document in config.toml.example

**New layout:**
1. Add layout mode to `Layout` class
2. Switch layout based on config or keybind

## Code Style

- 4-space indent, 120 column limit
- Allman braces
- `snake_case` for functions/variables
- `PascalCase` for types/classes
- Use `std::ranges` algorithms
- RAII for all resources
