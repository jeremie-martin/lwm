# Phase 2 Confirmation 2: Event Queue Ordering

## Question

Can a race condition occur where KeyRelease resets `toggle_key_released_` before a subsequent queued KeyPress is processed?

## Answer: NO - Race Condition is Impossible

**Agent 3's claim is CORRECT. Agent 5's race condition scenario CANNOT occur.**

## X11 Auto-Repeat Event Sequence

When a key is held down (with DetectableAutoRepeat disabled - the default):

```
Physical KeyPress → [synthetic KeyRelease → synthetic KeyPress]* → Physical KeyRelease
```

**Critical:** Each synthetic KeyRelease is **immediately preceded** by a synthetic KeyPress (from the same auto-repeat cycle), not followed by one.

## Event Processing Sequence

For a held toggle key:

| Event | Type | `toggle_key_released_` Before | Action | `toggle_key_released_` After |
|-------|------|------------------------------|--------|------------------------------|
| 1 | Physical KeyPress | true | **TOGGLE EXECUTES** | false |
| 2 | Synthetic KeyRelease | false | Reset flag | true |
| 3 | Synthetic KeyPress | true | Check fails, **IGNORED** | false |
| 4 | Synthetic KeyRelease | false | Reset flag | true |
| 5 | Synthetic KeyPress | true | Check fails, **IGNORED** | false |
| ... | (pattern continues) | | | |
| N | Physical KeyRelease | false | Reset flag | true |

## Why Agent 5's Race Condition is Impossible

1. **X11 Protocol guarantees FIFO ordering** - events cannot be reordered
2. **Synthetic KeyRelease comes AFTER its paired KeyPress** - not before the next one
3. **Event loop processes one event at a time** - each event fully handled before next

When Event 2 (synthetic KeyRelease) sets `toggle_key_released_ = true`:
- Event 3 (synthetic KeyPress) is next in queue
- Event 3 checks `!toggle_key_released_` which is now FALSE
- Event 3 is correctly **ignored**

## Proof by Contradiction

If Agent 5's scenario were possible:
- Events would need to arrive as: KeyPress → KeyPress → KeyRelease
- But X11 does NOT generate this sequence with default settings
- DetectableAutoRepeat would be required, but LWM doesn't enable it

## Conclusion

**The fix in commit a2af5d7 is correct.** The X11 protocol's guaranteed FIFO ordering and synthetic KeyRelease placement ensure that auto-repeat KeyPress events are always correctly blocked.

## Sources

- [X Window System Protocol Specification](https://www.x.org/releases/X11R7.7/doc/xproto/x11protocol.html)
- [XkbSetDetectableAutoRepeat(3) Manual](https://www.x.org/releases/X11R7.6/doc/man/man3/XkbSetDetectableAutoRepeat.3.xhtml)
- [XCB Events Tutorial](https://xcb.freedesktop.org/tutorial/events/)
