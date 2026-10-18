# Codex usage notes

These examples produced useful results in one codebase review session. They record what worked in that context, rather than general guidance.

## Invocation that worked on this host

In that environment, sandboxed `codex exec` attempts failed because bubblewrap could not create a network namespace or user namespace. Subsequent useful runs used the bypass directly on the repository:

```sh
codex exec -C <repository> \
  --dangerously-bypass-approvals-and-sandbox \
  -m gpt-6.1-sol \
  -o "$OUT" \
  "$(cat "$PROMPT")" \
  > "$LOG" 2>&1
```

Prompts were kept in files, the final answer was written with `-o`, and the full run log was redirected separately. Earlier experiments used a throwaway clone; later runs worked directly in the repository.

## Tasks that returned useful results

Each task below pairs an excerpt from its original prompt with the observed outcome.
Commit ranges and focus areas were specific to that run. The common feature was a
self-contained scope, required evidence, and clear output criteria.

### Independent design study

> Task: a from-scratch design study of the open candidate “Scratchpads as tagged windows.” [...] What does a user actually need? Propose the simplest design that covers those needs. Show the user-facing config and commands. Estimate the net production-line change and state what behaviour would be lost or changed. Recommend whether to do it, do part, or keep the current design. Ground claims about current behaviour in file:line references; disagreement with the premise is welcome.

Codex recommended keeping the current design and explained the trade-offs.

### Adversarial change review

> Read AGENTS.md first, then ARCHITECTURE.md, DECISIONS.md, X11.md and IPC.md. Task: an adversarial review of the six most recent commits [...] Find unintended behaviour changes or regressions; inconsistencies; and missing or weak test coverage. For every finding give file:line, the concrete scenario that breaks, and your confidence. Verify each claim against the code; drop anything you cannot substantiate. Do not modify files or run tests.

A later review found four concrete defects, which were checked and fixed in the main
workstream.

### Test audit

> Audit the quality of the test suite. The bar is: every test must be able to fail. Find weak tests by describing a concrete code change that would still pass, and suggest a concrete assertion or test. Rank findings by importance, verify each claim against the code, and drop anything you cannot substantiate. Do not modify files or run tests.

The audit identified both caught and surviving mutations, including a surviving
oversized-property case.

## Session continuation

The transcript confirmed that `codex exec resume` was available and noted that omitting `--ephemeral` leaves a session resumable. No follow-up was actually made with `resume`; runs were briefed as standalone tasks. Some successful invocations used `--ephemeral`, while later ones omitted it.

## Source

The commands and examples above are drawn from the session transcript.
