#!/usr/bin/env bash
# G0 wrapper: run the golden-set recall grader, then record the baseline
# score into chitta as a provenance signal.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# The frozen replica has no background work to quiesce. Preserve the live
# daemon's legacy quiesce behavior when no replica socket was requested.
quiesce=""
if [[ -z "${CHITTA_EVAL_SOCKET:-}" ]]; then
    quiesce="${MIND:-$HOME/.claude/mind}/.quiesce"
    touch "$quiesce"
    trap 'rm -f "$quiesce"' EXIT
fi

out="$(python3 "$here/grade-recall.py" "$@")" || rc=$? || true
rc="${rc:-0}"
printf '%s\n' "$out"

score="$(printf '%s\n' "$out" | sed -n 's/^SCORE=//p' | tail -1)"
date="$(date -u +%Y-%m-%dT%H:%M:%SZ)"

if [ -n "$score" ]; then
    socket_args=()
    if [[ -n "${CHITTA_EVAL_SOCKET:-}" ]]; then
        socket_args+=(--socket-path "$CHITTA_EVAL_SOCKET")
    fi
    chitta remember \
        --content "[done] grade-recall baseline score:$score date:$date" \
        --kind signal --realm cc-soul --tags "grader,baseline,provenance" \
        "${socket_args[@]}" || true
fi

exit "$rc"
