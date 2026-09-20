#!/usr/bin/env bash
# Codex PreToolUse wrapper: force low-token guardrails by default.
set -euo pipefail

MATCHER="${1:-}"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Enforce strict pre-tool behavior for Codex sessions. Set both names so the
# effective value is seen whichever one pre-tool-hook.sh reads.
export CHITTA_STRICT_MODE="${CHITTA_STRICT_MODE:-${CC_SOUL_STRICT_MODE:-1}}"
export CC_SOUL_STRICT_MODE="$CHITTA_STRICT_MODE"
export CHITTA_SUBAGENT_BASH_RECALL="${CHITTA_SUBAGENT_BASH_RECALL:-${CC_SOUL_SUBAGENT_BASH_RECALL:-0}}"
export CC_SOUL_SUBAGENT_BASH_RECALL="$CHITTA_SUBAGENT_BASH_RECALL"
export CHITTA_DEEP_SEARCH="${CHITTA_DEEP_SEARCH:-${CC_SOUL_DEEP_SEARCH:-0}}"
export CC_SOUL_DEEP_SEARCH="$CHITTA_DEEP_SEARCH"

rc=0
output=$("${SCRIPT_DIR}/pre-tool-hook.sh" "$MATCHER") || rc=$?
# Adapt older daemon responses at the Codex boundary, including before a
# rebuilt daemon is deployed. Never turn an explicit denial into an approval.
if normalized=$(jq -c '
    if (.hookSpecificOutput | type) == "object" then
        .hookSpecificOutput |= (
            if .permissionDecision == "block" then
                .permissionDecision = "deny"
                | .permissionDecisionReason //= .additionalContext
            else . end
            | if .permissionDecision == "deny" then del(.updatedInput)
              elif has("updatedInput") and .permissionDecision == null then
                  .permissionDecision = "allow"
              else . end
        )
    else . end
' <<< "$output" 2>/dev/null); then
    printf '%s' "$normalized"
else
    printf '%s' "$output"
fi
exit "$rc"
