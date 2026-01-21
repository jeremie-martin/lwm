# Fix Applied: Correct Auto-Repeat Detection

## The Bug

The original fix in commit a2af5d7 had a critical logic flaw:

```cpp
// BROKEN LOGIC:
if (keysym == last_toggle_keysym_ && !toggle_key_released_)
    return;
last_toggle_keysym_ = keysym;
toggle_key_released_ = false;
toggle_workspace();

// In handle_key_release:
toggle_key_released_ = true;
```

### Why It Was Broken

Tracing through the event sequence:

1. **First KeyPress:** `toggle_key_released_=true` initially
   - Condition: `(same_key && !true)` = `(same_key && false)` = FALSE
   - Toggle executes, sets `toggle_key_released_=false`

2. **Synthetic KeyRelease:** Sets `toggle_key_released_=true`

3. **Auto-repeat KeyPress:**
   - Condition: `(same_key && !true)` = `(same_key && false)` = **FALSE**
   - **Toggle executes again!** ← BUG

The synthetic KeyRelease (part of X11 auto-repeat) reset the flag, allowing subsequent auto-repeat KeyPress events through.

## The Correct Fix

X11 auto-repeat sends KeyRelease-KeyPress pairs with **identical timestamps**. We use this to detect auto-repeat:

```cpp
// FIXED LOGIC:
if (keysym == last_toggle_keysym_ && e.time == last_toggle_release_time_)
    return;  // Same key AND same timestamp as KeyRelease = auto-repeat
last_toggle_keysym_ = keysym;
last_toggle_release_time_ = 0;  // Reset on new toggle
toggle_workspace();

// In handle_key_release:
last_toggle_release_time_ = e.time;  // Record timestamp for comparison
```

### Why This Works

1. **First KeyPress (time=T1):** `last_toggle_release_time_=0`
   - Condition: `(same_key && T1 == 0)` = FALSE
   - Toggle executes, resets `last_toggle_release_time_=0`

2. **Synthetic KeyRelease (time=T2):** Records `last_toggle_release_time_=T2`

3. **Auto-repeat KeyPress (time=T2):** Same timestamp as KeyRelease!
   - Condition: `(same_key && T2 == T2)` = **TRUE**
   - **Returns early, toggle blocked!** ✓

4. **Real KeyPress after release (time=T3):** Different timestamp
   - Condition: `(same_key && T3 == T2)` = FALSE (T3 > T2)
   - Toggle executes ✓

## Files Changed

- `src/lwm/wm.hpp`: Changed `toggle_key_released_` (bool) to `last_toggle_release_time_` (xcb_timestamp_t)
- `src/lwm/wm_events.cpp`: Updated handle_key_press and handle_key_release to use timestamp comparison

## Why the Investigation Missed This

The confirmation agents verified that:
1. KeyRelease events ARE delivered (true)
2. Events are processed in FIFO order (true)

But they didn't trace through the actual logic to see that the boolean flag approach was fundamentally broken - the synthetic KeyRelease **resets the flag**, which defeats the purpose.

The timestamp-based approach is immune to this because auto-repeat KeyPress events have the **same timestamp** as their paired KeyRelease, while real KeyPress events have a **later timestamp**.
