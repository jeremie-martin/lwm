#!/bin/bash
set -euo pipefail

FILE="@COMPLETE_STATE_MACHINE.md"
iteration=1

echo "Starting state machine reviewer loop"
echo "Repo: $(pwd)"
echo "File: $FILE"
echo "Press Ctrl+C to stop"
echo ""

trap 'echo ""; echo "Stopping."; exit 0' INT

has_changes_for_file() {
  # Returns 0 if there are staged OR unstaged changes for $FILE, else 1.
  if ! git diff --quiet -- "$FILE" 2>/dev/null; then
    return 0
  fi
  if ! git diff --cached --quiet -- "$FILE" 2>/dev/null; then
    return 0
  fi
  return 1
}

commit_touches_file() {
  # Returns 0 if HEAD commit includes $FILE in its diff, else 1.
  git show --name-only --pretty="" HEAD | grep -Fxq "$FILE"
}

revert_other_files() {
  # Revert any modified/staged files except $FILE.
  local other
  other="$(git status --porcelain | awk '{print $2}' | grep -v -Fx "$FILE" || true)"
  if [[ -n "$other" ]]; then
    echo "→ Reverting changes to non-target files:"
    echo "$other" | sed 's/^/   - /'
    while IFS= read -r f; do
      [[ -z "$f" ]] && continue
      git restore --staged --worktree -- "$f" 2>/dev/null || true
    done <<< "$other"
  fi
}

append_iteration_footer_to_head_commit() {
  local oldmsg newmsg footer
  oldmsg="$(git log -1 --pretty=%B)"
  footer="[auto] reviewer-loop iteration $iteration"

  # Avoid appending twice if rerun
  if printf "%s" "$oldmsg" | grep -Fq "$footer"; then
    echo "→ Iteration footer already present in commit message; skipping amend"
    return 0
  fi

  newmsg="${oldmsg}

${footer}"

  git commit --amend -m "$newmsg" --no-verify >/dev/null
  echo "→ Appended iteration footer to commit message"
}

while true; do
  echo "──────────────────────────────────────────────"
  echo "[$(date '+%Y-%m-%d %H:%M:%S')] Iteration $iteration starting..."

  PROMPT=$(cat <<'EOF'
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

  before_head="$(git rev-parse HEAD)"
  opencode run "$PROMPT" -m zai-coding-plan/glm-4.7 || {
    echo "→ opencode returned non-zero; skipping commit handling for this iteration"
    ((iteration++))
    continue
  }
  revert_other_files
  after_head="$(git rev-parse HEAD)"

  if [[ "$after_head" != "$before_head" ]]; then
    echo "→ Detected new commit created by agent"
    if commit_touches_file; then
      append_iteration_footer_to_head_commit
    else
      echo "→ WARNING: latest commit does not touch $FILE; leaving it as-is"
    fi
  else
    if has_changes_for_file; then
      echo "→ Agent changed $FILE but did not commit; committing from script"
      git add "$FILE"
      git commit -m "Update $FILE (iteration $iteration)" --no-verify >/dev/null
      append_iteration_footer_to_head_commit
    else
      echo "→ No changes detected for $FILE"
    fi
  fi

  echo "[$(date '+%Y-%m-%d %H:%M:%S')] Iteration $iteration done."
  echo "Next iteration starting immediately..."
  echo ""

  ((iteration++))
done

