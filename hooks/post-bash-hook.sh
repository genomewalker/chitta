#!/bin/bash
# Headless bridge participant: debugging hints it never asked for.
if [[ -n "${CHITTA_HEADLESS:-$CC_SOUL_HEADLESS}" ]]; then cat >/dev/null; printf '{}'; exit 0; fi

# PostToolUse hook for Bash: Surface relevant memories on command failure
#
# When a command fails, searches for gotchas/corrections related to the command
# and injects them as context for debugging.

# Don't use set -e: we want hooks to succeed even if some parts fail

CHITTA_BIN="${CHITTA_BIN:-$HOME/.claude/bin/chitta}"
MIND_PATH="${MIND_PATH:-$HOME/.claude/mind}"
LAST_CMD_FILE="$MIND_PATH/.last_bash_cmd"

[[ ! -x "$CHITTA_BIN" ]] && exit 0

# Source shared library for fire-and-forget queue_write — never block on daemon RPC
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${SCRIPT_DIR}/lib.sh" 2>/dev/null || true

# Read stdin (PostToolUse gets tool result)
STDIN_DATA=$(cat)
exec </dev/null  # stdin consumed; children must not inherit the still-open hook pipe (chitta CLI blocks on it)
# Debug: `touch $MIND_PATH/.dump_bash_payload` to append raw payloads to
# $MIND_PATH/bash_payload_dump.jsonl (schema drift diagnosis). Fail-open.
[[ -f "$MIND_PATH/.dump_bash_payload" ]] && printf '%s\n' "$STDIN_DATA" >> "$MIND_PATH/bash_payload_dump.jsonl" 2>/dev/null || true

# Extract exit code and command. Two payload shapes (captured 2026-09-11):
#   PostToolUse         — success only; result under .tool_response (older
#                         builds: .tool_result); Claude Code never delivers a
#                         failed Bash call to this event.
#   PostToolUseFailure  — .error = "Exit code N\n<stderr>", no tool_response.
# Until the failure event was registered, this hook only ever saw exit 0: the
# ledger recorded zero failures in 10 days and the gotcha-on-failure lane below
# never fired.
command=$(echo "$STDIN_DATA" | jq -r '.tool_input.command // empty')
_hook_event=$(echo "$STDIN_DATA" | jq -r '.hook_event_name // "PostToolUse"')
if [[ "$_hook_event" == "PostToolUseFailure" ]]; then
    _err=$(echo "$STDIN_DATA" | jq -r '.error // ""')
    exit_code=$(printf '%s' "$_err" | grep -oE '^Exit code [0-9]+' | grep -oE '[0-9]+$')
    exit_code="${exit_code:-1}"
    output=""
    stderr=$(printf '%s' "$_err" | tail -n +2 | head -c 500)
elif echo "$STDIN_DATA" | jq -e '.tool_response | type == "string"' >/dev/null 2>&1; then
    # Codex shape (captured 2026-09-13): PostToolUse fires for failures too, but
    # tool_response is the aggregated output STRING with no exit code. Recording
    # 0 here would book every Codex failure as a success (it did, until today).
    # exit_code stays "null" (unknown); a text heuristic flags likely failures.
    output=$(echo "$STDIN_DATA" | jq -r '.tool_response' | head -c 500)
    stderr=""
    exit_code=0          # downstream branches keep their pre-existing behavior
    exit_known=0         # ...but the ledger must not claim success
    if printf '%s' "$output" | grep -qiE 'No such file|command not found|Permission denied|Traceback \(most recent|^Error[: ]|FAILED|fatal:|Exit code [1-9]'; then
        likely_fail=1
    fi
else
    exit_code=$(echo "$STDIN_DATA" | jq -r '(.tool_response // .tool_result // {}) | .exit_code // 0')
    output=$(echo "$STDIN_DATA" | jq -r '(.tool_response // .tool_result // {}) | .stdout // empty' | head -c 500)
    stderr=$(echo "$STDIN_DATA" | jq -r '(.tool_response // .tool_result // {}) | .stderr // empty' | head -c 500)
