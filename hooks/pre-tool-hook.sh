#!/bin/bash
# Safety remains local. All other pre-tool decisions are daemon policy.
MATCHER=${1:-}
[[ -n "$MATCHER" ]] || exit 0
INPUT=$(cat)
exec </dev/null
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/lib.sh"

safety_check() {
    local cmd="$1"
    # Broad MCP matches include the HTTP transport. Only a PID-selection pipeline
    # that explicitly filters --http out may kill other MCP workers.
    if [[ "${CHITTA_ALLOW_MCP_KILL:-0}" != "1" ]]; then
        local _kill_segment _mcp_cmd="${cmd//\[c\]/c}"
        while IFS= read -r _kill_segment; do
            [[ "$_kill_segment" =~ (^|[^[:alnum:]_.-])(pkill|kill|killall)([[:space:]]|$) ]] || continue
            [[ "$_kill_segment" == *chitta-mcp* ]] || continue
            if [[ ! "$_kill_segment" =~ (^|[^[:alnum:]_.-])(pkill|killall)[[:space:]] ]]; then
                local _exclude="grep[[:space:]]+(-[EF]*v[EF]*|--invert-match)[[:space:]]+(--[[:space:]]+)?['\"]?--http['\"]?([[:space:]|)]|$)"
                # The exclusion must filter PID input, not the kill command's output.
                if grep -qE "kill[[:space:]][^|]*\\$\\([^)]*pgrep[^)]*\\|[[:space:]]*$_exclude" <<<"$_kill_segment" ||
                   { [[ ! "${_kill_segment%%grep*}" =~ (^|[^[:alnum:]_.-])kill[[:space:]] ]] &&
                     grep -qE "pgrep[^|]*\\|[[:space:]]*$_exclude.*\\|[^;]*xargs[[:space:]][^;]*kill([[:space:]]|$)" <<<"$_kill_segment"; }; then
                    continue
                fi
            fi
            echo '{"hookSpecificOutput":{"hookEventName":"PreToolUse","permissionDecision":"deny","permissionDecisionReason":"Killing chitta-mcp can sever the HTTP transport. Use scripts/dev-install.sh for managed recovery; CHITTA_ALLOW_MCP_KILL=1 is the explicit environment bypass."}}'
            return 3
        done < <(tr ';&\n' '\n' <<<"$_mcp_cmd")
    fi
    if echo "$cmd" | grep -qE '^\s*rm\s+-rf\s+(/|~/?\s*$)'; then
        echo '{"hookSpecificOutput":{"hookEventName":"PreToolUse","permissionDecision":"block","additionalContext":"rm -rf on / or ~ is destructive"}}'
        return 2
    fi
    if echo "$cmd" | grep -qE '^\s*chmod\s+-R\s+777\s+/\s*$'; then
        echo '{"hookSpecificOutput":{"hookEventName":"PreToolUse","permissionDecision":"block","additionalContext":"chmod -R 777 / is destructive"}}'
        return 2
    fi
    if echo "$cmd" | grep -qE '^\s*dd\s+.*of=/dev/sd[a-z]'; then
        echo '{"hookSpecificOutput":{"hookEventName":"PreToolUse","permissionDecision":"block","additionalContext":"dd writing to raw disk device"}}'
        return 2
    fi
    # Glob/bulk-delete guard. Incident 2026-07-30: an agent ran
    # `rm -f chunk_*.sorted.fq` in the repo cwd, deleting 204 untracked files.
    # Blocks wildcard rm, find -delete/-exec rm, xargs rm, and git clean -f
    # UNLESS a scratch/temp path (/tmp|/dev/shm|/scratch) appears in the same
    # command segment. Explicit single-path deletes and git-clean dry-runs pass.
    # Command is split on ; & | so a scratch path in one segment can't unlock a
    # destructive delete in another. Bypass: prefix CHITTA_ALLOW_GLOB_RM=1
    # (CC_SOUL_ALLOW_GLOB_RM=1 still honored).
    if [[ "${CHITTA_ALLOW_GLOB_RM:-${CC_SOUL_ALLOW_GLOB_RM:-0}}" != "1" ]] && ! grep -qE '(^|[[:space:]])(CHITTA|CC_SOUL)_ALLOW_GLOB_RM=1([[:space:]]|$)' <<<"$cmd"; then
        local _seg _danger=0
        while IFS= read -r _seg; do
            [[ -z "$_seg" ]] && continue
            grep -qE '(^|[^[:alnum:]_.-])(rm(dir)?[[:space:]][^|;&]*[][*?]|find[[:space:]][^|;&]*(-delete([[:space:]]|$)|-exec(dir)?[[:space:]]+rm)|xargs\b[^|]*[[:space:]]rm([[:space:]]|$)|git[[:space:]]+clean[[:space:]][^|;&]*-[[:alnum:]]*f)' <<<"$_seg" || continue
            grep -qE '(/tmp/|/dev/shm/|/scratch/)' <<<"$_seg" && continue
            grep -qE 'git[[:space:]]+clean[^|;&]*(--dry-run|-[[:alnum:]]*n)' <<<"$_seg" && continue
            _danger=1; break
        done < <(tr ';&|\n' '\n' <<<"$cmd")
        if [[ $_danger -eq 1 ]]; then
            echo '{"hookSpecificOutput":{"hookEventName":"PreToolUse","permissionDecision":"deny","permissionDecisionReason":"Bulk/glob delete outside a scratch path blocked (incident 2026-07-30: rm -f chunk_*.sorted.fq deleted 204 untracked files). Delete explicit paths, scope to /tmp|/scratch|/dev/shm, or prefix the command with CHITTA_ALLOW_GLOB_RM=1 if intentional."}}'
            return 3
        fi
    fi
    return 0
}

if [[ "$MATCHER" == Bash ]]; then
    command=$(jq -r '.tool_input.command // empty' <<< "$INPUT" 2>/dev/null)
    [[ -n "$command" ]] || exit 0
    safety_result=$(safety_check "$command")
    safety_rc=$?
    if [[ $safety_rc -eq 2 || $safety_rc -eq 3 ]]; then
        printf '%s\n' "$safety_result"
        [[ $safety_rc -eq 2 ]] && exit 2
        exit 0
    fi
fi
if [[ -n "${CHITTA_HEADLESS:-${CC_SOUL_HEADLESS:-}}" ]]; then printf '{}'; exit 0; fi
ROOT=$(resolve_cc_soul_root) || {
    printf '[chitta] daemon unavailable; context not loaded.\n'
    exit 0
}
printf '%s' "$INPUT" | python3 -S "$ROOT/chitta-mcp/hook_client.py" pre-tool "$MATCHER"
