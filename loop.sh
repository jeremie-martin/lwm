#!/bin/bash
set -euo pipefail

FILE="@COMPLETE_STATE_MACHINE.md"
iteration=1

# Two modes: "alignment" (verify against implementation) and "structure" (document quality)
PROMPT_ALIGNMENT=$(cat <<'EOF'
The goal is not to create a new document, but to review and refine the existing @COMPLETE_STATE_MACHINE.md, updating it in place.

@COMPLETE_STATE_MACHINE.md should serve as the authoritative reference for the window manager's complete state machine—its states, transitions, behaviors, and edge cases. A well-crafted document of this kind reads as a coherent whole: every section earns its place, concepts appear once and integrate cleanly, and the structure makes the logic easy to follow whether read end-to-end or consulted for specifics.

Launch at least three independent reviewer agents to audit the document. They should assess whether:
- The logic accurately and completely reflects the current implementation
- The document is internally consistent, with no contradictions or undocumented assumptions
- The structure serves the content—sections are well-scoped, logically ordered, and work together as a unified reference

Apply their feedback directly to @COMPLETE_STATE_MACHINE.md. This may mean correcting, clarifying, consolidating, or restructuring—whatever best serves the document's integrity and coherence.

If you made any changes, stage and commit them.
Only edit @COMPLETE_STATE_MACHINE.md.
EOF
)

PROMPT_STRUCTURE=$(cat <<'EOF'
Review @COMPLETE_STATE_MACHINE.md as a document—not for technical accuracy, but for how well it works as a reference.

Read it end-to-end and consider: Does the structure serve the content? Are there sections that overlap or repeat each other? Are there places where the same concept is explained twice, or where consolidating would make things clearer? Does each section feel necessary and well-placed?

If you identify opportunities to improve the document's coherence—through restructuring, merging, or tightening—apply them directly. But only make changes that genuinely improve clarity or reduce unnecessary complexity; don't reorganize for the sake of reorganizing.

If you made any changes to @COMPLETE_STATE_MACHINE.md, stage and commit them.
Only edit @COMPLETE_STATE_MACHINE.md.
EOF
)

echo "Starting state machine reviewer loop"
echo "Repo: $(pwd)"
echo "File: $FILE"
echo "Alternating: alignment → structure → alignment → ..."
echo "Press Ctrl+C to stop"
echo ""

trap 'echo ""; echo "Stopping."; exit 0' INT

has_changes_for_file() {
  if ! git diff --quiet -- "$FILE" 2>/dev/null; then
    return 0
  fi
  if ! git diff --cached --quiet -- "$FILE" 2>/dev/null; then
    return 0
  fi
  return 1
}

commit_touches_file() {
  git show --name-only --pretty="" HEAD | grep -Fxq "$FILE"
}

append_iteration_footer_to_head_commit() {
  local mode="$1"
  local oldmsg newmsg footer
  oldmsg="$(git log -1 --pretty=%B)"
  footer="[auto] reviewer-loop iteration $iteration ($mode)"

  if printf "%s" "$oldmsg" | grep -Fq "[auto] reviewer-loop iteration $iteration"; then
    echo "→ Iteration footer already present; skipping amend"
    return 0
  fi

  newmsg="${oldmsg}

${footer}"

  git commit --amend -m "$newmsg" --no-verify >/dev/null
  echo "→ Appended iteration footer ($mode)"
}

while true; do
  # Alternate: odd = alignment, even = structure
  if (( iteration % 2 == 1 )); then
    mode="alignment"
    PROMPT="$PROMPT_ALIGNMENT"
  else
    mode="structure"
    PROMPT="$PROMPT_STRUCTURE"
  fi

  echo "──────────────────────────────────────────────"
  echo "[$(date '+%Y-%m-%d %H:%M:%S')] Iteration $iteration [$mode]"

  before_head="$(git rev-parse HEAD)"
  opencode run "$PROMPT" -m zai-coding-plan/glm-4.7 || {
    echo "→ opencode returned non-zero; skipping commit handling"
    ((iteration++))
    continue
  }
  after_head="$(git rev-parse HEAD)"

  if [[ "$after_head" != "$before_head" ]]; then
    echo "→ New commit detected"
    if commit_touches_file; then
      append_iteration_footer_to_head_commit "$mode"
    else
      echo "→ WARNING: commit does not touch $FILE"
    fi
  else
    if has_changes_for_file; then
      echo "→ Uncommitted changes; committing from script"
      git add "$FILE"
      git commit -m "Update $FILE (iteration $iteration, $mode)" --no-verify >/dev/null
      append_iteration_footer_to_head_commit "$mode"
    else
      echo "→ No changes"
    fi
  fi

  echo "[$(date '+%Y-%m-%d %H:%M:%S')] Iteration $iteration done."
  echo ""

  ((iteration++))
done