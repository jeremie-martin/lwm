# Repository instructions

Start with [README.md](README.md), then load only the reference that owns the
task:

- For build, test, style, or review work, read
  [CONTRIBUTING.md](CONTRIBUTING.md).
- Before changing client state, visibility, focus, fullscreen, stacking,
  workspaces, restart, scratchpads, or hotplug behavior, read
  [ARCHITECTURE.md](ARCHITECTURE.md).
- Before changing ICCCM, EWMH, classification, hints, client messages, or
  `_LWM_*` properties, read [X11.md](X11.md).
- Before changing socket commands, JSON, discovery, or events, read
  [IPC.md](IPC.md).
- Before changing configuration parsing or defaults, read and update
  [config.toml.example](config.toml.example).

Use the existing state-transition funnels and add tests through the real
boundary. Follow the documentation ownership table in
[CONTRIBUTING.md](CONTRIBUTING.md); update one authoritative explanation rather
than copying behavior between files.
