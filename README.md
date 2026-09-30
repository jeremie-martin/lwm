# LWM

LWM is a small tiling window manager for X11, written in C++23. It provides
master-stack and monocle layouts, floating windows, per-monitor workspaces,
focus-follows-mouse, scratchpads, RANDR hotplug handling, and a local IPC
client named `lwmctl`.

LWM is not a compositor, panel, launcher, or desktop session. Run those as
separate programs, normally from `[autostart]` in the LWM configuration or from
your X session.

## Build

Required tools and libraries:

- CMake 3.20 or newer
- Git and a C++23 compiler
- `pkg-config`
- X11/XCB modules `xcb`, `xcb-keysyms`, `xcb-randr`, `xcb-ewmh`, `xcb-icccm`,
  `xcb-sync`, and `x11`
- `xcb-xtest` and Xvfb for the full test suite

CMake downloads pinned toml++ and Quill sources on the first configure; test
builds also download pinned Catch2 and nlohmann/json sources. JSON parsing is
a test-only dependency used to validate IPC independently of its implementation.

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

Build the release binaries or build and run all tests in Debug with required
X11 integration:

```sh
make
make test
```

The binaries are written to `build/src/app/lwm` and `build/src/app/lwmctl`.
See [CONTRIBUTING.md](CONTRIBUTING.md#build-and-test) for direct CMake use,
focused tests, sanitizers, and separate Debug/Release builds.

## Install and start

```sh
sudo make install
```

With the default CMake prefix this installs `lwm`, `lwmctl`, `lwm-notify`, and `lwm-notify-bridge`
under `/usr/local/bin`.

`sudo make uninstall` removes the files recorded in `build/install_manifest.txt`,
including all four programs. Keep the build directory used for installation;
for another build directory use `make uninstall BUILD_DIR=...`. This honors the
prefix used at install time. For a staged installation, supply the same `DESTDIR`
when uninstalling. Missing files are harmless; directories are left in place.

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
names, and scratchpads. Adjust the terminal and launcher commands before use.

Each monitor has its own workspaces. Commands target the focused monitor;
workspace and monitor indices in configuration and IPC are zero-based. Sticky
windows stay on their owning monitor and appear on all its workspaces. EWMH
uses a [flat desktop list](X11.md#desktops-and-root-properties) across monitors.

Focus follows the pointer. Master-stack tiles windows; monocle gives each tiled
window the full content rectangle. Floating windows retain independent geometry.
A fullscreen window hides other normal windows on its monitor except its own
transients. Scratchpads provide named windows and a generic pool that can be
hidden and recalled on the current workspace.

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
lwmctl state
lwmctl scratchpad list
lwmctl subscribe focus_change,workspace_switch
lwmctl reload-config
lwmctl restart
```

`lwm-notify [notify-send arguments...]` shows a desktop notification when
`notify-send` is installed. When the caller has a numeric `WINDOWID`, it also
marks that exact managed source window urgent; LWM does not guess a source from
notification metadata. The optional `lwm-notify-bridge` forwards desktop
notifications that carry an explicit `x-window-id` hint; it requires `busctl`
and `jq`. Run it in your session only if needed. It does not infer a target from
application names, and reports command errors to stderr.

List commands return JSON and subscriptions stream JSON Lines. See
[IPC.md](IPC.md) for discovery, the raw wire protocol, schemas, and delivery
semantics.

## Logs

LWM sends INFO-and-higher diagnostics to the system journal by default:

```sh
journalctl -b _UID="$(id -u)" SYSLOG_IDENTIFIER=lwm
lwmctl log status
```

Use `--log-level trace|debug|info|warn|error|critical|off` to select verbosity.
`-V` enables DEBUG; `-d all` and `--debug` enable TRACE. For a terminal or a
pipe, use `--log-target stderr` and `--log-color auto|always|never`. For example:

```sh
lwm --log-target stderr --log-level debug 2>&1 | tee lwm.log
```

Direct regular-file redirection is rejected for the stderr target: filesystem
writes can block even with `O_NONBLOCK`. Journal storage, retention, and access
are managed by the host's journal configuration. LWM no longer creates private
rotating files; `--log-file` and `--no-log-file` report a migration error. Existing
log files are left untouched.

Logging is best effort. A full queue or unavailable/slow destination drops
records rather than waiting for the consumer. `lwmctl log status` distinguishes
queue overflow from delivery failures; a successful send means the local socket
accepted the record, not that it was persisted. Missing journal service does not
prevent startup. See [IPC.md](IPC.md#logging-status) for counters and
[ARCHITECTURE.md](ARCHITECTURE.md#logging) for delivery and lifecycle details.

To try LWM without replacing your current window manager, use the
[nested Xephyr preview](CONTRIBUTING.md#nested-preview).

## Documentation

- [config.toml.example](config.toml.example): configuration syntax and examples
- [ARCHITECTURE.md](ARCHITECTURE.md): internal state, invariants, and transition ownership
- [X11.md](X11.md): ICCCM/EWMH behavior and limits
- [IPC.md](IPC.md): socket protocol and event schemas
- [CONTRIBUTING.md](CONTRIBUTING.md): development and verification workflow
- [ROADMAP.md](ROADMAP.md): verified open work and design questions

LWM is licensed under the [MIT License](LICENSE).
