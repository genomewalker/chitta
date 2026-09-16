#!/bin/bash
# Headless bridge participant: no session to restore — it answers once and exits.
if [[ -n "${CHITTA_HEADLESS:-$CC_SOUL_HEADLESS}" ]]; then cat >/dev/null; printf '{}'; exit 0; fi

# One process-group deadline covers setup, every lane, and output assembly.
# SIGKILL bounds children that ignore TERM. Only this invocation's private group
# is affected; the supervising shell removes its local result directory.
if [[ "${1:-}" != --session-start-worker ]]; then
    HOOK_BUDGET_MS="${CHITTA_HOOK_BUDGET_MS:-${CC_SOUL_HOOK_BUDGET_MS:-6000}}"
    [[ "$HOOK_BUDGET_MS" =~ ^[1-9][0-9]*$ ]] || HOOK_BUDGET_MS=6000
    _ld=$(mktemp -d /tmp/chitta-session-start.XXXXXX) || exit 0
    trap 'rm -rf "$_ld"' EXIT
    printf -v _budget '%d.%03d' "$((HOOK_BUDGET_MS / 1000))" "$((HOOK_BUDGET_MS % 1000))"
    _trace=()
    [[ $- == *x* ]] && _trace=(-x)
    timeout --signal=KILL "$_budget" bash "${_trace[@]}" "${BASH_SOURCE[0]}" --session-start-worker "$_ld" "$PPID" > "$_ld/out"
    _rc=$?
    if [[ -s "$_ld/out" ]]; then
        cat "$_ld/out"
    elif [[ "$_rc" -ne 0 ]]; then
        # Every lane timed out (daemon busy or node saturated): say so instead of
        # printing nothing, so the model knows context was not loaded.
        printf '[chitta] session context unavailable (hook budget %s ms exceeded, daemon busy); run /recap once it responds.\n' "$HOOK_BUDGET_MS"
    fi
    exit 0
fi
_ld="$2"
_SESSION_START_PARENT_PID="$3"
# Nested timeout normally creates a new process group, escaping the outer
# deadline. Keep every lane (including registry_call's timeout) in our group.
timeout() { command timeout --foreground "$@"; }
declare -A _lane_pids=()
_launch_lane() {
    local name="$1"
    shift
    # Explicit stdin preserves the registry payload in an asynchronous shell.
    ( "$@" || true; : ) <&0 >"$_ld/$name" 2>/dev/null &
    _lane_pids["$name"]=$!
}
_read_lane() {
    local name="$1" dest="$2" value
    wait "${_lane_pids[$name]}" 2>/dev/null || true
    value=$(<"$_ld/$name")
    printf -v "$dest" '%s' "$value"
}

# SessionStart hook: Initialize soul context with FULL state restoration
#
# LOSSLESS: Restores complete session state after compaction
# - Files that were being worked on
# - Decisions made
# - Tasks and progress
# - Blockers and discoveries

# Don't use set -e: we want to continue even if some parts fail
# This is critical for post-compaction sessions where some data may be missing

CHITTA_BIN="${CHITTA_BIN:-$HOME/.claude/bin/chitta}"
MAX_WAIT="${CHITTA_MAX_WAIT:-${CC_SOUL_MAX_WAIT:-2}}"

# Source shared library (provides queue_write with ack_id, get_queue_file, etc.)
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${SCRIPT_DIR}/lib.sh"

# Ephemeral hook state; persistent policy and cross-hook shared files stay in mind.
HOOK_STATE_DIR=$(runtime_state_dir "${CHITTA_DB_PATH:-${HOME}/.claude/mind}")
mkdir -p "$HOOK_STATE_DIR" 2>/dev/null || true
PLUGIN_DIR="$(resolve_cc_soul_root 2>/dev/null || dirname "$SCRIPT_DIR")"

SOCKET_PATH=$(get_socket_path)

# Parse JSON input
# Claude Code provides: session_id, transcript_path, source (startup|resume|clear|compact)
INPUT=$(cat)
exec </dev/null  # stdin consumed; children must not inherit the still-open hook pipe (chitta CLI blocks on it)
SESSION_ID=$(echo "$INPUT" | jq -r '.session_id // empty')
TRANSCRIPT_PATH=$(echo "$INPUT" | jq -r '.transcript_path // empty')
HOOK_SOURCE=$(echo "$INPUT" | jq -r '.source // "startup"')

# Clean stale per-session sentinels — but NOT on compact (same session continues)
MIND_PATH="${CHITTA_DB_PATH:-${HOME}/.claude/mind}"
STRICT_MODE_FILE="${MIND_PATH}/.strict_claude_style"
STRICT_MODE_DEFAULT="${CHITTA_STRICT_MODE_DEFAULT:-${CC_SOUL_STRICT_MODE_DEFAULT:-1}}"
_reset_session_state() {
if [[ "$HOOK_SOURCE" != "compact" ]]; then
    rm -f "$MIND_PATH/.session_active" "$MIND_PATH/.gaps_surfaced"
    rm -f "$MIND_PATH/.stop_dedup_"* 2>/dev/null || true
    rm -f "${HOOK_STATE_DIR}/.size_warned_"* 2>/dev/null || true
    # Reset subagent counter for new session
    [[ -n "$SESSION_ID" ]] && rm -f "$MIND_PATH/.subagent_count_${SESSION_ID}" 2>/dev/null || true
fi

# Strict Claude-style mode toggle persisted per session workspace.
# Default ON; set CHITTA_STRICT_MODE_DEFAULT=0 to disable auto-enable.
mkdir -p "$MIND_PATH" 2>/dev/null || true
if [[ "$STRICT_MODE_DEFAULT" == "1" ]]; then
    printf '%s\n' "enabled $(date -u +%Y-%m-%dT%H:%M:%SZ)" > "$STRICT_MODE_FILE" 2>/dev/null || true
elif [[ "$STRICT_MODE_DEFAULT" == "0" ]]; then
    rm -f "$STRICT_MODE_FILE" 2>/dev/null || true
fi

# Initialize turn-discipline counter to current turn so the discipline nudge
# measures idle turns within THIS session, not across session boundaries.
if [[ -n "$SESSION_ID" ]]; then
    TURN_FILE="${HOOK_STATE_DIR}/.turn_index_${SESSION_ID}"
    CURRENT_TURN=$(cat "$TURN_FILE" 2>/dev/null || echo 0)
    echo "$CURRENT_TURN" > "${HOOK_STATE_DIR}/.last_store_turn_${SESSION_ID}"
fi

}
_launch_lane cleanup _reset_session_state

