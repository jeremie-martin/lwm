# LWM

LWM is a tiling window manager for X11, written in C++26. It provides master-stack
and monocle layouts, floating windows, per-monitor workspaces, focus-follows-mouse,
scratchpads, RandR hotplug handling, and the `lwmctl` control client.

Run a compositor, panel, launcher, and desktop services separately, from
`~/.xinitrc` or your display manager's session script.

## Build

You need CMake 3.30+, Git, pkg-config, and GCC 16.2+ with C++26 reflection.
Ordinary Clang releases are insufficient; an experimental P2996 implementation can
use `-freflection-latest`, but GCC is the tested compiler. Check these versions
before using distribution packages.

System libraries are libsystemd and the X11/XCB modules `xcb`, `xcb-keysyms`,
`xcb-randr`, `xcb-ewmh`, `xcb-icccm`, and `x11`. Tests also require
`xcb-xtest` and Xvfb. For example, on Debian/Ubuntu:

```sh
sudo apt install cmake g++ git pkg-config libsystemd-dev libx11-dev libxcb1-dev \
  libxcb-keysyms1-dev libxcb-randr0-dev libxcb-ewmh-dev \
  libxcb-icccm4-dev libxcb-xtest0-dev xvfb
```

On Arch Linux:

```sh
sudo pacman -S --needed cmake gcc git pkgconf libx11 libxcb systemd-libs xcb-util-keysyms xcb-util-wm xorg-server-xvfb
```

CMake fetches pinned toml++, reflect-cpp, and Quill sources; test builds also fetch
Catch2 and nlohmann/json. Build the Release binaries with:

```sh
make
```

They appear at `build/src/app/lwm` and `build/src/app/lwmctl`. See
[TESTING.md](TESTING.md#build-and-test) for validation builds.

With `make`, select a non-default compiler at first configuration with
`CMAKE_OPTIONS="-DCMAKE_CXX_COMPILER=/path/to/g++"` or
`CMAKE_OPTIONS="-DCMAKE_TOOLCHAIN_FILE=/path/to/toolchain.cmake"`.
`CMAKE` selects the CMake executable; `BUILD_DIR` selects the build directory.
Dependency revisions are pinned in [CMakeLists.txt](CMakeLists.txt);
`-DFETCHCONTENT_SOURCE_DIR_<NAME>=/path/to/checkout` in `CMAKE_OPTIONS` can reuse
a local checkout.

## Install and start

```sh
sudo make install
config_dir="${XDG_CONFIG_HOME:-$HOME/.config}/lwm"
mkdir -p "$config_dir"
cp config.toml.example "$config_dir/config.toml"
```

Edit the terminal and launcher commands in that file. Start LWM from `.xinitrc`
or an X11 display-manager session:

```sh
/usr/local/bin/lwm --config "$config_dir/config.toml"
```

LWM refuses to start alongside another window manager. To try it inside an
existing desktop, use the [nested preview](TESTING.md#nested-preview).

Without `--config`, LWM reads `${XDG_CONFIG_HOME:-$HOME/.config}/lwm/config.toml`;
an absent implicit file means built-in defaults. A missing explicit file or an
invalid one is logged at critical level and LWM starts with the defaults, so a
mistake cannot end a session that LWM leads; a failed reload keeps the active
configuration. `lwm --check-config [--config PATH]` validates a file without a
display and exits nonzero with the error. Key grabs follow keyboard mapping changes,
such as `setxkbmap`. `lwm --help` lists startup options; value options accept
`--option VALUE` or `--option=VALUE`.

Installation defaults to `/usr/local/bin` and includes `lwm`, `lwmctl`,
`lwm-notify`, and `lwm-notify-bridge`. Set `CMAKE_INSTALL_PREFIX` and
`CMAKE_INSTALL_BINDIR` through `CMAKE_OPTIONS` to change it. `sudo make uninstall`
removes the files in that build's install manifest. Keep the build directory;
use the same `BUILD_DIR` and, for staged installations, `DESTDIR`. Missing files
are harmless and directories are left in place.

## Configure and control

[config.toml.example](config.toml.example) is the configuration reference, starter
file and built-in default. A file is the whole configuration: only what it binds is
bound. Key bindings map a combo to the command text `lwmctl` takes, as in
`"super+f" = "window fullscreen"`, or to the argv list of a program to launch, as in
`"super+Return" = ["ghostty"]`. Workspace names define the workspace count, and a
`[[workspace_keys]]` group binds one keyboard layout's keys to switch and move. Rule
placement uses `workspace` and `monitor`, each accepting an index or
a name; rule layers use `layer = "normal"`, `"above"`, or `"below"`; rule window types
are the lowercase names of types that become clients, e.g. `dialog`. Mouse bindings
take `move`, `resize`, or what a key binding takes. Invalid
reloads keep the active configuration.

Each monitor has its own workspaces. Commands target the focused monitor and use
zero-based indices.

```sh
lwmctl --help
lwmctl workspace switch 2
lwmctl layout set monocle
lwmctl ratio adjust -0.05
lwmctl window float
lwmctl window to-workspace 3
lwmctl config reload
lwmctl state
lwmctl watch
```

Use `lwmctl restart` to replace the running process. A failed exec reconstructs
the WM from its prepared handoff. Handoffs are private to one format version:
restarting into an incompatible version retains EWMH workspace placement but
falls back to fresh adoption for private state. Startup and unexpected runtime
failures exit with a diagnostic rather than entering a restart loop.

For scripts and panels, [IPC.md](IPC.md) defines the commands, the X-based transport,
and the JSON state that `lwmctl watch` streams. [X11.md](X11.md) defines application-facing behavior and
intentional protocol limits.

## Diagnostics and notifications

Logs go to the system journal at INFO and above:

```sh
journalctl -b _UID="$(id -u)" SYSLOG_IDENTIFIER=lwm
```

For terminal diagnostics:

```sh
lwm --config "$config_dir/config.toml" --log-target stderr --log-level debug 2>&1 | tee lwm.log
```

DEBUG explains classification, placement, focus, and fullscreen decisions; TRACE
adds binding and geometry submissions. Window IDs match `lwmctl window list`.
Logging options apply at startup, not on TOML reload. Journal retention is managed
by the host; use stderr redirection for a file.

Logging is asynchronous and best effort. A full queue drops new records so a slow
sink does not block ordinary WM operations. **Normal shutdown drains the worker
and can wait for a stalled output destination.** Exec restart does not drain and
may lose queued messages. The [status schema](IPC.md#logging-status) explains what
backend notifications measure.

`lwm-notify [notify-send arguments...]` shows a notification and, when `WINDOWID`
is numeric, marks that managed source window urgent. The optional
`lwm-notify-bridge` forwards explicit `x-window-id` hints from desktop
notifications; it requires `busctl` and `jq`. Neither guesses a window from an
application name. The wrapper is best effort and suppresses errors; it displays
notifications only when `notify-send` is installed. The bridge reports command errors
on stderr.

The model and its ownership boundaries are described in
[ARCHITECTURE.md](ARCHITECTURE.md).

LWM is licensed under the [MIT License](LICENSE).
