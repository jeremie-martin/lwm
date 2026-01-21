# Workspace Toggle Flickering Investigation

Investigation into the workspace toggle flickering issue, analyzing commit a2af5d7 which attempted to fix the problem by tracking key press/release state.

## Quick Summary

**VERDICT: The fix in commit a2af5d7 was BROKEN. A corrected fix has been applied.**

The original fix used a boolean flag that got reset by synthetic KeyRelease events (part of X11 auto-repeat), defeating its purpose. The corrected fix uses timestamp comparison to detect auto-repeat.

## Investigation Documents

| File | Description |
|------|-------------|
| [00_investigation_plan.md](00_investigation_plan.md) | Original investigation methodology |
| [01_phase1_agent1_implementation_analysis.md](01_phase1_agent1_implementation_analysis.md) | Code analysis of toggle key handling |
| [02_phase1_agent2_flickering_sources.md](02_phase1_agent2_flickering_sources.md) | Search for other flickering sources |
| [03_phase1_agent3_x11_key_handling.md](03_phase1_agent3_x11_key_handling.md) | X11 auto-repeat research |
| [04_phase1_agent4_switch_workspace.md](04_phase1_agent4_switch_workspace.md) | switch_workspace function analysis |
| [05_phase1_agent5_event_loop.md](05_phase1_agent5_event_loop.md) | Event loop race condition analysis |
| [06_phase1_summary.md](06_phase1_summary.md) | Summary of Phase 1 findings and discrepancies |
| [07_phase2_confirmation1_keyrelease_delivery.md](07_phase2_confirmation1_keyrelease_delivery.md) | Confirmation: KeyRelease IS delivered |
| [08_phase2_confirmation2_event_ordering.md](08_phase2_confirmation2_event_ordering.md) | Confirmation: Race condition is impossible |
| [09_final_conclusions.md](09_final_conclusions.md) | Initial conclusions (later revised) |
| [10_fix_applied.md](10_fix_applied.md) | **THE ACTUAL BUG AND FIX** |

## Key Findings

### The Actual Bug

The original fix (commit a2af5d7) used a boolean `toggle_key_released_` flag:
- Set to `false` on KeyPress
- Set to `true` on KeyRelease
- Block if same key AND flag is `false`

**Problem:** X11 auto-repeat sends synthetic KeyRelease events between each auto-repeat KeyPress. These reset the flag to `true`, allowing the next auto-repeat KeyPress through!

### The Correct Fix

X11 auto-repeat KeyRelease-KeyPress pairs have **identical timestamps**. The fix compares timestamps:

```cpp
// Block if same key AND timestamp matches the KeyRelease (= auto-repeat)
if (keysym == last_toggle_keysym_ && e.time == last_toggle_release_time_)
    return;
```

### Why the Investigation Initially Missed This

Agents confirmed that:
1. KeyRelease events ARE delivered ✓
2. Events are processed in FIFO order ✓

But they traced through the X11 event sequence without tracing through the **actual code logic** to see that the boolean flag gets reset by each synthetic KeyRelease.

## Methodology

- **Phase 1:** 5 agents investigated in parallel from different angles
- **Phase 2:** 2 agents confirmed/debunked conflicting findings
- **Phase 3:** Synthesized results into final conclusions

## Conclusion

The corrected fix (timestamp-based auto-repeat detection) should now properly prevent key auto-repeat from triggering multiple toggles.

**Lesson learned:** When verifying a fix, trace through the actual code logic with specific values, don't just verify high-level assumptions about event delivery.