# Check chitta CLI exists and daemon is running
if [[ ! -x "$CHITTA_BIN" ]]; then
    # Compatibility bootstrap only; normal sessions never load the adapter.
    [[ -z "$SESSION_ID" ]] || registry_call 8 register --client claude <<< "$INPUT"
    wait "${_lane_pids[cleanup]}" 2>/dev/null || true
    exit 0
fi
if ! daemon_available; then
    wait "${_lane_pids[cleanup]}" 2>/dev/null || true
    exit 0
fi

# Detect subagent session: SubagentStart hook writes sentinel before session starts
SENTINEL="${MIND_PATH}/.pending_subagent_start"
IS_SUBAGENT=false
if [[ "$HOOK_SOURCE" == "startup" && -f "$SENTINEL" ]]; then
    age=$(($(date +%s) - $(stat -c %Y "$SENTINEL" 2>/dev/null || echo 0)))
    if [[ "$age" -lt 30 ]]; then
        IS_SUBAGENT=true
    fi
    rm -f "$SENTINEL"
fi

# Start realm-independent reads before path decoding and realm/ledger lookup.
if [[ "$IS_SUBAGENT" != "true" ]]; then
_launch_lane soul timeout "$MAX_WAIT" "$CHITTA_BIN" soul_context
_launch_lane themes timeout "$MAX_WAIT" "$CHITTA_BIN" sql_query \
    --query "SELECT t.memory_count, substr(m.content, 1, 60) as label FROM theme t JOIN memory m ON t.representative_id = m.id WHERE t.memory_count > 0 ORDER BY t.updated_at DESC LIMIT 3" --json
_launch_lane kinds timeout "$MAX_WAIT" "$CHITTA_BIN" sql_query \
    --query "SELECT kind, COUNT(*) as cnt FROM memory WHERE access_count > 1 GROUP BY kind ORDER BY cnt DESC LIMIT 6" --json
_launch_lane counts timeout "$MAX_WAIT" "$CHITTA_BIN" sql_query \
    --query "SELECT COUNT(*) as total, COUNT(CASE WHEN priority_tier = 2 THEN 1 END) as critical, COUNT(CASE WHEN pinned = true THEN 1 END) as pinned FROM memory" --json
_launch_lane corrections_raw timeout "$MAX_WAIT" "$CHITTA_BIN" recall --query correction --tag correction --limit 5 --json
_launch_lane compliance timeout "$MAX_WAIT" "$CHITTA_BIN" recall --query "compliance:auto user correction" --limit 2 --text-only
_launch_lane probe timeout 1 "$CHITTA_BIN" query_triplets --predicate probe_signal --limit 10
_launch_lane cache timeout "$MAX_WAIT" "$CHITTA_BIN" recall --query "cache:break session cache_hit_ratio" --limit 1 --text-only

fi

# Derive project directory from transcript path
# Transcript path: ~/.claude/projects/-maps-projects-X-Y-Z/session.jsonl
# Encoded path uses dashes, but dir names can have hyphens too (e.g., cc-soul)
PROJECT_DIR=$(jq -r '.cwd // .project_dir // empty' <<< "$INPUT")
if [[ -z "$PROJECT_DIR" && -n "$TRANSCRIPT_PATH" ]]; then
    PROJECT_ENCODED=$(dirname "$TRANSCRIPT_PATH" | xargs basename)
    PROJECT_DIR=$(decode_project_path "$PROJECT_ENCODED")
fi

# Detect realm from project directory
REALM=$(detect_project_realm "$PROJECT_DIR")