fi

# Outcome ledger tap (additive, Phase 1): every bash exit code, for the
# injected-memory credit join. Fail-open — never blocks the hook.
if [[ -n "$command" && -f "${SCRIPT_DIR}/outcome-ledger.sh" ]]; then
    source "${SCRIPT_DIR}/outcome-ledger.sh" 2>/dev/null
    _lg_sid=$(echo "$STDIN_DATA" | jq -r '.session_id // "unknown"' 2>/dev/null)
    _lg_cmd=$(printf '%s' "${command:0:80}" | jq -Rs . 2>/dev/null)
    _lg_exit="${exit_code:-0}"; [[ "${exit_known:-1}" == "0" ]] && _lg_exit="null"
    _lg_extra=""; [[ "${likely_fail:-0}" == "1" ]] && _lg_extra=',"likely_fail":true'
    _lg_stderr='""'
    if [[ "${exit_code:-0}" != "0" || "${likely_fail:-0}" == "1" ]]; then
        _lg_stderr=$(printf '%s' "${stderr:-${_err:-$output}}" | head -c 160 | jq -Rs . 2>/dev/null)
    fi
    ledger_append "{\"stderr_head\":${_lg_stderr:-\"\"},\"event\":\"bash_outcome\",\"exit_code\":${_lg_exit},\"cmd_head\":${_lg_cmd:-\"\"}${_lg_extra}}" "$_lg_sid" 2>/dev/null || true
fi

# Normalize command to first word (basename only)
normalize_cmd() {
    echo "$1" | awk '{print $1}' | sed 's|.*/||'
}

# Detect long-running job launches and store operational state
is_long_running_launch() {
    local cmd="$1"
    echo "$cmd" | grep -qE \
        '(^|\s)(nohup|sbatch|srun|qsub|bsub)\s' \
        || echo "$cmd" | grep -qE '(parallel\s.*--sshlogin|parallel\s.*-S\s|gnu parallel)' \
        || echo "$cmd" | grep -qE '(screen\s+-dm|tmux\s+new(-session)?\s+-d)' \
        || { echo "$cmd" | grep -qE '\s*&\s*$' && echo "$cmd" | grep -qvE '^\s*(sleep|echo|true|false|wait)\s'; }
}

extract_job_paths() {
    local cmd="$1"
    local paths=""
    # --out_dir / --output / -o / --outdir
    local outdir
    outdir=$(echo "$cmd" | grep -oE '(--(out_dir|output|outdir|out)\s+\S+|-o\s+\S+)' | grep -oE '\S+$' | head -1)
    [[ -n "$outdir" ]] && paths+="out:$outdir "
    # --log / --joblog / > file redirection
    local logfile
    logfile=$(echo "$cmd" | grep -oE '(--joblog\s+\S+|--log\s+\S+)' | grep -oE '\S+$' | head -1)
    [[ -z "$logfile" ]] && logfile=$(echo "$cmd" | grep -oE '>\s*\S+\.log' | grep -oE '\S+\.log' | head -1)
    [[ -n "$logfile" ]] && paths+="log:$logfile "
    echo "$paths"
}

# Detect analysis scripts worth tracking (python/R/shell/snakemake/nextflow/etc.)
is_analysis_script() {
    local cmd="$1"
    echo "$cmd" | grep -qE \
        '(^|\s)(python3?|Rscript|julia|perl|snakemake|nextflow)\s+\S+\.(py|R|jl|pl|nf|smk)' \
        || echo "$cmd" | grep -qE '(^|\s)bash\s+\S+\.sh(\s|$)' \
        || echo "$cmd" | grep -qE '(^|\s)\./\S+\.(sh|py|R)(\s|$)' \
        || echo "$cmd" | grep -qE '(^|\s)(snakemake|nextflow)\s' \
        || { echo "$cmd" | grep -qE "(^|\s)(python3?|Rscript)\s" \
             && echo "$cmd" | grep -qvE '^\s*(pip|conda|which|python.*--version)'; }
}

