# LWM Documentation Index

Welcome to the LWM documentation. This document provides a roadmap for navigating the documentation system.

## Quick Start

If you're new to LWM:
1. Read [README.md](README.md) for installation and basic usage
2. Review [BEHAVIOR.md](BEHAVIOR.md) to understand key concepts and user-facing behavior
3. Check [config.toml.example](config.toml.example) for configuration options

## Documentation by Audience

### For Users
- **[README.md](README.md)** - Quick start guide, features, installation, configuration
- **[BEHAVIOR.md](BEHAVIOR.md)** - User-facing behavior: focus model, workspaces, monitors, window rules

### For Contributors
- **[CLAUDE.md](CLAUDE.md)** - Development guide, architecture overview, code conventions, quick reference

### For Protocol/Compliance
- **[COMPLIANCE.md](COMPLIANCE.md)** - ICCCM/EWMH protocol requirements (normative, testable)
- **[SPEC_CLARIFICATIONS.md](SPEC_CLARIFICATIONS.md)** - Design decisions on ambiguous spec points

### For Implementation Details
- **[IMPLEMENTATION.md](IMPLEMENTATION.md)** - Architecture overview, data structures, invariants, error handling
- **[STATE_MACHINE.md](STATE_MACHINE.md)** - Window state machine, state transitions, lifecycle
- **[EVENT_HANDLING.md](EVENT_HANDLING.md)** - Event-by-event handling specifications

**Note**: [COMPLETE_STATE_MACHINE.md](COMPLETE_STATE_MACHINE.md) is superseded by the documents above and retained only for reference.

### For Future Work
- **[FEATURE_IDEAS.md](FEATURE_IDEAS.md)** - Feature backlog, design principles, implementation ideas

### Legacy Documentation
- **[COMPLETE_STATE_MACHINE.md](COMPLETE_STATE_MACHINE.md)** - **⚠️ SUPERSEDED** - Historical reference only. Replaced by IMPLEMENTATION.md, STATE_MACHINE.md, and EVENT_HANDLING.md. Retained for transition reference.

## Key Concepts by Topic

### Window Management
- Classification: [BEHAVIOR.md §1.3](BEHAVIOR.md#13-window-classes-behavioral)
- States: [STATE_MACHINE.md §3](STATE_MACHINE.md#3-window-states)
- Visibility: [IMPLEMENTATION.md §5](IMPLEMENTATION.md#5-visibility-and-off-screen-positioning)

### Focus System
- Focus model: [BEHAVIOR.md §2](BEHAVIOR.md#2-focus-and-active-monitor-policy)
- Focus eligibility: [STATE_MACHINE.md §6](STATE_MACHINE.md#6-focus-system)
- Focus restoration: [BEHAVIOR.md §2.4](BEHAVIOR.md#24-focus-restoration)

### Workspaces and Monitors
- Per-monitor workspaces: [BEHAVIOR.md §1](BEHAVIOR.md#1-concepts-and-model)
- Workspace switching: [STATE_MACHINE.md §10](STATE_MACHINE.md#10-workspace-management)
- Multi-monitor behavior: [BEHAVIOR.md §4](BEHAVIOR.md#4-monitor-behavior)

### Protocol Compliance
- ICCCM: [COMPLIANCE.md §1](COMPLIANCE.md#icccm-compliance)
- EWMH: [COMPLIANCE.md §2](COMPLIANCE.md#ewmh-compliance)
- Design decisions: [SPEC_CLARIFICATIONS.md](SPEC_CLARIFICATIONS.md)

## Common Questions

**How does LWM handle window visibility?**
→ See [IMPLEMENTATION.md §5](IMPLEMENTATION.md#5-visibility-and-off-screen-positioning) for the off-screen positioning approach

**What states can a window be in?**
→ See [STATE_MACHINE.md §3](STATE_MACHINE.md#3-window-states) for the complete state model

**How are sticky windows handled?**
→ See [SPEC_CLARIFICATIONS.md §3](SPEC_CLARIFICATIONS.md#3-sticky-window-scope) for design rationale

**What EWMH atoms are supported?**
→ See [COMPLIANCE.md §1](COMPLIANCE.md#1-root-window-properties-write-and-maintain)

**How do I add a feature?**
→ See [CLAUDE.md §Adding Features](CLAUDE.md#adding-features) for contribution guidelines

## Documentation Conventions

- **Normative**: MUST/SHOULD language indicates testable requirements (COMPLIANCE.md)
- **Descriptive**: Explains current behavior without prescribing (BEHAVIOR.md, IMPLEMENTATION.md)
- **Rationale**: Explains design decisions (SPEC_CLARIFICATIONS.md)
- **Reference**: Complete implementation details (STATE_MACHINE.md, EVENT_HANDLING.md)

## Source Code References

Documentation includes specific source code citations (e.g., `wm.cpp:1234`) for verification. Key source files:
- `wm.cpp` - Main window management
- `wm_events.cpp` - Event handlers
- `wm_focus.cpp` - Focus system
- `wm_workspace.cpp` - Workspace operations
- `wm_floating.cpp` - Floating windows
- `src/lwm/core/types.hpp` - Core data structures