# BEGIN handoff capsule
_load_handoff_capsule() {
    local project branch args tid response
    [[ -d "$PROJECT_DIR" ]] || return 0
    project=$(cd "$PROJECT_DIR" && pwd -P) || return 0
    branch=$(git -C "$project" symbolic-ref --quiet --short HEAD 2>/dev/null ||
        git -C "$project" rev-parse --short HEAD 2>/dev/null || true)
    tid=$(jq -r '.thread_id // empty' <<< "$INPUT")
    args=$(jq -nc --arg project "$project" --arg tid "$tid" \
        '{project_dir:$project,limit:100} + (if $tid == "" then {} else {thread_id:$tid} end)')
    if [[ "${CHITTA_LEDGER_POLICY:-1}" == 1 ]]; then
        response=$(timeout 0.15 "$CHITTA_BIN" ledger_op --op hook_handoff_context \
            --args "$(jq -c --arg branch "$branch" '. + {branch:$branch}' <<< "$args")" --json 2>/dev/null) || response='{}'
        if jq -se 'length==1 and (.[0].value.text|type)=="string"' <<< "$response" >/dev/null 2>&1; then
            jq -r '.value.text' <<< "$response"
            return 0
        fi
    fi
    timeout "$MAX_WAIT" "$CHITTA_BIN" ledger_op --op session_list --args "$args" --json |
        jq -r --arg branch "$branch" --arg project "$project" '
        [.value.rows[]? | (.metadata_json | fromjson? // {}) | .handoff // empty |
         select(.version == 1 and .project_dir == $project and .branch == $branch)] |
        sort_by(.saved_at) | last |
        select(.verified == true and (.next_action | type == "string" and length > 0)) |
        "[handoff]",
        "Next action: \(.next_action)",
        "Branch: \(.branch)",
        "Artifacts: \((.artifact_paths // []) | join(", "))",
        "Blocker: \(if .blocker == "" then "none recorded" else .blocker end)",
        "Source: \(.source.kind) (session \(.source.session_id))",
        "[/handoff]"'
}
if [[ "$IS_SUBAGENT" != true ]]; then
    _launch_lane handoff _load_handoff_capsule
fi
# END handoff capsule

# Native registration owns the session binding. Read an existing thread on
# resume and claim its lease through ledger_op, preserving the adapter contract.
_register_session() {
    local thread_id metadata args registered=0
    thread_id=$(jq -r '.thread_id // empty' <<< "$INPUT")
    if [[ -z "$thread_id" ]]; then
        args=$(jq -nc --arg sid "$SESSION_ID" '{session_id:$sid}')
        thread_id=$(timeout "$MAX_WAIT" "$CHITTA_BIN" ledger_op --op session_get \
            --args "$args" --json | jq -r '.value.thread_id // empty')
    fi
    metadata=$(jq -c --arg tid "$thread_id" --arg host "${HOSTNAME:-}" \
        --arg model "${CHITTA_MODEL:-${CC_SOUL_MODEL:-}}" '
        {client:"claude", model:(.model // $model), host:$host, thread_id:$tid,
         hook_source:(.source // .hook_event_name // "")}' <<< "$INPUT")
    if timeout "$MAX_WAIT" "$CHITTA_BIN" session_register --session_id "$SESSION_ID" \
        --realm "$REALM" --pid "$_SESSION_START_PARENT_PID" \
        --project_dir "${PROJECT_DIR:-$PWD}" --transcript_path "$TRANSCRIPT_PATH" \
        --metadata "$metadata" >/dev/null; then
        registered=1
    fi
    if [[ "$registered" == 1 && -n "$thread_id" ]]; then
        args=$(jq -nc --arg sid "$SESSION_ID" --arg tid "$thread_id" \
            '{session_id:$sid,thread_id:$tid}')
        timeout "$MAX_WAIT" "$CHITTA_BIN" ledger_op --op lease_claim --args "$args" >/dev/null
    fi
    if [[ -n "$TRANSCRIPT_PATH" ]]; then
        timeout "$MAX_WAIT" "$CHITTA_BIN" transcript_register --session_id "$SESSION_ID" \
            --transcript_path "$TRANSCRIPT_PATH" --realm "$REALM" >/dev/null || {
            args=$(jq -nc --arg sid "$SESSION_ID" --arg path "$TRANSCRIPT_PATH" --arg realm "$REALM" \
                '{session_id:$sid,transcript_path:$path,realm:$realm}')
            queue_write transcript_register "$args"
        }
    fi
}
if [[ -n "$SESSION_ID" ]]; then
    _launch_lane registry _register_session
fi
if [[ "$IS_SUBAGENT" == "true" ]]; then
    # SubagentStart already injected context; registration still runs.
    wait "${_lane_pids[@]}" 2>/dev/null || true
    exit 0
fi

# ═══════════════════════════════════════════════════════════════════════════
# Background maintenance: auto-index and realm-retag
# ═══════════════════════════════════════════════════════════════════════════

_load_ledger() {
    local args response
    if [[ "${CHITTA_LEDGER_POLICY:-1}" == 1 ]]; then
        args=$(jq -nc --arg project "$REALM" --arg source "$HOOK_SOURCE" \
            --argjson now "$(date +%s)" '{project:$project,source:$source,now:$now}')
        response=$(timeout 0.15 "$CHITTA_BIN" ledger_op --op hook_session_context \
            --args "$args" --json 2>/dev/null) || response='{}'
        if jq -se 'length==1 and (.[0] | (.value.ledger|type)=="object" and (.value.card|type)=="string" and
            (.value.post_compact|type)=="boolean" and (.value.post_clear|type)=="boolean")' \
            <<< "$response" >/dev/null 2>&1; then
            printf '%s\n' "$response"
            return 0
        fi
    fi
    timeout "$MAX_WAIT" "$CHITTA_BIN" ledger_load --project "$REALM" --json || echo '{}'
}
_launch_lane ledger _load_ledger
if [[ -n "${REALM:-}" && "$REALM" != brahman ]]; then
    _launch_lane recent timeout "$MAX_WAIT" "$CHITTA_BIN" sql_query \
        --query "SELECT id, kind, content FROM memory WHERE realm = '${REALM}' ORDER BY accessed_at DESC LIMIT 3" --json
fi

_render_tasks() {
    local inbox_args thread_args inbox_pid thread_pid response
    if [[ "${CHITTA_LEDGER_POLICY:-1}" == 1 ]]; then
        response=$(timeout 0.15 "$CHITTA_BIN" ledger_op --op hook_task_context \
            --args "$(jq -nc --arg realm "$REALM" '{realm:$realm}')" --json 2>/dev/null) || response='{}'
        if jq -se 'length==1 and (.[0].value.text|type)=="string"' <<< "$response" >/dev/null 2>&1; then
            jq -r '.value.text' <<< "$response"
            return 0
        fi
    fi
    inbox_args=$(jq -nc --arg realm "$REALM" '{target_realm:$realm,state:"pending",limit:5}')
    thread_args=$(jq -nc --arg realm "$REALM" '{realm:$realm,status:"active",limit:3}')
    # ledger_op exposes separate list operations. Dispatch concurrently, then
    # format both replies with one jq process in the original card order.
    (timeout "$MAX_WAIT" "$CHITTA_BIN" ledger_op --op inbox_list --args "$inbox_args" --json |
        jq -c '.value.rows // []') >"$_ld/inbox.json" &
    inbox_pid=$!
    (timeout "$MAX_WAIT" "$CHITTA_BIN" ledger_op --op thread_list --args "$thread_args" --json |
        jq -c '.value.rows // []') >"$_ld/threads.json" &
    thread_pid=$!
    wait "$inbox_pid" 2>/dev/null || true
    wait "$thread_pid" 2>/dev/null || true
    jq -nr --arg realm "$REALM" --slurpfile inbox "$_ld/inbox.json" \
        --slurpfile threads "$_ld/threads.json" '
        ($inbox[0] // []) as $items | ($threads[0] // []) as $active |
        (if ($items | length) > 0 then
            ["━━━ inbox (\(if $realm == "" then "all" else $realm end)) ━━━",
             ($items[:5][] | (if .event_type == "completed" then "✓"
                elif (.event_type == "failed" or .event_type == "failure") then "✗"
                else "•" end) + " " + (.digest // "")[:110])] |
            "\n" + (join("\n") | sub("\n+$"; ""))
         else empty end),
        (if ($active | length) > 0 then
            ["━━━ active threads ━━━",
             ($active[:3][] | "  ⟳  \(.title // "?") [\((.thread_id // "")[:8])]")] |
            "\n" + (join("\n") | sub("\n+$"; ""))
         else empty end)'
}
_launch_lane tasks _render_tasks

# Auto-index codebase (10-minute rate limit built into script)
if [[ -n "$PROJECT_DIR" && -d "$PROJECT_DIR" ]]; then
    AUTO_INDEX_SCRIPT="$PLUGIN_DIR/scripts/auto-index.sh"
    if [[ -x "$AUTO_INDEX_SCRIPT" ]]; then
        (cd "$PROJECT_DIR" && "$AUTO_INDEX_SCRIPT") </dev/null >/dev/null 2>&1 &
        disown
    fi
fi

# Realm retag (daily rate limit built into script)
REALM_RETAG_SCRIPT="$PLUGIN_DIR/scripts/realm-retag.sh"
if [[ -x "$REALM_RETAG_SCRIPT" ]]; then
    "$REALM_RETAG_SCRIPT" </dev/null >/dev/null 2>&1 &
    disown
fi

# Trigger transcript distillation once the shared registry has recorded it.
if [[ -n "$TRANSCRIPT_PATH" && -f "$TRANSCRIPT_PATH" ]]; then
    # Trigger distillation of any pending un-distilled transcripts (handles post-compaction case)
    _read_lane registry _registry_unused
    queue_write "distill_trigger" "{\"session_id\":\"$SESSION_ID\"}"
fi

# Archive orphaned distillation staging files (one-time cleanup)
STAGING_DIR="$HOME/.claude/mind/.distill_staging"
if [[ -d "$STAGING_DIR" ]] && ls "$STAGING_DIR"/*.json >/dev/null 2>&1; then
    mkdir -p "$STAGING_DIR/archive"
    mv "$STAGING_DIR"/*.json "$STAGING_DIR/archive/" 2>/dev/null || true
fi

# Export session environment variables for other processes. Registration itself
# is handled above by the daemon-owned session and task ledger operations.
if [[ -n "$SESSION_ID" ]]; then
    CLAUDE_PID=$_SESSION_START_PARENT_PID
    SESSION_ENV_FILE="$HOME/.claude/mind/.session_env_$$"
    mkdir -p "$(dirname "$SESSION_ENV_FILE")"
    cat > "$SESSION_ENV_FILE" << EOF
export CLAUDE_SESSION_ID="$SESSION_ID"
export CLAUDE_TRANSCRIPT_PATH="$TRANSCRIPT_PATH"
export CLAUDE_REALM="$REALM"
export CLAUDE_PID="$CLAUDE_PID"
EOF
    chmod 600 "$SESSION_ENV_FILE"
fi

# Retry failed observations from previous sessions
FAILED_OBS_FILE="$HOME/.claude/mind/.failed_observations.jsonl"
if [[ -f "$FAILED_OBS_FILE" && -s "$FAILED_OBS_FILE" ]]; then
    TEMP_FAILED=$(mktemp)
    while IFS= read -r line; do
        [[ -z "$line" ]] && continue
        category=$(echo "$line" | jq -r '.category // "general"')
        content=$(echo "$line" | jq -r '.content // empty')
        if [[ -n "$content" ]]; then
            if timeout "$MAX_WAIT" "$CHITTA_BIN" observe --category "$category" --content "$content" >/dev/null 2>&1; then
                : # Success, don't add to temp file
            else
                echo "$line" >> "$TEMP_FAILED"
            fi
        fi
    done < "$FAILED_OBS_FILE"
    # Replace original with remaining failures (or remove if empty)
    if [[ -s "$TEMP_FAILED" ]]; then
        mv "$TEMP_FAILED" "$FAILED_OBS_FILE"
    else
        rm -f "$FAILED_OBS_FILE" "$TEMP_FAILED"
    fi
fi

# ═══════════════════════════════════════════════════════════════════════════
# Load and inject session state
# ═══════════════════════════════════════════════════════════════════════════

# Render the verified capsule before all other session context.
_read_lane handoff HANDOFF_CAPSULE
[[ -n "$HANDOFF_CAPSULE" ]] && printf '%s\n' "$HANDOFF_CAPSULE"

# Get full ledger entry (not just summary)
# ledger_load returns the most recent entry for the project
_read_lane ledger LEDGER_JSON
_LEDGER_POLICY=0
_LEDGER_CONTEXT_JSON="$LEDGER_JSON"
if jq -se 'length==1 and (.[0] | (.value.ledger|type)=="object" and (.value.card|type)=="string")' \
    <<< "$LEDGER_JSON" >/dev/null 2>&1; then
    LEDGER_JSON=$(jq -c '.value.ledger' <<< "$LEDGER_JSON")
    _LEDGER_POLICY=1
    if [[ -n "${CHITTA_LEDGER_PROFILE:-}" ]]; then
        printf '%s\n' "$_LEDGER_CONTEXT_JSON" > "$CHITTA_LEDGER_PROFILE"
    fi
fi

# The query depends on the ledger. Speculate the fallback concurrently, but
# display it only when the scoped answer contains no usable memory.
if [[ -n "${REALM:-}" && "$REALM" != brahman ]]; then
    _recall_query="$REALM"
    if [[ -n "$LEDGER_JSON" && "$LEDGER_JSON" != '{}' ]]; then
        _snap=$(echo "$LEDGER_JSON" | jq -r '.snapshot // empty' | head -1 | head -c 120)
        [[ -n "$_snap" ]] && _recall_query="$_snap"
    fi
    _project_kw=$(basename "${PROJECT_DIR:-$REALM}")
    _launch_lane scoped timeout "$MAX_WAIT" "$CHITTA_BIN" recall --query "$_recall_query" --realm "$REALM" --limit 6 --text-only
    _launch_lane fallback timeout "$MAX_WAIT" "$CHITTA_BIN" recall --query "${_project_kw} ${_recall_query}" --limit 6 --text-only
fi

# Check if this is a post-compaction session
# Primary signal: Claude Code passes source="compact" in hook input (authoritative)
# Fallback: ledger mood="pre-compact" (requires daemon to have saved checkpoint before compact)
if [[ "$_LEDGER_POLICY" == 1 ]]; then
    IFS=$'\t' read -r IS_POST_COMPACT IS_POST_CLEAR < <(
        jq -r '[.value.post_compact,.value.post_clear]|@tsv' <<< "$_LEDGER_CONTEXT_JSON")
else
MOOD=$(echo "$LEDGER_JSON" | jq -r '.mood // empty')
IS_POST_COMPACT=false
[[ "$HOOK_SOURCE" == "compact" ]] && IS_POST_COMPACT=true
[[ "$MOOD" == "pre-compact" ]] && IS_POST_COMPACT=true

# /clear: new session_id but same project — inject a bounded resume card
# so the chain of work isn't lost. Only fires when ledger is fresh (<4h).
IS_POST_CLEAR=false
if [[ "$HOOK_SOURCE" == "clear" ]]; then
    _ledger_ts=$(echo "$LEDGER_JSON" | jq -r '.updated_at // empty' 2>/dev/null)
    _now_s=$(date +%s)
    _age_s=99999
    if [[ -n "$_ledger_ts" ]]; then
        _ledger_s=$(date -d "$_ledger_ts" +%s 2>/dev/null || echo 0)
        _age_s=$(( _now_s - _ledger_s ))
    fi
    # Treat as resumable if mood is in_progress/pre-compact and age < 4h
    if [[ "$MOOD" == "in_progress" || "$MOOD" == "pre-compact" ]] && [[ "$_age_s" -lt 14400 ]]; then
        IS_POST_CLEAR=true
    fi
fi

fi

_collect_corrections() {
    local surface_file="${MIND_PATH}/.correction_surfaces"
    local corr_id corr_text tags current corrections_out="" index=0
    local -a ids=() texts=() tag_pids=()
    mkdir -p "$MIND_PATH"
    touch "$surface_file"
    # Quote JSON number tokens before decoding: jq 1.6 otherwise rounds u64
    # memory IDs. The quoted-string alternative keeps digits inside text intact.
    # NUL framing retains embedded tabs/newlines and jq slices Unicode characters.
    jq -Rjs '
        gsub("(?<quoted>\"(?:[^\"\\\\]|\\\\.)*\")|(?<number>-?[0-9]+(?:[.][0-9]+)?(?:[eE][+-]?[0-9]+)?)";
             if .quoted != null then .quoted else "\"" + .number + "\"" end) |
        fromjson | .results[]? |
        select(((.text // "") | ascii_downcase | contains("verified")) | not) |
        select((.correction_state // "emitted") != "verified" and
               (.correction_state // "emitted") != "applied") |
        ((.id // "") | tostring | gsub("\u0000"; "") | sub("\n+$"; "")) as $id |
        ((.text // "")[:120] | gsub("\u0000"; "") | sub("\n+$"; "")) as $text |
        select($id != "" and $text != "") | $id, "\u0000", $text, "\u0000"
    ' "$_ld/corrections_raw" >"$_ld/correction_records"
    while IFS= read -r -d '' corr_id && IFS= read -r -d '' corr_text; do
        ids+=("$corr_id")
        texts+=("$corr_text")
        (
            timeout 0.5 "$CHITTA_BIN" triplet_history --id "$corr_id" --predicate tagged --json |
                jq -r '[.triplets[]? | .object // ""] | join(" ")'
        ) >"$_ld/tag-$index" 2>/dev/null &
        tag_pids+=("$!")
        index=$((index + 1))
    done <"$_ld/correction_records"
    for index in "${!ids[@]}"; do
        wait "${tag_pids[$index]}" 2>/dev/null || true
        tags=$(<"$_ld/tag-$index")
        [[ "$tags" =~ wontfix|verified ]] && continue
        corr_id="${ids[$index]}"
        corr_text="${texts[$index]}"
        current=$(grep -c "^${corr_id}$" "$surface_file" 2>/dev/null | tail -1 || echo 0)
        current="${current//[^0-9]/}"
        [[ -z "$current" ]] && current=0
        [[ "$current" -ge 5 ]] && continue
        echo "$corr_id" >>"$surface_file"
        corrections_out="${corrections_out}${corr_text}\n"
    done
    if [[ -n "$corrections_out" ]]; then
        echo ""
        echo "[recent-corrections]"
        printf '%b' "$corrections_out" | head -5
        echo "[/recent-corrections]"
    fi
}

if [[ "$IS_POST_COMPACT" == "true" ]]; then
    if [[ "$_LEDGER_POLICY" == 1 ]]; then
        jq -j '.value.card' <<< "$_LEDGER_CONTEXT_JSON"
    else
    # This is a continuation after compaction - inject full state
    echo ""
    echo "[session-restored]"

    # Active files
    ACTIVE_FILES=$(echo "$LEDGER_JSON" | jq -r '.active_files // [] | .[]' 2>/dev/null)
    if [[ -n "$ACTIVE_FILES" ]]; then
        echo "Files in context:"
        echo "$ACTIVE_FILES" | while read -r f; do
            [[ -n "$f" ]] && echo "  - $f"
        done
    fi

    # Decisions made
    DECISIONS=$(echo "$LEDGER_JSON" | jq -r '.decisions // [] | .[]' 2>/dev/null)
    if [[ -n "$DECISIONS" ]]; then
        echo ""
        echo "Decisions made:"
        echo "$DECISIONS" | while read -r d; do
            [[ -n "$d" ]] && echo "  - $d"
        done
    fi

    # Pending tasks
    TODOS=$(echo "$LEDGER_JSON" | jq -r '.todos // [] | .[] | "[\(.status)] \(.content)"' 2>/dev/null)
    if [[ -n "$TODOS" ]]; then
        echo ""
        echo "Tasks:"
        echo "$TODOS" | while read -r t; do
            [[ -n "$t" ]] && echo "  $t"
        done
    fi

    # Blockers
    BLOCKERS=$(echo "$LEDGER_JSON" | jq -r '.blockers // [] | .[]' 2>/dev/null)
    if [[ -n "$BLOCKERS" ]]; then
        echo ""
        echo "Blockers:"
        echo "$BLOCKERS" | while read -r b; do
            [[ -n "$b" ]] && echo "  ! $b"
        done
    fi

    # Discoveries
    DISCOVERIES=$(echo "$LEDGER_JSON" | jq -r '.discoveries // [] | .[]' 2>/dev/null)
    if [[ -n "$DISCOVERIES" ]]; then
        echo ""
        echo "Discoveries:"
        echo "$DISCOVERIES" | while read -r d; do
            [[ -n "$d" ]] && echo "  * $d"
        done
    fi

    # Snapshot (what we were doing)
    SNAPSHOT=$(echo "$LEDGER_JSON" | jq -r '.snapshot // empty')
    if [[ -n "$SNAPSHOT" && ${#SNAPSHOT} -gt 20 ]]; then
        echo ""
        echo "Last context:"
        echo "$SNAPSHOT" | head -c 500
        echo ""
    fi

    echo "[/session-restored]"
    echo ""
    fi
elif [[ "$IS_POST_CLEAR" == "true" ]]; then
    if [[ "$_LEDGER_POLICY" == 1 ]]; then
        jq -j '.value.card' <<< "$_LEDGER_CONTEXT_JSON"
    else
    # /clear with a recent in-progress ledger — inject bounded resume hint
    _clear_goal=$(echo "$LEDGER_JSON" | jq -r '.snapshot // empty' | grep -m1 '^Goal:' | sed 's/^Goal:[[:space:]]*//' | head -c 200)
    [[ -z "$_clear_goal" ]] && _clear_goal=$(echo "$LEDGER_JSON" | jq -r '.snapshot // empty' | head -1 | head -c 200)
    _clear_next=$(echo "$LEDGER_JSON" | jq -r '.next_steps // [] | .[0] // empty' 2>/dev/null | head -c 150)
    _clear_files=$(echo "$LEDGER_JSON" | jq -r '.active_files // [] | .[:5] | join(", ")' 2>/dev/null)
    _clear_updated=$(echo "$LEDGER_JSON" | jq -r '.updated_at // empty' 2>/dev/null)

    _card="[last-session]"
    [[ -n "$_clear_goal" ]] && _card="${_card}\nPrevious task: ${_clear_goal}"
    [[ -n "$_clear_next" ]] && _card="${_card}\nNext step: ${_clear_next}"
    [[ -n "$_clear_files" ]] && _card="${_card}\nActive files: ${_clear_files}"
    [[ -n "$_clear_updated" ]] && _card="${_card}\nSaved: ${_clear_updated}"
    _card="${_card}\nRun /recap for full context. [/last-session]"
    echo -e "$_card"
    fi
else
    # Normal session start - just show minimal info
    # Dependent correction tags start as soon as their recall has completed.
    _read_lane corrections_raw corrections_raw
    _launch_lane corrections _collect_corrections
    _read_lane tasks _tasks_txt
    [[ -n "$_tasks_txt" ]] && printf '%s\n' "$_tasks_txt"

    _read_lane soul soul_output
    if [[ -n "$soul_output" ]]; then
        memories=$(echo "$soul_output" | grep -oE 'Memory: [0-9]+' | grep -oE '[0-9]+' || echo "0")
        triplets=$(echo "$soul_output" | grep -oE '[0-9]+ triplets' | grep -oE '[0-9]+' || echo "0")
        [[ "$memories" != "0" ]] && echo "[soul] m=$memories t=$triplets"
    fi

    # The daemon returns the same normal/clear/compact ledger card.
    if [[ "$_LEDGER_POLICY" == 1 ]]; then
        jq -j '.value.card' <<< "$_LEDGER_CONTEXT_JSON"
    elif [[ -n "$LEDGER_JSON" && "$LEDGER_JSON" != "{}" ]]; then
        session=$(echo "$LEDGER_JSON" | jq -r '.session_id // empty')
        mood=$(echo "$LEDGER_JSON" | jq -r '.mood // empty')
        [[ -n "$session" ]] && echo "[ledger] $session ($mood)"
    fi

    # ═══════════════════════════════════════════
    # TOPOLOGY: Structural map of memory state
    # ═══════════════════════════════════════════
    TOPOLOGY_PARTS=()

    _read_lane themes theme_out
    _read_lane kinds kind_out
    _read_lane counts count_out
    recent_out=""
    if [[ -n "${REALM:-}" && "$REALM" != brahman ]]; then
        _read_lane recent recent_out
    fi

    if [[ -n "$theme_out" ]]; then
        theme_str=$(echo "$theme_out" | jq -r '[.rows[]? | "\(.memory_count)m: \(.label)"] | join(" | ")' 2>/dev/null || true)
        [[ -n "$theme_str" ]] && TOPOLOGY_PARTS+=("Themes: $theme_str")
    fi

    if [[ -n "$kind_out" ]]; then
        kind_str=$(echo "$kind_out" | jq -r '[.rows[]? | "\(.kind):\(.cnt)"] | join(", ")' 2>/dev/null || true)
        [[ -n "$kind_str" ]] && TOPOLOGY_PARTS+=("Active: $kind_str")
    fi

    if [[ -n "$recent_out" ]]; then
        recent_str=$(echo "$recent_out" | jq -r '.rows[]? | "#\(.id) [\(.kind)] \(.content[0:80])"' 2>/dev/null || true)
        if [[ -n "$recent_str" ]]; then
            TOPOLOGY_PARTS+=("Recent (${REALM}):")
            while IFS= read -r line; do
                [[ -n "$line" ]] && TOPOLOGY_PARTS+=("  $line")
            done <<< "$recent_str"
        fi
    fi

    if [[ -n "$count_out" ]]; then
        total_mem=$(echo "$count_out" | jq -r '.rows[0]?.total // 0' 2>/dev/null || echo "0")
        critical_mem=$(echo "$count_out" | jq -r '.rows[0]?.critical // 0' 2>/dev/null || echo "0")
        pinned_mem=$(echo "$count_out" | jq -r '.rows[0]?.pinned // 0' 2>/dev/null || echo "0")
        [[ "$total_mem" != "0" ]] && TOPOLOGY_PARTS+=("Total: ${total_mem} memories (${critical_mem} critical, ${pinned_mem} pinned)")
    fi

    # Emit topology block
    if [[ ${#TOPOLOGY_PARTS[@]} -gt 0 ]]; then
        echo ""
        echo "[topology]"
        for part in "${TOPOLOGY_PARTS[@]}"; do
            echo "$part"
        done
        echo "[/topology]"
    fi

    # ===========================================
    # RECALL: Surface actual memory content for the current realm.
    # This fills the gap between topology (counts) and actionable context.
    # Uses ledger snapshot as query seed if available, falls back to realm name.
    # Runs for any non-brahman realm; content is capped to keep context lean.
    # ===========================================
    if [[ -n "${REALM:-}" && "${REALM}" != "brahman" ]]; then
        _recall_raw=""
        _read_lane scoped _recall_raw

        _recall_body=$(printf '%s\n' "$_recall_raw" | grep -E '^#[0-9]+ \[[0-9]+%\] \[[^]]+\][[:space:]]+[^[:space:]]' | grep -vE '^#[0-9]+ \[[0-9]+%\] \[episode\]')

        # Pass 2: unfiltered fallback — covers projects whose memories live under brahman
        if [[ -z "$_recall_body" ]]; then
            _read_lane fallback _recall_raw
            _recall_body=$(printf '%s\n' "$_recall_raw" | grep -E '^#[0-9]+ \[[0-9]+%\] \[[^]]+\][[:space:]]+[^[:space:]]' | grep -vE '^#[0-9]+ \[[0-9]+%\] \[episode\]')
        fi

        if [[ -n "$_recall_body" ]]; then
            echo ""
            echo "[recall:${REALM}]"
            printf '%s\n' "$_recall_body" | head -c 1200
            echo ""
            echo "[/recall:${REALM}]"
            echo "[soul] If context above is sparse for the current task, call mcp__chitta__recall or mcp__chitta__smart_context for deeper retrieval."
        fi
    fi

    # ===========================================
    # CORRECTIONS RECAP: Surface recent corrections (with suppression after N surfaces)
    # Corrections tagged 'wontfix' or 'verified' are suppressed.
    # Others are suppressed after CORRECTION_MAX_SURFACES sessions without action.
    # ===========================================
    _read_lane corrections _corrections_txt
    [[ -n "$_corrections_txt" ]] && printf '%s\n' "$_corrections_txt"

    # Check for compliance failures (missed learning opportunities)
    _read_lane compliance compliance
    compliance=$(printf '%s' "$compliance" | head -c 300)
    if [[ -n "$compliance" && "$compliance" != *"No memories"* ]]; then
        echo ""
        echo "[compliance] missed corrections"
    fi

    # ===========================================
    # BEHAVIORAL PROBE NUDGE: Surface chronic behavioral patterns from recent sessions
    # Reads probe_signal triplets stored by the stop hook over the last 5 sessions.
    # If a pattern is chronic (>=3 occurrences), inject a direct behavioral nudge.
    # ===========================================
    _probe_nudge=""
    _read_lane probe _probe_triplets
    if [[ -n "$_probe_triplets" && "$_probe_triplets" != *"No triplets"* ]]; then
        _hedge_count=$(echo "$_probe_triplets" | grep -c "hedging" || true)
        _syco_count=$(echo "$_probe_triplets" | grep -c "sycophantic" || true)
        _shallow_count=$(echo "$_probe_triplets" | grep -c "shallow" || true)

        [[ "${_hedge_count:-0}" -ge 3 ]] && _probe_nudge="${_probe_nudge}[probe] hedging×${_hedge_count} — direct, drop qualifiers\n"
        [[ "${_syco_count:-0}" -ge 3 ]] && _probe_nudge="${_probe_nudge}[probe] sycophancy×${_syco_count} — push back, accuracy>agreement\n"
        [[ "${_shallow_count:-0}" -ge 3 ]] && _probe_nudge="${_probe_nudge}[probe] shallow×${_shallow_count} — think deeper before answering\n"
    fi
    if [[ -n "$_probe_nudge" ]]; then
        echo ""
        echo -e "$_probe_nudge"
    fi

    # ===========================================
    # CACHE BREAK WARNING: Surface recent cache break detections
    # ===========================================
    _read_lane cache _sus3_cb_warn
    _sus3_cb_warn=$(printf '%s' "$_sus3_cb_warn" | head -c 400)
    if [[ -n "$_sus3_cb_warn" && "$_sus3_cb_warn" != *"No memories"* && "$_sus3_cb_warn" != *"0 memories"* ]]; then
        echo ""
        echo "⚠️ BEFORE RUNNING: [cache] Recent cache break detected:"
        echo "$_sus3_cb_warn" | head -3
    fi

    # ===========================================
    # MEMORY.MD MERGE: Import Claude Code auto-memory into chitta
    # ===========================================
    SANITIZED_PATH=$(echo "$PROJECT_DIR" | sed 's|/|-|g')
    MEMORY_FILE="$HOME/.claude/projects/$SANITIZED_PATH/memory/MEMORY.md"

    if [[ -f "$MEMORY_FILE" ]]; then
        MEMORY_CONTENT=$(cat "$MEMORY_FILE" 2>/dev/null || true)

        # Skip if empty or only contains chitta auto-synced content
        if [[ -n "$MEMORY_CONTENT" && "$MEMORY_CONTENT" != *"Chitta Soul Memories (auto-synced)"* ]] || \
           [[ "$MEMORY_CONTENT" == *"## Project Notes"* ]]; then

            # Extract user-added content (after "## Project Notes" or before "Chitta Soul")
            USER_CONTENT=""
            if [[ "$MEMORY_CONTENT" == *"## Project Notes"* ]]; then
                USER_CONTENT=$(echo "$MEMORY_CONTENT" | sed -n '/## Project Notes/,/---/p' | grep -v "^##" | grep -v "^---" | head -20)
            elif [[ "$MEMORY_CONTENT" != *"Chitta Soul"* ]]; then
                # No chitta section, entire file is user content
                USER_CONTENT=$(echo "$MEMORY_CONTENT" | grep -v "^#" | head -20)
            fi

            if [[ -n "$USER_CONTENT" && ${#USER_CONTENT} -gt 10 ]]; then
                # Import into chitta (fire-and-forget via queue)
                PROJECT_NAME=$(basename "$PROJECT_DIR")
                IMPORT_SSL="[memory:$PROJECT_NAME] Claude Code MEMORY.md import\n$USER_CONTENT"
                queue_write "remember" "{\"content\":$(echo -e "$IMPORT_SSL" | jq -Rs .),\"tags\":[\"memory-import\",\"$PROJECT_NAME\"]}"
                echo "[soul] imported MEMORY.md content" >&2
            fi
        fi
    fi
fi

# ===========================================
# MSG-NOTIFY DAEMON: Launch background message polling for this session
# ===========================================
if [[ -n "$SESSION_ID" && "$SESSION_ID" != "default" ]]; then
    NOTIFY_SCRIPT="${SCRIPT_DIR}/../scripts/msg-notify.sh"
    if [[ -f "$NOTIFY_SCRIPT" ]]; then
        # Kill any stale notify daemon for this session
        PID_FILE="${MIND_PATH}/.msg_notify_pids/${SESSION_ID}.pid"
        if [[ -f "$PID_FILE" ]]; then
            old_pid=$(cat "$PID_FILE" 2>/dev/null || true)
            if [[ -n "$old_pid" ]]; then
                kill "$old_pid" 2>/dev/null || true
            fi
            rm -f "$PID_FILE"
        fi
        # Copy script to temp to avoid NFS lock on plugin marketplace dir
        NOTIFY_TMP="/tmp/msg-notify-$$.sh"
        cp "$NOTIFY_SCRIPT" "$NOTIFY_TMP"
        chmod +x "$NOTIFY_TMP"
        bash "$NOTIFY_TMP" "$SESSION_ID" 5 </dev/null >/dev/null 2>&1 &
        disown $! 2>/dev/null || true
        rm -f "$NOTIFY_TMP"
    fi
fi

# ===========================================
# WATCH PATHS: Register key project files for FileChanged hooks
# Only emits on startup/resume (compact-restore-hook handles its own)
# ===========================================
if [[ -n "$PROJECT_DIR" && -d "$PROJECT_DIR" ]]; then
    _wp_array="["
    _wp_first=true
    for _wp_f in Snakefile Nextfile pyproject.toml Cargo.toml CMakeLists.txt package.json go.mod Makefile; do
        if [[ -f "$PROJECT_DIR/$_wp_f" ]]; then
            $_wp_first && _wp_first=false || _wp_array+=","
            _wp_array+="\"$PROJECT_DIR/$_wp_f\""
        fi
    done
    _wp_array+="]"

    # If we have watchPaths, emit them as JSON hookSpecificOutput on fd 3
    # which we'll merge at exit. For now, save to temp file.
    if [[ "$_wp_array" != "[]" ]]; then
        echo "$_wp_array" > "$MIND_PATH/.watch_paths_$$"
    fi
fi

# ===========================================
# FINAL OUTPUT: Wrap in JSON if watchPaths exist, otherwise plain text passthrough
# Plain text was already emitted to stdout above. JSON hookSpecificOutput requires
# ALL stdout to be JSON (no mixed mode). Since session-start-hook emits plain text
# throughout, we only use JSON wrapper when we have watchPaths to register.
# ===========================================
_wp_file="$MIND_PATH/.watch_paths_$$"
if [[ -f "$_wp_file" ]]; then
    _wp_json=$(cat "$_wp_file")
    rm -f "$_wp_file"
    # Note: plain text was already printed to stdout. We can't retroactively wrap it.
    # Instead, emit the watchPaths as a separate line for potential future JSON parsing.
    # The FileChanged watcher is also registered by compact-restore-hook.sh (which does use JSON).
    # For startup/resume, register watchPaths via the daemon's file_watch RPC as fallback.
    queue_write "file_watch_register" "{\"session_id\":\"$SESSION_ID\",\"paths\":$_wp_json}" 2>/dev/null || true
fi

# Reap only our lanes; never wait on detached maintenance daemons.
wait "${_lane_pids[@]}" 2>/dev/null || true
exit 0
