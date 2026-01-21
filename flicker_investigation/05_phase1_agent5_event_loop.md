# Agent 5: Event Loop Race Conditions

## Executive Summary

Analyzed the event loop and XCB event handling for race conditions. Found several architectural factors that could affect event processing.

## Event Loop Architecture (wm.cpp:105-157)

```cpp
while (running_)
{
    int poll_result = poll(&pfd, 1, timeout_ms);
    if (poll_result > 0)
    {
        while (auto event = xcb_poll_for_event(conn_.get()))
        {
            std::unique_ptr<xcb_generic_event_t, decltype(&free)> eventPtr(event, free);
            handle_event(*eventPtr);
        }
    }
    handle_timeouts();
}
```

## Key Finding 1: Unbounded Event Batching

- Uses `xcb_poll_for_event()` in tight loop within single poll() cycle
- **All pending XCB events processed sequentially** before returning to poll()
- Multiple KeyPress/KeyRelease pairs can be queued and processed rapidly
- No event batching limits or yield points

## Key Finding 2: Asynchronous Key Grab Mode

```cpp
xcb_grab_key(conn_.get(), 1, window, mod, *keycode,
             XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC);
```

- X server processes keyboard input asynchronously
- Doesn't wait for WM to acknowledge
- Events can queue up before any are processed

## Key Finding 3: Potential Race Condition Scenario

**Critical scenario identified:**

If KeyRelease arrives between two queued KeyPress events:
1. KeyPress #1: toggle happens, `toggle_key_released_ = false`
2. KeyRelease: `toggle_key_released_ = true`
3. KeyPress #2 (already queued): `!toggle_key_released_` is now false → **toggle happens again!**

However, this scenario may be mitigated by X11's synthetic KeyRelease behavior (see Agent 3).

## Key Finding 4: State Mutation Before Action

```cpp
else if (action->type == "toggle_workspace")
{
    if (keysym == last_toggle_keysym_ && !toggle_key_released_)
        return;
    last_toggle_keysym_ = keysym;
    toggle_key_released_ = false;  // State mutated HERE
    toggle_workspace();            // Action HERE
}
```

If `toggle_workspace()` returns early (e.g., only one workspace), state is still changed, potentially affecting next key press.

## Key Finding 5: No Event Deduplication

Identical or near-identical toggle events are not coalesced - each fully executes.

## Root Window Event Mask

```cpp
uint32_t values[] = {
    XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT | XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY
    | XCB_EVENT_MASK_ENTER_WINDOW | XCB_EVENT_MASK_LEAVE_WINDOW
    | XCB_EVENT_MASK_POINTER_MOTION | XCB_EVENT_MASK_STRUCTURE_NOTIFY
    | XCB_EVENT_MASK_PROPERTY_CHANGE | XCB_EVENT_MASK_BUTTON_PRESS
    | XCB_EVENT_MASK_BUTTON_RELEASE
};
```

**Note:** KeyPress/KeyRelease NOT in mask - they arrive through grabs instead.

## Summary of Concerns

1. Unbounded event batching
2. Async key grab mode
3. State machine race window between KeyRelease and next KeyPress
4. Multiple workspace mutations can interleave visibly
5. No event deduplication
