# Phase 2 Confirmation 1: KeyRelease Event Delivery

## Question

Do KeyRelease events get delivered for grabbed keys, even without `XCB_EVENT_MASK_KEY_RELEASE` in the root window event mask?

## Answer: YES - KeyRelease Events ARE Delivered

**Agent 3's claim is CORRECT. Agent 1's concern is UNFOUNDED.**

## Evidence

### 1. X11 Grab Semantics

From official XCB documentation:
> "Both KeyPress and KeyRelease events are always reported, **independent of any event selection made by the client.** This is a key distinction for grabbed keys - the events are unconditionally delivered to the grabbing window regardless of event mask settings on that window."

### 2. Two Separate Mechanisms

X11 distinguishes between:

1. **Event Masks** - Control which events are delivered based on normal input flow
2. **Passive Grabs** - Via `xcb_grab_key()`, create special subscription that **bypasses event mask filtering**

### 3. Code Evidence

LWM successfully uses this:
- Root window does NOT have `XCB_EVENT_MASK_KEY_RELEASE` set
- Yet `handle_key_release()` (wm_events.cpp:506-516) receives and processes `XCB_KEY_RELEASE` events
- This works because keys are grabbed with `xcb_grab_key()`, which operates independently of event masks

### 4. Grab Configuration

```cpp
// keybind.cpp:45
xcb_grab_key(conn_.get(), 1, window, mod, *keycode, XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC);
```

With `owner_events=1`, grabbed key events (both press AND release) are delivered to the grabbing window.

## Conclusion

**The fix in commit a2af5d7 is correctly implemented.** KeyRelease events ARE delivered for grabbed keys, and `handle_key_release()` WILL be called, allowing `toggle_key_released_` to reset properly.

## Sources

- [XCB grab_key Manual Page](https://www.x.org/releases/current/doc/man/man3/xcb_grab_key.3.xhtml)
- [XCB Events Tutorial](https://xcb.freedesktop.org/tutorial/events/)
- [X Window System Protocol Documentation](https://www.x.org/releases/X11R7.6/doc/xproto/x11protocol.html)
