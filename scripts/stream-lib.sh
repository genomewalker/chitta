#!/usr/bin/env bash
# Shared worker lifecycle, sourced by both launchers.
stream_context() {
    cat "$ROOT/codex-plugin/stream-contract.md"
    printf '\n## What chitta knows about this task\n\n'
    timeout 25 "$CLI" recall --query "$title" --realm chitta --tag decision --limit 4 --sources false 2>/dev/null | cut -c1-600 || true
    timeout 25 "$CLI" recall --query "stream=$name handoff $title" --realm chitta --tag handoff --limit 3 --sources false 2>/dev/null | cut -c1-600 || true
    printf '\n## Code map\n\n'
    timeout 30 "$CLI" code_query --question "$title" --path "$W" --limit 12 2>/dev/null | head -n 60 || true
    printf '\n## Task\n\n'
    cat "$task"
}

stream_check() {
    printf 'stream %s: worker PID %s\n' "$name" "$(cat "$S/$name.pid" 2>/dev/null || echo none)"
    stream_rpc stream_list "$(jq -nc --arg stream "$name" '{stream:$stream}')"
    "$CLI" recall --query "stream=$name handoff" --realm chitta --tag handoff --limit 3 --sources false
}

stream_rpc() {
    jq -nc --arg op "$1" --argjson args "$2" '{jsonrpc:"2.0",id:1,method:"tools/call",params:{name:"ledger_op",arguments:{op:$op,args:$args}}}' |
        "$CLI" 2>/dev/null | jq -e 'select(.result.isError == false) | .result.structured.value'
}

stream_claim() {
    claim_args=$(jq -nc --arg stream "$name" --arg session_id "$holder" --arg worktree "$W" --arg branch "$branch" --arg title "$title" '{stream:$stream,session_id:$session_id,worktree:$worktree,branch:$branch,title:$title}')
    local result
    result=$(stream_rpc stream_claim "$claim_args") || return
    printf '%s\n' "$result" > "$S/$name.claim.json"
    jq -e '.. | objects | select(.claimed? == true)' <<< "$result" >/dev/null
}

stream_release() {
    stream_rpc stream_release "$(jq -nc --arg stream "$name" --arg session_id "$holder" '{stream:$stream,session_id:$session_id}')" >/dev/null
}

stream_launch() {
    local lead=${CHITTA_LEAD_SESSION:-}
    [[ "$name" =~ ^[a-zA-Z0-9][a-zA-Z0-9._-]*$ ]] || return 2
    [[ ! -f "$S/$name.pid" ]] || ! kill -0 "$(<"$S/$name.pid")" 2>/dev/null || {
        echo "stream $name already running" >&2; return 1;
    }
    exec {stream_lock}>"$W/.stream.lock"
    flock -n "$stream_lock" || { echo "worktree already has a worker: $W" >&2; return 1; }
    local prompt="$S/$name.prompt.md"
    cat "$HOME/.claude/agent_safety_preamble.md" "$context" > "$prompt"
    printf '\n## Coordination\nStream: %s\nLead session: %s\nWrite the final handoff line to %s/%s.handoff.\n' "$name" "$lead" "$S" "$name" >> "$prompt"
    holder=$(cat /proc/sys/kernel/random/uuid)
    stream_claim || { echo "stream claim failed: $name" >&2; return 1; }
    printf 'Holder session: %s\nUse ledger_op stream_handoff with stream, session_id and content for intermediate handoffs; this renews the claim. Write the final handoff file only; the supervisor persists and messages it.\n' "$holder" >> "$prompt"
    rm -f "$S/$name.handoff" "$S/$name.pid"
    if [[ $1 == claude ]]; then
        export CLAUDE_CONFIG_DIR="$S/$name.claude"
        mkdir -p "$CLAUDE_CONFIG_DIR"
        set -- "$@" --session-id "$holder"
    fi
    export ROOT S CLI W name lead holder claim_args
    if [[ ${CHITTA_STREAM_WAIT:-0} == 1 ]]; then
        bash "$ROOT/scripts/stream-lib.sh" --supervise "$@" < "$prompt" > "$S/$name.log" 2>&1
        return $?
    fi
    nohup setsid bash "$ROOT/scripts/stream-lib.sh" --supervise "$@" < "$prompt" > "$S/$name.log" 2>&1 &
    exec {stream_lock}>&-
    printf 'stream %s launched; PID file %s/%s.pid; log %s/%s.log\n' "$name" "$S" "$name" "$S" "$name"
}

if [[ ${1:-} == --supervise ]]; then
    set -euo pipefail
    shift
    cd "$W"
    cleanup() {
        stream_release || echo 'ERROR: stream release failed' >&2
        rm -f "$S/$name.pid"
    }
    trap cleanup EXIT
    exec 3<&0
    if [[ $1 == codex ]]; then
        setsid "$@" "$(cat <&3)" </dev/null &
    else
        setsid "$@" <&3 &
    fi
    worker=$!
    printf '%s\n' "$worker" > "$S/$name.pid"
    interrupted=0
    stop_worker() {
        interrupted=1
        kill -TERM -- "-$worker" 2>/dev/null || kill -TERM "$worker" 2>/dev/null || true
    }
    trap stop_worker TERM INT
    status=0
    wait "$worker" || status=$?
    if ((interrupted)); then
        wait "$worker" 2>/dev/null || true
        status=143
    fi
    if [[ -s "$S/$name.handoff" && $status == 0 ]]; then
        handoff=$(<"$S/$name.handoff")
    else
        ((status != 0)) || status=1
        handoff="[handoff] stream=$name gates=fail:worker-exit-$status next=inspect-$S/$name.log"
        printf '%s\n' "$handoff" > "$S/$name.handoff"
    fi
    # The supervisor owns final persistence/delivery, preventing duplicate messages.
    stream_rpc stream_handoff "$(jq -nc --arg stream "$name" --arg session_id "$holder" --arg content "$handoff" '{stream:$stream,session_id:$session_id,content:$content}')" >/dev/null
    if [[ -n "$lead" ]]; then
        "$CLI" msg_send --target "$lead" --session_id "$holder" --content "$handoff"
    fi
    exit "$status"
fi
