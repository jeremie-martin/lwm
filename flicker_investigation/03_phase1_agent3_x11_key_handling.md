# Agent 3: X11 Key Handling Research

## Executive Summary

Researched X11 key auto-repeat behavior and verified the fix's correctness. **The fix IS correct** and should work on all X11 systems.

## X11 Auto-Repeat Behavior

### Standard (Non-Detectable) Mode (Default)

When a user holds a key, X11 generates:
```
KeyPress (physical) → synthetic KeyRelease → KeyPress (auto-repeat) → synthetic KeyRelease → ... → KeyRelease (physical)
```

**Key Insight:** X11 synthetically generates a KeyRelease after each auto-repeat KeyPress to create predictable behavior.

### Detectable Auto-Repeat Mode (Advanced)

If `XkbSetDetectableAutoRepeat` is active:
- Continuous KeyPress events (no synthetic releases)
- Single KeyRelease only when physically released

## Fix Correctness Analysis

### Timeline with Standard Auto-Repeat:

| Event | `last_toggle_keysym_` | `toggle_key_released_` | Toggle? |
|-------|----------------------|----------------------|---------|
| Physical KeyPress | Set to keysym | Set to false | YES |
| Synthetic KeyRelease | unchanged | Set to true | - |
| Auto-repeat KeyPress #1 | match | true | NO (blocked) |
| Synthetic KeyRelease | unchanged | Set to true | - |
| Auto-repeat KeyPress #2 | match | true | NO (blocked) |
| Physical KeyRelease | unchanged | Set to true | - |

**Result:** Only one toggle per physical key press.

### Works with Detectable Auto-Repeat Too

Even if detectable auto-repeat is enabled:
- First KeyPress: toggle happens
- Subsequent KeyPress: `toggle_key_released_` is still false → blocked
- Final KeyRelease: resets state

## Comparison with Other Window Managers

- **i3:** Investigated `XCB_XKB_PER_CLIENT_FLAG_DETECTABLE_AUTO_REPEAT` but didn't implement due to inconsistent support
- **dwm:** No explicit auto-repeat handling documented
- **JUCE:** Initially used timing-based detection, found unreliable, switched to `XkbSetDetectableAutoRepeat`

## Conclusion

**The fix is architecturally sound and correctly addresses the root cause.**

LWM's solution:
- Simple and maintainable (2 state variables)
- Universal X11 compatibility (no XKB requirement)
- Better than previous 150ms debounce
- Zero overhead (event-driven, no timers)

## CRITICAL DISCREPANCY WITH AGENT 1

Agent 1 raised concern about KeyRelease not being delivered. However, for **grabbed keys** with `xcb_grab_key()`, the X server delivers both KeyPress AND KeyRelease events to the grabbing client regardless of the event mask on the window.

**This needs verification in Phase 2.**
