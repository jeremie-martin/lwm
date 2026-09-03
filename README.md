# LWM

LWM is a small tiling window manager for X11, written in C++23. It provides
master-stack and monocle layouts, floating windows, per-monitor workspaces,
focus-follows-mouse, scratchpads, RANDR hotplug handling, and a local IPC
client named `lwmctl`.

LWM is not a compositor, panel, launcher, or desktop session. Run those as
separate programs, normally from `[autostart]` in the LWM configuration or from
your X session.

## Runtime model

- Every monitor has its own workspace set and one current workspace.
- Commands act on the focused monitor. Sticky windows remain on one monitor
  but are visible on all of that monitor's workspaces.
- Normal workspace changes move managed windows off-screen; LWM does not
  unmap and remap them.
- One tiled or floating fullscreen window can own a monitor's visible scope.
  Other tiled and floating windows there are hidden, except transients owned by the
  fullscreen window.
- Docks and desktop windows are managed outside normal focus and tiling.
  Tooltips, notifications, dropdown/pop-up menus, and other ephemeral windows
  are mapped but otherwise unmanaged.

See [ARCHITECTURE.md](ARCHITECTURE.md) for the complete maintainer model and
[X11.md](X11.md) for the ICCCM/EWMH contract.

## Build

Required tools and libraries:

- CMake 3.20 or newer
- Git and a C++23 compiler
- `pkg-config`
- X11/XCB modules `xcb`, `xcb-keysyms`, `xcb-randr`, `xcb-ewmh`, `xcb-icccm`,
  `xcb-sync`, and `x11`
- `xcb-xtest` and Xvfb for the full test suite

CMake downloads pinned toml++ and spdlog sources on the first configure; test
builds also download pinned Catch2 sources.

Arch Linux:

```sh
sudo pacman -S --needed cmake gcc git pkgconf libx11 libxcb xcb-util-keysyms xcb-util-wm xorg-server-xvfb
```

Debian/Ubuntu:

```sh
sudo apt install cmake g++ git pkg-config libx11-dev libxcb1-dev \
  libxcb-keysyms1-dev libxcb-randr0-dev libxcb-ewmh-dev \
  libxcb-icccm4-dev libxcb-sync-dev libxcb-xtest0-dev xvfb
```

Build the release binaries or build and run all tests:

```sh
make
make test
```

The binaries are written to `build/src/app/lwm` and
`build/src/app/lwmctl`. Direct CMake use is also supported:

```sh
cmake -S . -B build -DBUILD_TESTS=ON
cmake --build build -j
ctest --test-dir build --output-on-failure
```

## Install and start

```sh
sudo make install
```

With the default CMake prefix this installs `lwm`, `lwmctl`, and `lwm-notify`
under `/usr/local/bin`.

`sudo make uninstall` currently removes only `/usr/local/bin/lwm`; remove
`lwmctl` and `lwm-notify` separately until the open install-system gap is fixed.

Copy the reference configuration and start LWM with an explicit path:

```sh
config_dir="${XDG_CONFIG_HOME:-$HOME/.config}/lwm"
mkdir -p "$config_dir"
cp config.toml.example "$config_dir/config.toml"
/usr/local/bin/lwm --config "$config_dir/config.toml"
```

Use the same `lwm --config ...` command in `.xinitrc` or a display-manager
session entry. An explicit path must exist and parse successfully. Without an
explicit path, LWM reads `$XDG_CONFIG_HOME/lwm/config.toml` only when
`XDG_CONFIG_HOME` is set; if that implicit file is absent, it uses built-in
defaults.

`lwm --help` is the source of truth for startup and logging options.

## Configure

[config.toml.example](config.toml.example) is the commented starter and
configuration reference. It documents commands, autostart, appearance,
layouts, focus behavior, key and mouse bindings, window rules, workspace
names, and scratchpads. Workspace and monitor indices are zero-based.

Reload with `lwmctl reload-config`, a configured `reload_config` binding, or
`SIGHUP`. A successful reload updates appearance, bindings, workspace names,
focus and layout parameters, command and scratchpad definitions, and rules
that currently match managed tiled or floating windows.

Reload has these limits:

- `[autostart]` is not run again.
- `[workspaces].count` changes are rejected; restart LWM instead.
- `[layout].strategy` does not replace the strategy already stored on an
  existing workspace; use `lwmctl layout set` for the current workspace.
- Removing or changing a rule does not undo effects already applied by that
  rule when the window no longer matches.

## Runtime control

`lwmctl --help` is the command reference. Common examples:

```sh
lwmctl ping
lwmctl workspace switch 2
lwmctl layout set monocle
lwmctl ratio adjust -0.05
lwmctl window list
lwmctl subscribe focus_change,workspace_switch
lwmctl reload-config
lwmctl restart
```

`lwm-notify [notify-send arguments...]` shows a desktop notification when
`notify-send` is installed. When the caller has a numeric `WINDOWID`, it also
marks that exact managed source window urgent; LWM does not guess a source from
notification metadata.

List commands return JSON and subscriptions stream JSON Lines. See
[IPC.md](IPC.md) for discovery, the raw wire protocol, schemas, and delivery
semantics.

## Logs

LWM writes INFO-and-higher records to stderr. By default it also writes
WARN-and-higher records to a private rotating file at
`$XDG_RUNTIME_DIR/lwm/lwm-<pid>.log`, falling back to `/tmp/lwm-<pid>.log`.
The file is mode `0600`, rotates at 1 MiB, and keeps three backups. Use
`--log-level`, `--log-file`, `--no-log-file`, and `--log-color` to change the
startup policy.

## Nested preview

Install Xephyr, then run:

```sh
./scripts/preview.sh
```

The script builds a debug binary, starts a nested server on `:100`, and starts
the sample Polybar configuration when Polybar is installed. Launch test
applications with `DISPLAY=:100 <program>`.

## Documentation

- [config.toml.example](config.toml.example): configuration syntax and examples
- [ARCHITECTURE.md](ARCHITECTURE.md): internal state, invariants, and transition ownership
- [X11.md](X11.md): ICCCM/EWMH behavior and limits
- [IPC.md](IPC.md): socket protocol and event schemas
- [CONTRIBUTING.md](CONTRIBUTING.md): development and verification workflow
- [ROADMAP.md](ROADMAP.md): verified open work and design questions

LWM is licensed under the [MIT License](LICENSE).
