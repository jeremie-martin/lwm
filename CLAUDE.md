# Repository instructions

Start with [README.md](README.md). Read the reference for the affected boundary:

- [CONTRIBUTING.md](CONTRIBUTING.md): build, validation, review, and documentation ownership.
- [ARCHITECTURE.md](ARCHITECTURE.md): state, transitions, geometry, focus, lifecycle, and restart.
- [X11.md](X11.md): ICCCM/EWMH, client messages, hints, and private properties.
- [IPC.md](IPC.md): commands, JSON, discovery, and subscriptions.
- [config.toml.example](config.toml.example): configuration syntax, defaults, and reload.

Use the existing state operations and completion boundary. Test observable contracts,
verify documentation against implementation, and update its owning explanation rather
than adding another copy. Keep code comments focused on local invariants and rationale.