if [[ "$exit_code" == "0" && -n "$command" ]]; then
    curr_cmd=$(normalize_cmd "$command")

    # Long-running launch: store raw command + extracted paths as a signal
    # so other sessions can find it via recall. Claude adds semantic layer separately.
    if is_long_running_launch "$command"; then
        job_paths=$(extract_job_paths "$command")
        short_cmd=$(echo "$command" | head -c 300)
        # Determine realm from CWD
        cwd=$(pwd 2>/dev/null || echo "")
        realm=""
        if [[ "$cwd" == *"/repos/"* ]]; then
            realm=$(echo "$cwd" | grep -oE '/repos/[^/]+' | sed 's|/repos/||')
        fi
        content="[job:auto] cmd: $short_cmd | $job_paths| cwd: $cwd"
        content_json=$(printf '%s' "$content" | jq -Rs .)
        realm_json=$(printf '%s' "$realm" | jq -Rs .)
        queue_write "remember" "{\"content\":$content_json,\"kind\":\"signal\",\"realm\":$realm_json,\"tags\":[\"long-running-job\"],\"visibility\":1}" 2>/dev/null || true
    fi

    # ── Task ledger: provenance extraction ──────────────────────────────────
    _PLUGIN_DIR="${CHITTA_PLUGIN_DIR:-${CC_SOUL_PLUGIN_DIR:-$(dirname "$(dirname "$(realpath "${BASH_SOURCE[0]}")")")}}"
    _MCP_DIR="$_PLUGIN_DIR/chitta-mcp"
    if is_long_running_launch "$command" || is_analysis_script "$command"; then
        _task_id=$(cat "$MIND_PATH/.pending_task_id" 2>/dev/null || echo "")
        [[ -n "$_task_id" ]] && rm -f "$MIND_PATH/.pending_task_id"
        _pb_sid=$(echo "$STDIN_DATA" | jq -r '.session_id // empty' 2>/dev/null)
        _thread_id=$(cat "$MIND_PATH/.current_thread_${_pb_sid:-none}" 2>/dev/null \
            || cat "$MIND_PATH/.current_thread_id" 2>/dev/null || echo "")
        _snap_flag=""
        [[ -n "$_task_id" && -f "$MIND_PATH/.fs_snapshot_${_task_id}" ]] \
            && _snap_flag="--before-snapshot $MIND_PATH/.fs_snapshot_${_task_id}"
        _prov_json=$(echo "$STDIN_DATA" | timeout 3 python3 "$_MCP_DIR/provenance.py" extract \
            --cmd "$command" --cwd "${cwd:-$(pwd)}" --stdout-from-stdin \
            ${_snap_flag} 2>/dev/null || echo "{}")
        if [[ -n "$_task_id" ]]; then
            _prov_patch=$(echo "$_prov_json" | python3 -c "
import sys,json; d=json.load(sys.stdin)
keys='cmd cwd git_branch git_commit conda_env inputs outputs params config_files references job_id scheduler status_check_cmd completion_digest'.split()
print(json.dumps({k:d[k] for k in keys if k in d}))" 2>/dev/null || echo "{}")
            queue_write "long_task_update" "{\"task_id\":\"$_task_id\",\"payload_patch\":$_prov_patch}" 2>/dev/null || true
            while IFS= read -r _path; do
                [[ -z "$_path" ]] && continue
                timeout 2 python3 "$_MCP_DIR/task_ledger.py" artifact_register \
                    --task-id "$_task_id" --path "$_path" --thread-id "$_thread_id" 2>/dev/null || true
            done < <(echo "$_prov_json" | python3 -c "
import sys,json; d=json.load(sys.stdin)
[print(p) for p in (d.get('outputs') or [])+(d.get('new_files') or [])]" 2>/dev/null)
            rm -f "$MIND_PATH/.fs_snapshot_${_task_id}"
        fi
    fi
    # ── End task ledger ──────────────────────────────────────────────────────

    # ── Auto-store milestone: significant state-changing events ──────────────
    # Detects git commit, successful build, test pass, SLURM submit —
    # stores a category:milestone memory so sessions never silently lose state.
    _milestone_title=""
    _milestone_content=""
    _combined_out="${output}${stderr}"

    # Commits are usually chained (`git add … && git commit …`), so match at
    # any command boundary, not only at the start of the line.
    if echo "$command" | grep -qE '(^|&&|;|\|\|)\s*git\s+(-C\s+\S+\s+)?commit(\s|$)'; then
        _msg=$(echo "$command" | grep -oE -- "-m\s+['\"]?[^'\"]+['\"]?" | sed "s/-m\s*['\"]//;s/['\"]$//" | head -c 120)
        [[ -z "$_msg" ]] && _msg=$(echo "$output" | grep -oE '\[.+\]' | head -1)
        _milestone_title="git commit: ${_msg:-committed}"
        _branch=$(git rev-parse --abbrev-ref HEAD 2>/dev/null || echo "")
        _hash=$(git rev-parse --short HEAD 2>/dev/null || echo "")
        _milestone_content="branch:$_branch commit:$_hash cmd:${command:0:200}"

    elif echo "$command" | grep -qE '(cargo\s+(build|test|install)|cmake\s+--build|make\b)' \
         && ! echo "$_combined_out" | grep -qiE '(error\[E[0-9]|undefined reference|FAILED|ld: )'; then
        _proj=$(basename "$(pwd)")
        if echo "$command" | grep -q "test"; then
            _passed=$(echo "$_combined_out" | grep -oE '[0-9]+ passed' | tail -1)
            _milestone_title="tests passed: $_proj ${_passed}"
        else
            _milestone_title="build succeeded: $_proj"
        fi
        _milestone_content="cmd:${command:0:200} cwd:$(pwd)"

    elif echo "$command" | grep -qE '^\s*(sbatch|srun)\s' \
         && echo "$_combined_out" | grep -qE 'Submitted batch job [0-9]+'; then
        _job_id=$(echo "$_combined_out" | grep -oE 'Submitted batch job [0-9]+' | grep -oE '[0-9]+$')
        _milestone_title="SLURM job submitted: job_id=$_job_id"
        _milestone_content="cmd:${command:0:200} job_id:$_job_id cwd:$(pwd)"

    elif echo "$command" | grep -qE '(pytest|python.*-m\s+pytest|Rscript.*test)' \
         && echo "$_combined_out" | grep -qE '[0-9]+ passed'; then
        _passed=$(echo "$_combined_out" | grep -oE '[0-9]+ passed' | tail -1)
        _milestone_title="tests passed: $_passed"
        _milestone_content="cmd:${command:0:200} cwd:$(pwd)"
    fi

    if [[ -n "$_milestone_title" ]]; then
        _realm=""
        _cwd=$(pwd 2>/dev/null || echo "")
        [[ "$_cwd" == *"/repos/"* ]] && _realm=$(echo "$_cwd" | grep -oE '/repos/[^/]+' | sed 's|/repos/||')
        _title_json=$(printf '%s' "$_milestone_title" | jq -Rs .)
        _content_json=$(printf '%s' "$_milestone_content" | jq -Rs .)
        _realm_json=$(printf '%s' "$_realm" | jq -Rs .)
        queue_write "remember" \
            "{\"title\":$_title_json,\"content\":$_content_json,\"category\":\"milestone\",\"realm\":$_realm_json,\"tags\":[\"auto-milestone\"],\"visibility\":1}" \
            2>/dev/null || true
        # Signal to prompt-core.sh that storage happened this turn
        echo "$(date +%s)" > "$MIND_PATH/.last_auto_store_ts" 2>/dev/null || true
    fi
    # ── End milestone auto-store ──────────────────────────────────────────────

    # Read previous command if exists
    if [[ -f "$LAST_CMD_FILE" ]]; then
        prev_cmd=$(cat "$LAST_CMD_FILE" 2>/dev/null)

        # Record habit if both commands present and different.
        # Uses queue_write (fire-and-forget file append) instead of RPC —
        # bash post-hooks fire on every command; a blocking RPC here can
        # stack up behind WAL/consolidation work and stall MCP responsiveness.
        if [[ -n "$prev_cmd" && "$prev_cmd" != "$curr_cmd" ]]; then
            trigger_json=$(printf 'bash:%s' "$prev_cmd" | jq -Rs .)
            response_json=$(printf 'bash:%s' "$curr_cmd" | jq -Rs .)
            queue_write "habit_observe" "{\"trigger\":$trigger_json,\"response\":$response_json}" 2>/dev/null || true
        fi
    fi

    # Save current command for next time
    mkdir -p "$MIND_PATH" 2>/dev/null
    echo "$curr_cmd" > "$LAST_CMD_FILE"

    exit 0
fi

[[ -z "$command" ]] && exit 0

detect_output_type() {
    local out="$1"
    local err="$2"
    local combined="$out$err"

    if echo "$combined" | grep -qE '(FAILED|PASSED|test result:|not ok|ok [0-9]+ test)'; then
        echo "TestResults"
    elif echo "$combined" | grep -qE '(error\[E[0-9]|undefined reference|ld: |make\[|CMake Error)'; then
        echo "BuildOutput"
    elif echo "$combined" | grep -qE '(^[0-9]{4}-[0-9]{2}-[0-9]{2}|\[(INFO|WARN|ERROR)\])'; then
        echo "LogOutput"
    elif echo "$out" | grep -qE '^\s*[\{\[]'; then
        echo "JsonOutput"
    else
        echo "Generic"
    fi
}

build_typed_query() {
    local cmd="$1"
    local out="$2"
    local err="$3"
    local otype="$4"

    case "$otype" in
        TestResults)
            local test_name
            test_name=$(echo "$out$err" | grep -iE '(FAILED|not ok)' | head -1 | sed 's/.*FAILED\s*//;s/.*not ok\s*[0-9]*\s*//' | head -c 100)
            local framework
            framework=$(echo "$cmd" | grep -oE '(pytest|cargo test|jest|mocha|rspec|go test|npm test)' | head -1)
            echo "test failure $test_name $framework"
            ;;
        BuildOutput)
            local error_line
            error_line=$(echo "$out$err" | grep -m1 'error' | head -c 120)
            local lang
            lang=$(echo "$cmd" | grep -oE '(cargo|cmake|make|gcc|g\+\+|rustc|javac|go build|npm run build)' | head -1)
            echo "build error $lang $error_line"
            ;;
        LogOutput)
            local error_msg
            error_msg=$(echo "$out$err" | grep -i 'ERROR' | head -1 | sed 's/.*ERROR[]\s:]*//' | head -c 120)
            local service
            service=$(echo "$cmd" | awk '{print $1}' | sed 's|.*/||')
            echo "error $service $error_msg"
            ;;
        *)
            local q="$cmd"
            if [[ -n "$err" ]]; then
                local error_terms
                error_terms=$(echo "$err" | grep -oiE '(error|failed|not found|permission denied|no such|cannot|invalid)' | head -3 | tr '\n' ' ')
                [[ -n "$error_terms" ]] && q="$q $error_terms"
            fi
            echo "$q"
            ;;
    esac
}

output_type=$(detect_output_type "$output" "$stderr")
query=$(build_typed_query "$command" "$output" "$stderr" "$output_type")

escaped_query=$(json_escape "$query")

# Search for gotchas and corrections
memories=$(timeout 2 "$CHITTA_BIN" recall --query "$escaped_query" --limit 2 --text-only 2>/dev/null | head -c 400 || true)

if [[ -n "$memories" && "$memories" != *"No memories"* ]]; then
    escaped_mem=$(json_escape "$memories")
    echo "{\"hookSpecificOutput\":{\"hookEventName\":\"PostToolUse\",\"additionalContext\":\"🔴 COMMAND FAILED (exit $exit_code) - Related memories:\\n$escaped_mem\"}}"
fi

exit 0
