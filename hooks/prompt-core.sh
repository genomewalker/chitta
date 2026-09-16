#!/bin/bash
# Headless bridge participant: the room supplies its own CONTEXT block, and
# injecting global soul recall here is the bleed commit 87e915d removed one
# layer up. Stay quiet.
if [[ -n "${CHITTA_HEADLESS:-$CC_SOUL_HEADLESS}" ]]; then cat >/dev/null; printf '{}'; exit 0; fi

# UserPromptSubmit hook: Surface relevant memories + detect learning opportunities
#
# HIGH PERFORMANCE: Single call with smart routing
# - Uses structured_recall (three-lens: facts/context/temporal, falls back to smart_recall)
# - Handles temporal, aspect, entity, code, and exploratory queries
# - Minimum 30% confidence threshold
# - Detects patterns for proactive learning

# Don't use set -e: we want hooks to succeed even if some parts fail

CHITTA_BIN="${CHITTA_BIN:-$HOME/.claude/bin/chitta}"
MAX_WAIT="${CHITTA_MAX_WAIT:-${CC_SOUL_MAX_WAIT:-2}}"
MIN_CONFIDENCE=30
MIND_PATH="${CHITTA_DB_PATH:-${HOME}/.claude/mind}"
# The hyb lane always requests this many results, so a header count that HITS
# it (N == HYB_LANE_LIMIT) tells us nothing about the realm's actual size —
# only a count strictly BELOW it means the query's candidate pool was
# genuinely exhausted. The C2 small-realm relaxation (below) relies on this.
HYB_LANE_LIMIT=5

# Source shared library
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${SCRIPT_DIR}/lib.sh"

# Ephemeral hook state; persistent policy and cross-hook shared files stay in mind.
HOOK_STATE_DIR=$(runtime_state_dir "${CHITTA_DB_PATH:-${HOME}/.claude/mind}")
mkdir -p "$HOOK_STATE_DIR" 2>/dev/null || true

METRICS_FILE="${MIND_PATH}/.hook_metrics.json"
ALERT_FILE="${MIND_PATH}/.hook_alerts.log"
mkdir -p "$MIND_PATH" 2>/dev/null || true

# Global wall-clock deadline. The individual `timeout` values in this file sum
# to ~19s worst case, but settings.json gives UserPromptSubmit 8s — so without a
# deadline the hook is killed mid-flight and emits nothing at all. Optional
# enrichment is skipped once the budget is spent; memory injection and the
# correction lanes never are.
# ceiling: budget is wall-clock, not CPU; a stalled daemon still costs one
# per-call timeout after the last check. upgrade: pass a deadline to chitta.
_HOOK_T0=$(date +%s%3N)
_HOOK_BUDGET_T0=$_HOOK_T0
[[ "${CHITTA_HOOK_NOW:-}" =~ ^[1-9][0-9]{12}$ ]] && _HOOK_BUDGET_T0=$(command date +%s%3N)
HOOK_BUDGET_MS="${CHITTA_HOOK_BUDGET_MS:-${CC_SOUL_HOOK_BUDGET_MS:-6000}}"
budget_left() { (( $(command date +%s%3N) - _HOOK_BUDGET_T0 < HOOK_BUDGET_MS )); }

# Recall scheduling telemetry. EPOCHREALTIME is a Bash builtin, so lane
# boundaries add no processes to the already latency-sensitive fan-out. The
# date fallback is only for old Bash builds that do not expose EPOCHREALTIME.
declare -A _LANE_MS=() _LANE_TIMEOUT=()
_LANE_PIDS=()
_LANE_ORDER=(sem ctx hyb kw corr corrk xr)
_LANE_STAT_MARKER="__CHITTA_LANE_STAT__:"
_RECALL_TELEMETRY_ACTIVE=0
_RECALL_TELEMETRY_DONE=0
_RECALL_EMPTY=0
_RECALL_CONTEXT_EMITTED=0
_LEDGER_OUTPUT=""
_NOW_MS=0
_clock_ms() {
    if [[ "${CHITTA_HOOK_NOW:-}" =~ ^[1-9][0-9]{12}$ ]]; then
        _NOW_MS=$CHITTA_HOOK_NOW
        return
    fi
    local _stamp="${EPOCHREALTIME:-}"
    if [[ -n "$_stamp" ]]; then
        _NOW_MS="${_stamp/./}"
        _NOW_MS="${_NOW_MS:0:13}"
    else
        _NOW_MS=$(date +%s%3N)
    fi
}

_run_lane() {
    local _lane="$1" _wait="$2" _start _rc _elapsed _bad=false
    shift 2
    _clock_ms; _start=$_NOW_MS
    timeout "$_wait" "$@" >"$_ld/$_lane" 2>/dev/null
    _rc=$?
    _clock_ms; _elapsed=$(( _NOW_MS - _start ))
    [[ $_rc -eq 124 || $_rc -eq 137 || ! -s "$_ld/$_lane" ]] && _bad=true
    _LANE_MS["$_lane"]=$_elapsed
    _LANE_TIMEOUT["$_lane"]=$_bad
    return 0
}

_launch_lane() {
    local _lane="$1" _wait="$2"
    shift 2
    (
        local _start _rc _elapsed _bad=0
        _start=$_LANE_FANOUT_T0
        timeout "$_wait" "$@"
        _rc=$?
        _clock_ms; _elapsed=$(( _NOW_MS - _start ))
        [[ $_rc -eq 124 || $_rc -eq 137 || ! -s "$_ld/$_lane" ]] && _bad=1
        printf '\n%s%010d%d' "$_LANE_STAT_MARKER" "$_elapsed" "$_bad"
    ) >"$_ld/$_lane" 2>/dev/null &
    _LANE_PIDS+=("$!")
}

_wait_lanes() {
    (( ${#_LANE_PIDS[@]} )) || return 0
    wait "${_LANE_PIDS[@]}" 2>/dev/null || true
    return 0
}

_read_lane() {
    local _lane="$1" _dest="$2" _raw="" _ms="" _bad=false _cut
    [[ -f "$_ld/$_lane" ]] && _raw=$(<"$_ld/$_lane")
    if [[ "$_raw" == *"$_LANE_STAT_MARKER"* ]]; then
        [[ "${_raw: -1}" == "1" ]] && _bad=true
        _ms="${_raw: -11:10}"
        _ms=$(( 10#$_ms ))
        _cut=$(( ${#_raw} - ${#_LANE_STAT_MARKER} - 12 ))
        _raw="${_raw:0:_cut}"
        while [[ "$_raw" == *$'\n' ]]; do _raw="${_raw%$'\n'}"; done
        [[ -z "$_raw" ]] && _bad=true
        _LANE_MS["$_lane"]=$_ms
        _LANE_TIMEOUT["$_lane"]=$_bad
    fi
    printf -v "$_dest" '%s' "$_raw"
}

_render_lane_telemetry() {
    local _lane _sep="" _bad
    _LANE_TIMING_FIELD=""
    _LANE_MS_JSON="{"
    _LANE_TIMEOUT_JSON="{"
    for _lane in "${_LANE_ORDER[@]}"; do
        [[ -n "${_LANE_MS[$_lane]+set}" ]] || continue
        # Pin presentation only; retain the daemon's real timeout/degraded flag.
        [[ "${CHITTA_HOOK_NOW:-}" =~ ^[1-9][0-9]{12}$ ]] && _LANE_MS["$_lane"]=0
        _bad=""
        [[ "${_LANE_TIMEOUT[$_lane]}" == "true" ]] && _bad="!"
        _LANE_TIMING_FIELD+="${_sep}${_lane}=${_LANE_MS[$_lane]}${_bad}"
        _LANE_MS_JSON+="${_sep}\"${_lane}\":${_LANE_MS[$_lane]}"
        _LANE_TIMEOUT_JSON+="${_sep}\"${_lane}\":${_LANE_TIMEOUT[$_lane]}"
        _sep=","
    done
    _LANE_MS_JSON+="}"
    _LANE_TIMEOUT_JSON+="}"
    _clock_ms
    _HOOK_ELAPSED_MS=$(( _NOW_MS - _HOOK_T0 ))
}

_recall_telemetry_finish() {
    local _exit_status=$? _lg_evt
    [[ "$_RECALL_TELEMETRY_ACTIVE" -eq 1 && "$_RECALL_TELEMETRY_DONE" -eq 0 ]] || return "$_exit_status"
    _RECALL_TELEMETRY_DONE=1
    _render_lane_telemetry
    if [[ -f "${SCRIPT_DIR}/outcome-ledger.sh" ]]; then
        source "${SCRIPT_DIR}/outcome-ledger.sh" 2>/dev/null || true
    fi
    if ! declare -F ledger_append >/dev/null; then
        return "$_exit_status"
    fi
    if [[ "$_RECALL_CONTEXT_EMITTED" -eq 1 && -n "$_LEDGER_OUTPUT" ]]; then
        _lg_evt=$(printf '%s' "$_LEDGER_OUTPUT" | jq -Rsc \
            --argjson ms "$_LANE_MS_JSON" --argjson timeouts "$_LANE_TIMEOUT_JSON" \
            --argjson elapsed "$_HOOK_ELAPSED_MS" '
            [split("\n")[] | capture("^\\[(?<lane>\\w+)\\]#(?<id>\\d+)")] |
            reduce .[] as $m ({}; .[$m.lane] += [$m.id]) |
            . as $lanes | [ .[][] ] | unique | select(length > 0) |
            {event:"injected", ids:., lanes:$lanes, lane_ms:$ms,
             lane_timeout:$timeouts, hook_ms:$elapsed}' 2>/dev/null)
        [[ -n "$_lg_evt" ]] && ledger_append "$_lg_evt" "$SESSION_ID"
    elif [[ "$_RECALL_EMPTY" -eq 1 ]]; then
        ledger_append "{\"event\":\"recall_empty\",\"lane_ms\":${_LANE_MS_JSON},\"lane_timeout\":${_LANE_TIMEOUT_JSON},\"hook_ms\":${_HOOK_ELAPSED_MS}}" "$SESSION_ID"
    fi
    return "$_exit_status"
}

# realm_detect costs up to 1s and was called twice per prompt (checkpoint block
# + recall block). Memoize into REALM; never call inside $( ) or the cache dies
# with the subshell.
_REALM_CACHE=""
realm_detect_once() {
    # realm_detect emits a BARE realm string ("project:cc-soul"), not JSON. The old
    # parse grepped for `realm": "` (JSON) and matched nothing on every call, so REALM
    # silently fell back to "brahman" — making every recall lane global and bleeding
    # cross-project memories (aDNA, generic episodes) into scoped sessions. Strip any
    # quotes and pull the first `word:token` realm, tolerating both bare and JSON output.
    # An explicit CHITTA_REALM (headless runs, benchmarks) is authoritative:
    # skip detection and the canonical-shape grep, which would discard it.
    if [[ -z "$_REALM_CACHE" && -n "${CHITTA_REALM:-}" ]]; then
        _REALM_CACHE="$CHITTA_REALM"
    fi
    # Same rules as the CLI's realm_detect (.cc-soul-realm file, else the git
    # top-level name) but in-process: the 1 s CLI call timed out on a busy NFS
    # node and the old fallback made the whole turn global ("brahman"), which
    # is exactly the cross-realm leak seen 2026-09-14 09:57.
    if [[ -z "$_REALM_CACHE" ]]; then
        local _dir="${CWD:-$PWD}"
        if [[ -r "$_dir/.cc-soul-realm" ]]; then
            read -r _REALM_CACHE < "$_dir/.cc-soul-realm" || true
        fi
        if [[ -z "$_REALM_CACHE" ]]; then
            while [[ -n "$_dir" && "$_dir" != "/" ]]; do
                if [[ -e "$_dir/.git" ]]; then
                    _REALM_CACHE="project:${_dir##*/}"
                    # A linked worktree has a .git FILE pointing into
                    # <main>/.git/worktrees/<name>: name the realm after <main>, so
                    # Codex streams and evolve worktrees share the project's memory
                    # instead of an empty project:codex-wt-... realm (2026-09-15).
                    if [[ -f "$_dir/.git" ]]; then
                        local _gd
                        _gd=$(sed -n 's/^gitdir: //p' "$_dir/.git" 2>/dev/null)
                        if [[ "$_gd" == */.git/worktrees/* ]]; then
                            _gd="${_gd%%/.git/worktrees/*}"
                            _REALM_CACHE="project:${_gd##*/}"
                        fi
                    fi
                    break
                fi
                _dir="${_dir%/*}"
            done
        fi
        _REALM_CACHE=$(printf '%s' "$_REALM_CACHE" | grep -oE '^[a-z][a-z0-9_]*:[A-Za-z0-9_./-]+' | head -1)
        [[ -z "$_REALM_CACHE" ]] && _REALM_CACHE="brahman"
    fi
    REALM="$_REALM_CACHE"
}

# Parse input - Claude Code sends JSON with session_id and prompt (gracefully handle malformed input)
INPUT=$(cat)
exec </dev/null  # stdin consumed; children must not inherit the still-open hook pipe (chitta CLI blocks on it)
# Try to extract session_id from JSON first (most reliable source)
SESSION_ID=$(echo "$INPUT" | jq -r '.session_id // empty' 2>/dev/null || echo "")
# Fall back to registry lookup if not in JSON
[[ -z "$SESSION_ID" ]] && SESSION_ID=$(get_session_id)
if [[ -z "$SESSION_ID" ]]; then
    # Codex transcripts include rollout-<ts>-<uuid>.jsonl; use basename when present.
    tp=$(echo "$INPUT" | jq -r '.transcript_path // empty' 2>/dev/null || echo "")
    if [[ -n "$tp" ]]; then
        bn=$(basename "$tp" .jsonl 2>/dev/null || true)
        [[ -n "$bn" && "$bn" != "." ]] && SESSION_ID="$bn"
    fi
fi
[[ -z "$SESSION_ID" ]] && SESSION_ID="unknown"

# Try to extract prompt from JSON, fall back to raw input if not JSON
QUERY=$(echo "$INPUT" | jq -r '.prompt // empty' 2>/dev/null || echo "")
[[ -z "$QUERY" ]] && QUERY="$INPUT"
TRANSCRIPT_PATH=$(echo "$INPUT" | jq -r '.transcript_path // empty' 2>/dev/null || echo "")

[[ -z "$QUERY" ]] && exit 0
[[ ! -x "$CHITTA_BIN" ]] && exit 0

# Open background output files at launch so EXIT cleanup is safe even if the
# heartbeat is still running. Only continuity has an output consumer to join.
_ld=$(mktemp -d "${TMPDIR:-/tmp}/ccsoul-lanes.XXXXXX") || exit 0
trap '_recall_telemetry_finish; rm -rf "$_ld"' EXIT

# A prompt is authoritative proof that this frontend session is alive. Refresh
# both the Chitta session heartbeat and any thread lease, independent of whether
# this is a Claude or Codex adapter invocation. Rate-limited: the daemon's
# liveness TTL is 900s, so skip while the last successful heartbeat is
# still under 120s old.
if [[ "$SESSION_ID" != "unknown" ]]; then
    _HB_MARKER="${HOOK_STATE_DIR}/.hb_${SESSION_ID}"
    _HB_AGE=999999
    [[ -f "$_HB_MARKER" ]] && _HB_AGE=$(( $(date +%s) - $(stat -c %Y "$_HB_MARKER" 2>/dev/null || echo 0) ))
    if [[ "$_HB_AGE" -ge 120 ]]; then
        (
            session_heartbeat "$SESSION_ID" "$INPUT" && touch "$_HB_MARKER"
        ) >"$_ld/heartbeat" 2>/dev/null &
    fi
fi

# Strip system markup (task-notifications, system-reminders, command blocks) to get real user intent.
# When a message is purely system markup (e.g. task-notification firing UserPromptSubmit),
# skip all processing — no soul context needed.
CLEAN_QUERY=$(printf '%s\n' "$QUERY" | clean_query 2>/dev/null || printf '%s' "$QUERY")
[[ -z "$CLEAN_QUERY" ]] && exit 0

# Distinctive tokens of the cleaned turn, computed once. A turn with no content
# token ("Do all", "Status", "ok") has nothing to search for: every topic lane
# would return near-random high-similarity rows (seen 2026-09-14). Only the
# correction lanes run for such turns; a single-token turn keeps the lanes but
# raises the admission floor to the C2 KNOWN cut.
_QTOK=$(printf '%s' "$CLEAN_QUERY" | tr '[:upper:]' '[:lower:]' | grep -oE '[a-z0-9][a-z0-9_>/-]{3,}' | sort -u)
_QTOK_N=$(printf '%s\n' "$_QTOK" | grep -c . || true)
# Distinctive subset: generic English/dev words anchor nothing ("manage better
# short messages query" shares "query" with half the store). Only these tokens
# may vouch for a candidate when the C2 band says the turn is UNKNOWN.
_GENERIC_TOKENS=" perfect missing else please thanks thank great done ready working broken wrong correct fine okay again still also need want check status update issue problem think know maybe sure whats what's hello there anything everything something nothing already yet already once twice around later earlier today tomorrow yesterday tonight morning evening about after again also always another anything around because been before being better between both build called change changed changes check code come could current data does doing done each either else enough error even every everything file files find first fix from give going good great have help here high into issue just keep know last later less like line lines list little long look made make makes making manage many maybe mean message messages might more most much must need needs never next nothing only other others over please point problem project query question really right same seems short should show simple since small some something start still stuff sure take than that their them then there these they thing things think this those three through time today tried true type under unrelated until update used user uses using very want wants well were what when where whether which while will with within without work working works would write wrong your "
_QTOK_DISTINCT=""
for _tok in $_QTOK; do
    [[ "$_GENERIC_TOKENS" == *" $_tok "* ]] && continue
    if [[ ${#_tok} -lt 5 ]] && ! [[ "$_tok" =~ [0-9_/\>-] ]]; then continue; fi
    _QTOK_DISTINCT+="$_tok"$'\n'
done
_QTOK_DISTINCT=$(printf '%s' "$_QTOK_DISTINCT" | sort -u)
_QTOK_DISTINCT_N=$(printf '%s\n' "$_QTOK_DISTINCT" | grep -c . || true)
_QTOK_MIN="${CHITTA_MIN_QUERY_TOKENS:-${CC_SOUL_MIN_QUERY_TOKENS:-1}}"
# Gate on DISTINCTIVE tokens: "is it working now" has four tokens and nothing
# to anchor on; its hybrid rows were 70% [operational] notes about "working".
if [[ "${_QTOK_DISTINCT_N:-0}" -lt "$_QTOK_MIN" ]]; then
    _NO_TOPIC_LANES=1
elif [[ "${_QTOK_DISTINCT_N:-0}" -eq 1 && "$MIN_CONFIDENCE" -lt 70 ]]; then
    MIN_CONFIDENCE=70
fi

# Save cleaned message for Stop hook compliance detection (no system markup pollution)
mkdir -p "$MIND_PATH"
_prev_turn=$(cat "${HOOK_STATE_DIR}/.last_user_message" 2>/dev/null || true)
echo "$CLEAN_QUERY" > "${HOOK_STATE_DIR}/.last_user_message"

# Get turn index (locked read+increment via lib.sh)
TURN_INDEX=$(get_next_turn "$SESSION_ID")

# Store user turn in lossless conversation storage (uses lib.sh queue_write with ack_id)
safe_queue_write "store_turn" "{\"session_id\":\"$SESSION_ID\",\"role\":\"user\",\"content\":$(echo "$QUERY" | jq -Rs .),\"turn_index\":$TURN_INDEX}"

# Raw turn ingest removed: verbatim turn_user episodes flooded semantic recall
# with conversation fragments, preventing structured fact retrieval.
# Lossless storage (store_turn above) preserves the full transcript.
record_ingest_metric "true"

# Use clean query for all recall and pattern detection (strip markup from QUERY)
QUERY="$CLEAN_QUERY"

# Intent detection must see the user's literal words: the anaphora enrichment
# below is a RECALL anchor, and letting it feed the intent classifier both
# triggers on the previous turn's words and stores the previous turn's text
# inside [preference]/[belief] memories (echo-chamber contamination).
_INTENT_QUERY="$QUERY"

# Context enrichment: short/anaphoric queries ("add that", "do it", "fix this") have
# no referent — prepend the previous turn so recall has an anchor.
_word_count=$(echo "$QUERY" | wc -w)
_has_anaphor=$(echo "$QUERY" | grep -qiE '\b(that|this|it|those|them|they|he|she|the above|do it|fix it|add it)\b' && echo 1 || echo 0)
if [[ -n "$_prev_turn" && "$_prev_turn" != "$QUERY" ]] && \
   [[ "$_word_count" -lt 15 || "$_has_anaphor" == "1" ]]; then
    QUERY="${_prev_turn} ${QUERY}"
fi

# Rolling context window (step 1): recruit against the conversation THREAD, not
# only the latest message. Maintain a 2-line ring of prior user turns' distinctive
# tokens and build a recency-weighted bag (current x3, prev x2, prev-prev x1) so
# the embedding leans current but carries the thread. This drives ONLY the separate
# ctx lane below; the atomic lanes keep the literal per-turn $QUERY, because widening
# THOSE into a rolling window smears them into noise. The shared-entity shadow metric
# stays pinned to the turn's own tokens (_QTOK from CLEAN_QUERY), so enriching this
# query cannot inflate shares>0 (fable's definitional-inflation guard).
CTX_QUERY=""; _CTXTOK=""
if [[ "${CHITTA_CTX_LANE:-${CC_SOUL_CTX_LANE:-1}}" == "1" && "$SESSION_ID" != "unknown" ]]; then
    _ctx_ring="${HOOK_STATE_DIR}/.ctx_window_${SESSION_ID}"
    _cur_tok=$(printf '%s' "$CLEAN_QUERY" | tr '[:upper:]' '[:lower:]' \
               | grep -oE '[a-z0-9][a-z0-9_>/-]{3,}' | sort -u | tr '\n' ' ')
    _prev1=""; _prev2=""
    if [[ -f "$_ctx_ring" ]]; then
        mapfile -t _rl < "$_ctx_ring"
        (( ${#_rl[@]} >= 1 )) && _prev1="${_rl[${#_rl[@]}-1]}"
        (( ${#_rl[@]} >= 2 )) && _prev2="${_rl[${#_rl[@]}-2]}"
    fi
    CTX_QUERY="$_cur_tok $_cur_tok $_cur_tok $_prev1 $_prev1 $_prev2"
    _CTXTOK=$(printf '%s' "$CTX_QUERY" | tr ' ' '\n' | grep -v '^$' | sort -u)
    # push current, retain last 2 lines (gives current + 2 priors next turn)
    printf '%s\n' "$_prev1" "$_cur_tok" | grep -v '^$' > "$_ctx_ring" 2>/dev/null || true
fi

# ===========================================
# CACHE EXPIRY WARNING: Warn when >5 min idle
# ===========================================
CACHE_WARN=""
SESSION_WARN=""
LAST_STOP_FILE_SESSION="${HOOK_STATE_DIR}/.last_stop_time_${SESSION_ID}"
LAST_STOP_FILE=""

# Use session-scoped idle tracking only.
# Global fallback causes cross-session false positives after restarts.
if [[ "$SESSION_ID" != "unknown" && -f "$LAST_STOP_FILE_SESSION" ]]; then
    LAST_STOP_FILE="$LAST_STOP_FILE_SESSION"
fi

if [[ -n "$LAST_STOP_FILE" ]]; then
    LAST_STOP=$(cat "$LAST_STOP_FILE" 2>/dev/null || echo 0)
    NOW=$(date +%s)
    GAP=$(( NOW - LAST_STOP ))
    # Prompt-cache TTL is account/session dependent: it was 5 min when this
    # heuristic was written and is 60 min on the current plan, so a fixed 300 s
    # threshold fired false alarms (2026-09-13, 19 min idle). Default to 60 min;
    # override with CHITTA_CACHE_TTL_MIN (CC_SOUL_ alias honored) when the plan differs.
    _ttl_min="${CHITTA_CACHE_TTL_MIN:-${CC_SOUL_CACHE_TTL_MIN:-60}}"
    if [[ $GAP -gt $(( _ttl_min * 60 )) ]]; then
        GAP_MIN=$(( GAP / 60 ))
        CACHE_WARN="[cache-expired: ${GAP_MIN}m idle (> ${_ttl_min}m TTL) — full context re-prices at cache-write rates; run /compact or start new session with /recap]"
    fi
fi

# ===========================================
# SESSION SIZE WARNING: Warn when transcript is bloating
# ===========================================
if [[ -n "$TRANSCRIPT_PATH" && -f "$TRANSCRIPT_PATH" ]]; then
    TRANSCRIPT_SIZE=$(stat -c%s "$TRANSCRIPT_PATH" 2>/dev/null || echo 0)
    TRANSCRIPT_MB=$(( TRANSCRIPT_SIZE / 1048576 ))
    # >50MB = expensive session, warn every prompt
    if [[ $TRANSCRIPT_MB -gt 50 ]]; then
        SESSION_WARN="[context-bloat: ${TRANSCRIPT_MB}MB transcript — consider /compact or start fresh with /recap to reduce cache-write costs]"
    # >20MB = getting large, warn once
    elif [[ $TRANSCRIPT_MB -gt 20 && ! -f "${HOOK_STATE_DIR}/.size_warned_${SESSION_ID}" ]]; then
        SESSION_WARN="[context-growing: ${TRANSCRIPT_MB}MB — /compact saves cache-write tokens; /recap starts lean]"
        touch "${HOOK_STATE_DIR}/.size_warned_${SESSION_ID}" 2>/dev/null || true
    fi
fi

# Skip daemon-dependent operations immediately if daemon is not running.
# v6.0: route Retrieve event into interaction ledger via durable queue (file-based, no daemon needed)
if [[ -n "${SESSION_ID:-}" && "$SESSION_ID" != "unknown" ]]; then
    _lq=$(printf '%s' "$QUERY" | jq -Rs . 2>/dev/null || echo '""')
    queue_write "ledger_append" "{\"kind\":\"Retrieve\",\"session_id\":\"${SESSION_ID}\",\"payload\":{\"Retrieve\":{\"query\":${_lq},\"strategy\":\"prompt_hook\",\"limit\":0,\"refs\":[]}}}"
fi

# queue_write above is file-based (no daemon needed), everything below requires it.
daemon_available || exit 0

# ===========================================
# TURN-BASED CHECKPOINT: Save ledger every N turns
# ===========================================
CHECKPOINT_INTERVAL="${CHITTA_CHECKPOINT_INTERVAL:-${CC_SOUL_CHECKPOINT_INTERVAL:-10}}"
if [[ $((TURN_INDEX % CHECKPOINT_INTERVAL)) -eq 0 && $TURN_INDEX -gt 0 ]]; then
    # Detect realm for project
    realm_detect_once

    CHECKPOINT_ARGS=$(jq -n \
        --arg session_id "$SESSION_ID" \
        --arg project "$REALM" \
        --arg transcript_path "$TRANSCRIPT_PATH" \
        --arg mood "working" \
        --arg snapshot "Turn $TURN_INDEX checkpoint" \
        '{session_id: $session_id, project: $project, transcript_path: $transcript_path, mood: $mood, snapshot: $snapshot}')

    queue_write "ledger_save" "$CHECKPOINT_ARGS"
    echo "[ledger] checkpoint at turn $TURN_INDEX" >&2

    # Incremental distillation every N turns (fire-and-forget, non-blocking)
    queue_write "distill_trigger" "{\"session_id\":\"$SESSION_ID\"}"
    echo "[distill] queued incremental distillation at turn $TURN_INDEX" >&2

    # Mine thinking blocks for perception-change insights (fast C++ binary, non-blocking)
    THINKING_BIN="${HOME}/.claude/bin/chitta_thinking"
    if [[ -x "$THINKING_BIN" && -n "$TRANSCRIPT_PATH" && -f "$TRANSCRIPT_PATH" ]]; then
        "$THINKING_BIN" --transcript "$TRANSCRIPT_PATH" >/dev/null 2>&1 &
    fi
fi

# ===========================================
# REALM DETECTION: Filter recall to current project realm
# ===========================================
realm_detect_once

# CEC: log user_prompt event (fire-and-forget)
_clock_ms; _RECALL_FANOUT_T0=$_NOW_MS
timeout 0.5 "$CHITTA_BIN" log_event --tool "user_prompt" \
    --entity "$REALM" --outcome 0 --ts_ms "$_RECALL_FANOUT_T0" >/dev/null 2>&1 &

# ===========================================
# MEMORY RETRIEVAL: Choose strategy based on mode
# ===========================================
# RLM mode: Use soul_repl for dynamic exploration (more powerful but slower)
# Standard mode: Use structured_recall (three-lens: facts/context/temporal)
RLM_MODE="${CHITTA_RLM_MODE:-${CC_SOUL_RLM_MODE:-}}"

# Lane ablation (CHITTA_ABLATE_LANES=sem,ctx,hyb,kw,corr,xr — comma list;
# CC_SOUL_ABLATE_LANES still honored), defined here (not inside the lane
# block below) so it is always callable — including from the cross-realm
# fallback, which runs even in RLM_MODE.
_ABLATE_LANES_RAW="${CHITTA_ABLATE_LANES:-${CC_SOUL_ABLATE_LANES:-}}"
_ABLATE_LANES=",${_ABLATE_LANES_RAW},"
# A turn without content tokens keeps only the correction lanes (see _QTOK_N).
[[ "${_NO_TOPIC_LANES:-0}" == "1" ]] && _ABLATE_LANES="${_ABLATE_LANES}sem,hyb,kw,ctx,xr,"
_lane_ablated() { [[ "$_ABLATE_LANES" == *",$1,"* ]]; }

_RECALL_TELEMETRY_ACTIVE=1
# Fetch continuity concurrently with recall; consume it only when rendering.
_SESSION_PID=""
if [[ ! -f "$MIND_PATH/.session_active" ]] && budget_left; then
    timeout 1 "$CHITTA_BIN" recall --query "session_summary" --limit 1 \
        >"$_ld/session" 2>/dev/null &
    _SESSION_PID=$!
fi
if [[ -n "$RLM_MODE" ]]; then
    # RLM-style exploration via Python soul_repl.
    # Query passed via env, never interpolated into Python source — the raw
    # prompt is attacker-influenceable text.
    memories=$(CC_SOUL_RLM_QUERY="$QUERY" CC_SOUL_MCP_DIR="${SCRIPT_DIR}/../chitta-mcp" \
        timeout "$MAX_WAIT" python3 -c "
import os, sys
sys.path.insert(0, os.environ['CC_SOUL_MCP_DIR'])
from server import handle_smart_context
result = handle_smart_context({'mode': 'rlm', 'task': os.environ.get('CC_SOUL_RLM_QUERY', '')})
print(result)
" 2>/dev/null || true)
else
    # Run structured recall + hybrid recall in parallel, then merge.
    # Hybrid (BM25 + semantic + graph) catches literal tokens (filenames, IDs, paths)
    # that pure semantic recall misses when they have low cosine similarity.
    # Each lane runs in a backgrounded subshell writing to a file. `var=$(cmd) &`
    # captures the output inside the subshell and loses it — wait collects exit
    # status, not variables — so lanes must hand results back through the filesystem.
    # Lane ablation: a listed lane's recall call is skipped and its file stays
    # empty, exactly like a real timeout, so every downstream consumer (merge,
    # C2, admit accounting) already treats it as "no signal" with no separate
    # code path. `_lane_ablated`/`_ABLATE_LANES` are defined once, above the
    # RLM_MODE branch. Pre-touch every non-conditional lane file so
    # `$(<"$_ld/<lane>")` below never hits a missing-file error when a lane is
    # skipped. `corrk` isn't ablatable — it's the deterministic correction
    # probe, not one of the fuzzy recall lanes the env targets; `xr` is gated
    # further down, at the cross-realm fallback itself.
    touch "$_ld/sem" "$_ld/ctx" "$_ld/hyb" "$_ld/kw" "$_ld/corr" "$_ld/corrk"
    # Reuse the adjacent log_event timestamp as the common dispatch boundary:
    # each duration includes scheduler wait with no added start-clock process.
    _LANE_FANOUT_T0=$_RECALL_FANOUT_T0
    _lanes_rpc_ok=0
    # Default ON since 2026-09-13: bench (3 queries × 20 runs/arm) median 1496→1201 ms,
    # p95 3208→3087, 0 empties both arms; fail-open fallback to the six-process path.
    if [[ "${CHITTA_RECALL_LANES_RPC:-${CC_SOUL_RECALL_LANES_RPC:-1}}" == "1" ]]; then
        _rpc_lane_names=()
        for _rpc_lane in sem hyb kw corr; do
            _lane_ablated "$_rpc_lane" || _rpc_lane_names+=("$_rpc_lane")
        done
        _rpc_lane_names+=(corrk)
        if [[ -n "$CTX_QUERY" ]] && ! _lane_ablated ctx; then
            _rpc_lane_names+=(ctx)
        fi
        _rpc_lanes_json="["; _rpc_sep=""
        for _rpc_lane in "${_rpc_lane_names[@]}"; do
            _rpc_lanes_json+="${_rpc_sep}\"${_rpc_lane}\""; _rpc_sep=","
        done
        _rpc_lanes_json+="]"
        _rpc_cmd=("$CHITTA_BIN" recall_lanes --json --query "$QUERY" --realm "$REALM"
                  --lanes "$_rpc_lanes_json"
                  --limits "{\"sem\":6,\"ctx\":4,\"hyb\":${HYB_LANE_LIMIT},\"kw\":3,\"corr\":3}")
        [[ -n "$CTX_QUERY" ]] && _rpc_cmd+=(--ctx-query "$CTX_QUERY")
        if timeout "$((MAX_WAIT + 1))" "${_rpc_cmd[@]}" >"$_ld/rpc.json" 2>/dev/null; then
            # Validate every requested lane before writing any lane file. NUL
            # framing retains tabs, newlines and trailing newlines in lane text.
            if jq -sjer --argjson names "$_rpc_lanes_json" '
                if length == 1 then .[0] else error("expected one recall response") end |
                .lanes as $lanes |
                if all($names[]; . as $name | $lanes[$name] |
                    (.text | type == "string") and
                    (.ms | type == "number" and . >= 0 and . == floor) and
                    (.timed_out | type == "boolean") and
                    (.results | type == "array")) then
                    $names[] as $name | $lanes[$name] |
                    $name, "\u0000", (.ms | tostring), "\u0000",
                    (.timed_out | tostring), "\u0000", (.text | gsub("\u0000"; "")), "\u0000"
                else error("invalid recall lane") end
            ' "$_ld/rpc.json" >"$_ld/rpc.records" 2>/dev/null; then
                : >"$_ld/rpc.stats"
                while IFS= read -r -d '' _rpc_lane &&
                      IFS= read -r -d '' _rpc_ms &&
                      IFS= read -r -d '' _rpc_timeout &&
                      IFS= read -r -d '' _rpc_text; do
                    printf '%s' "$_rpc_text" >"$_ld/$_rpc_lane"
                    printf '%s\t%s\t%s\n' "$_rpc_lane" "$_rpc_ms" "$_rpc_timeout" >>"$_ld/rpc.stats"
                done <"$_ld/rpc.records"
                while IFS=$'\t' read -r _rpc_lane _rpc_ms _rpc_timeout; do
                    [[ -n "$_rpc_lane" ]] || continue
                    _LANE_MS["$_rpc_lane"]="$_rpc_ms"
                    _LANE_TIMEOUT["$_rpc_lane"]="$_rpc_timeout"
                done <"$_ld/rpc.stats"
                _lanes_rpc_ok=1
            fi
        fi
    fi

    # Fail open on an unavailable/invalid fan-in RPC by running the unchanged
    # process fan-out for this prompt.
    if [[ "$_lanes_rpc_ok" -ne 1 ]]; then
        if ! _lane_ablated sem; then
            _launch_lane sem "$MAX_WAIT" "$CHITTA_BIN" smart_recall --query "$QUERY" --limit 6 --realm "$REALM"
        fi
        # Hybrid gets +1s: it carries the C2 maxrel and the fused hits, and a cold
        # query-embed takes ~3.7s (measured 2026-09-01) vs ~0.4s warm. Losing it
        # drops C2 to none and silences every lane.
        if ! _lane_ablated hyb; then
            _launch_lane hyb "$((MAX_WAIT + 1))" "$CHITTA_BIN" recall --query "$QUERY" --strategy hybrid --limit "$HYB_LANE_LIMIT" --realm "$REALM"
        fi
        # Pure keyword lane: BM25 only, surfaces exact tokens (filenames, IDs, paths)
        # regardless of semantic similarity. Plain format (NOT --toon): the daemon's
        # TOON results[] schema is variable (an optional `affect` key) and its `atoms`
        # column is nested JSON carrying its own commas/quotes, which broke the old
        # positional-sed parser → the kw lane silently emitted zero lines every turn.
        # The plain format prints the same `[NN%] [type] text` shape as the hyb lane,
        # so it merges via the identical `[kw]`-prefix idiom below with no CSV parse.
        if ! _lane_ablated kw; then
            _launch_lane kw "$MAX_WAIT" "$CHITTA_BIN" recall --query "$QUERY" --strategy keyword \
                         --limit 3 --realm "$REALM"
        fi
        if ! _lane_ablated corr; then
            _launch_lane corr "$MAX_WAIT" "$CHITTA_BIN" recall --query "$QUERY" --tag "correction" --limit 3 --realm "$REALM" --include-global true
        fi
        # DETERMINISTIC correction lane (capability #2). Unlike the fuzzy --tag lane
        # above (which ranks corrections by cosine and drops the right one ~99% of
        # the time), correction_check is an exact-key bigram probe: if this turn
        # re-states a corrected mistake, its correction FIRES regardless of ranking.
        # Its result is promoted to systemMessage below on a RESERVED slot. Not
        # ablatable — it isn't one of the fuzzy recall lanes the env targets.
        _launch_lane corrk "$MAX_WAIT" "$CHITTA_BIN" correction_check --text "$QUERY"
        # Context lane (step 1): semantic recall against the recency-weighted thread
        # bag, NOT the literal turn. Recruits the memory the current thread is about
        # even when the latest message is anaphoric ("do it properly"). Empty file
        # when CTX_QUERY is unset (lane disabled, first turn, or ablated).
        if [[ -n "$CTX_QUERY" ]] && ! _lane_ablated ctx; then
            _launch_lane ctx "$MAX_WAIT" "$CHITTA_BIN" smart_recall --query "$CTX_QUERY" --limit 4 --realm "$REALM"
        fi
        _wait_lanes
    fi
    _sem_out="" _hyb_out="" _kw_out="" _corr_out="" _corrk_out="" _ctx_out=""
    _read_lane sem _sem_out; _read_lane hyb _hyb_out; _read_lane kw _kw_out
    _read_lane corr _corr_out; _read_lane corrk _corrk_out; _read_lane ctx _ctx_out

    # C2 signal: the hybrid-lane header carries the daemon's Platt-calibrated
    # max_relevance — the "feeling of knowing" scalar the C2 tag is calibrated on
    # (grade-recall.py --c2: gold-hit@3 monotonic KNOWN>THIN>UNKNOWN). Free: no extra call.
    _c2_pct=$(printf '%s\n' "$_hyb_out" | grep -oP '^Found .*\(maxrel \K[0-9]+(?=%\))' | head -1)
    # Empty hybrid result is the strongest UNKNOWN evidence, not a missing signal.
    # (A truly empty lane file = timeout = genuinely no signal: leave _c2_pct unset.)
    if [[ -z "$_c2_pct" && -n "$_hyb_out" ]] && \
       printf '%s' "$_hyb_out" | grep -qE '^(No (memories|messages|results) found|Found 0 results)'; then
        _c2_pct=0
    fi
    # Hybrid-timeout degrade: if the hybrid lane crossed MAX_WAIT under 4-lane
    # contention (empty file → maxrel unparsed → C2 tag silently drops), fetch
    # the header alone with a standalone limit-1 hybrid call. No lane contention
    # this time, so it almost always returns well under MAX_WAIT. This preserves
    # the exact Platt maxrel scalar the C2 tag is calibrated on — the semantic
    # lane's per-item [NN%] is display_pct (raw similarity/RRF), NOT the same
    # quantity, so we must not substitute it. A tag that degrades to a WRONG band
    # is worse than one that briefly retries; if this too times out, _c2_pct
    # stays unset and the tag honestly omits (no signal beats a wrong signal).
    # Only retry while there is budget: this fires precisely when the daemon is
    # congested, and the comment above already accepts an omitted tag as the
    # honest degrade ("no signal beats a wrong signal"). Also skipped when hyb
    # is deliberately ablated — a standalone hybrid call here would silently
    # defeat CHITTA_ABLATE_LANES=hyb by fetching the exact scalar the ablation
    # was asked to withhold. Unset _c2_pct then correctly means "not measured",
    # which the UNKNOWN-silence gate below already treats as no signal, not UNKNOWN.
    if [[ -z "${_c2_pct:-}" ]] && budget_left && ! _lane_ablated hyb; then
        _c2_deg=$(timeout "$MAX_WAIT" "$CHITTA_BIN" recall --query "$QUERY" \
                  --strategy hybrid --limit 1 --realm "$REALM" 2>/dev/null || true)
        _c2_pct=$(printf '%s\n' "$_c2_deg" | grep -oP '^Found .*\(maxrel \K[0-9]+(?=%\))' | head -1)
        if [[ -z "$_c2_pct" && -n "$_c2_deg" ]] && \
           printf '%s' "$_c2_deg" | grep -qE '^(No (memories|messages|results) found|Found 0 results)'; then
            _c2_pct=0
        fi
    fi

    # C2 small-realm relaxation setup (capability #3, default via
    # CHITTA_C2_SMALL_REALM). A scoped realm that is genuinely small can return
    # a confident top hit (hyb display_pct) while the calibrated maxrel still
    # reads UNKNOWN — the calibration was fit on the larger cross-project store,
    # not on a thin project realm. Parse the hyb header's `Found N results in
    # realm 'X'` once; the per-line admission loop below only consults the flag.
    #
    # N alone doesn't say "small realm": the hyb lane always asks for
    # HYB_LANE_LIMIT results, so N==HYB_LANE_LIMIT means the page was filled
    # and the realm could hold far more — that's NOT evidence of a thin store.
    # Only N < HYB_LANE_LIMIT (the query's candidate pool was exhausted before
    # filling the page) is real smallness evidence; _C2_SR_MAXN is then a
    # secondary, tighter absolute cap on top of that.
    _hyb_realm_n=""; _hyb_realm_name=""
    if [[ -n "$_hyb_out" ]] && \
       [[ "$(printf '%s\n' "$_hyb_out" | head -1)" =~ ^Found\ ([0-9]+)\ results\ in\ realm\ \'([^\']+)\' ]]; then
        _hyb_realm_n="${BASH_REMATCH[1]}"; _hyb_realm_name="${BASH_REMATCH[2]}"
    fi
    _hyb_top_pct=$(printf '%s\n' "$_hyb_out" | grep -oE '\[[0-9]+%\]' | head -1 | tr -d '[]%')
    _C2_SR_MAXN="${CHITTA_C2_SMALL_REALM_MAXN:-${CC_SOUL_C2_SMALL_REALM_MAXN:-3}}"
    _C2_SR_MINPCT="${CHITTA_C2_SMALL_REALM_MINPCT:-${CC_SOUL_C2_SMALL_REALM_MINPCT:-50}}"
    _c2_small_realm_relax=0
    if [[ "${CHITTA_C2_SMALL_REALM:-${CC_SOUL_C2_SMALL_REALM:-1}}" == "1" && -n "$_hyb_realm_n" && "$_hyb_realm_name" != "brahman" \
          && "$_hyb_realm_n" -lt "$HYB_LANE_LIMIT" && "$_hyb_realm_n" -le "$_C2_SR_MAXN" \
          && -n "$_hyb_top_pct" && "$_hyb_top_pct" -ge "$_C2_SR_MINPCT" ]]; then
        _c2_small_realm_relax=1
    fi

    # smart_recall keyword-route retagging (capability #2): the daemon's route
    # learner sometimes routes the sem-lane smart_recall call to BM25 ("Smart
    # recall (keyword, ep=N)"); that route's [NN%] is raw BM25 scale (5-11%),
    # which the sem lane's MIN_CONFIDENCE=30 floor drops wholesale. Until the
    # daemon normalizes route pct onto one scale, re-tag those result lines as
    # [kw] here so the BM25 confidence floor and the kw token-share UNKNOWN rule
    # apply instead — the same relief already given to the dedicated kw lane.
    # Semantic/hybrid/temporal/etc. routes are unaffected: only an exact
    # "keyword" header match retags.
    _sem_fmt="$_sem_out"
    if printf '%s\n' "$_sem_out" | head -1 | grep -qE '^Smart recall \(keyword,'; then
        _sem_fmt=$(printf '%s\n' "$_sem_out" | grep -E '\[[0-9]+%\]' | sed 's/^/[kw]/')
    fi

    # Keyword lane (plain format): keep only the per-result header lines
    # ("[NN%] [type] ... text"), drop multi-line bodies / the verbatim-atoms
    # trailer, and prefix [kw] so the admission filter uses the low BM25 floor.
    #
    # Type filter: BM25 fires on literal tokens, so it readily matches memories
    # that merely RESTATE a past user prompt — [belief]/[correction]/[goal]/
    # [preference] nodes are the user talking, not knowledge the reasoner can
    # use (self-referential prompt-echo). It also surfaces [rollup]/[task]/
    # [result]/[episode] process cruft. A relevance judge over 29 replayed real
    # prompts scored the unfiltered kw lane 0.345 useful; restricting to
    # knowledge types lifts it to 0.818 (drops 18 of 19 noise items, loses 1
    # useful). Keep only knowledge-typed results.
    _kw_fmt=$(printf '%s\n' "$_kw_out" | grep -E '^(#[0-9]+ )?\[[0-9]+%\]' | \
              grep -E '\] \[(wisdom|insight|research|data|milestone|convergence)\]' | \
              grep -v '\[thought\]' | sed 's/^/[kw]/')

    # Merge: prefix lane markers WITHOUT consuming the line (`s/^/[lane]/`).
    # Result lines are `#<id> [pct%] ...`; the old `s/^\[/[hyb]/` form replaced
    # the `[` and left `[hyb]55%]`, which the conf parser can't read — hyb and
    # corr were silently dead until 2026-09-01. Header lines carry no `[NN%]`
    # and are dropped by the filter loop below.
    # Strip [thought] from hybrid+keyword lanes — soul:meta artifacts, not domain knowledge.
    memories=$(printf '%s\n' "$_sem_fmt"; \
               printf '%s\n' "$_ctx_out" | grep -v '\[thought\]' | grep -E '\[[0-9]+%\]' | sed 's/^/[ctx]/'; \
               printf '%s\n' "$_hyb_out" | grep -v '\[thought\]' | grep -E '\[[0-9]+%\]' | sed 's/^/[hyb]/'; \
               printf '%s\n' "$_kw_fmt"; \
               printf '%s\n' "$_corr_out" | grep -v '\[thought\]' | grep -E '\[[0-9]+%\]' | sed 's/^/[corr]/')
fi

if [[ -z "$memories" || "$memories" == *"No memories"* ]] || \
   [[ ! "$memories" =~ \[[0-9]+%\] ]]; then
    memories=""
fi

# Cross-realm fallback: if scoped recall found nothing, retry without realm.
# Lets project:geodesic/environment memories surface in foreign-realm sessions.
# Cost: one extra hybrid call (~0.3s). Only fires when scoped recall was empty.
if [[ -z "$memories" ]] && [[ "$REALM" != "brahman" ]] && budget_left && ! _lane_ablated xr; then
    if [[ "$_RECALL_TELEMETRY_ACTIVE" -eq 1 && -n "${_ld:-}" ]]; then
        _run_lane xr "$MAX_WAIT" "$CHITTA_BIN" recall --query "$QUERY" --strategy hybrid --limit 5
        _fallback=$(<"$_ld/xr")
    else
        _fallback=$(timeout "$MAX_WAIT" "$CHITTA_BIN" recall --query "$QUERY" --strategy hybrid \
                    --limit 5 2>/dev/null || true)
    fi
    if [[ -n "$_fallback" && "$_fallback" != *"No memories"* ]]; then
        memories=$(printf '%s\n' "$_fallback" | grep -v '\[thought\]' | grep -E '\[[0-9]+%\]' | sed 's/^/[xr]/')
    fi
fi
if [[ -z "$memories" ]]; then
    [[ "$_RECALL_TELEMETRY_ACTIVE" -eq 1 ]] && _RECALL_EMPTY=1
    [[ -z "$CACHE_WARN" && -z "$SESSION_WARN" ]] && exit 0
fi

# Filter and format results
# Workspace inspector: every candidate carries an admission reason (its recall
# lane), and every rejection is counted by cause. Rendered as per-item [lane]
# tags + one [admit] summary line — the explicit, inspectable admission policy
# (the J-space paper's open problem: "what decides what enters the workspace").
OUTPUT=""
COUNT=0
_adm_sem=0; _adm_ctx=0; _adm_hyb=0; _adm_kw=0; _adm_corr=0; _adm_xr=0
_drop_conf=0; _drop_dup=0; _drop_meta=0; _drop_cap=0; _drop_unk=0
_INJECTED_FILE="${HOOK_STATE_DIR}/.injected_hashes_${SESSION_ID}"
# Phase 1: filter each candidate, collect survivors in merge order (sem first).
_cand_reason=(); _cand_line=()
# M0 turn-entity anchor (SHADOW): distinctive tokens of the cleaned user turn,
# computed once. Used to log — not yet enforce — whether each sem candidate
# shares an entity with the turn. Fable's 80-pair audit found off-topic waste
# (the dominant bucket, 25/54) has zero shared entity with the turn, while every
# USED memory echoes the turn's entities. Shadow-join with the used-signal
# validates the gate before it's allowed to drop live context.
# (_QTOK/_QTOK_N are computed right after CLEAN_QUERY.)

# A terse negation is usually a correction of the immediately preceding turn,
# not a request for topic search. Broad recall here reinforces the negated noun
# (for example, "is not sqlite" used to inject unrelated SQLite memories).
# Keep deterministic correction_check available, but silence every fuzzy lane.
_TERSE_NEGATION=0
_TURN_WORDS=$(printf '%s' "$CLEAN_QUERY" | wc -w | tr -d ' ')
if [[ "${_TURN_WORDS:-0}" -le 8 ]] && printf '%s\n' "$CLEAN_QUERY" | \
   grep -qiE '(^|[[:space:]])(is|are|was|were)[[:space:]]+not([[:space:]]|$)|^(no|not)([[:punct:][:space:]]|$)'; then
    _TERSE_NEGATION=1
    _corr_out=""
fi
while IFS= read -r line; do
    [[ -z "$line" ]] && continue
    # Match both formats: "[79%]..." (full_resonate) and "#123 [kind] [79%]..." (smart_recall)
    [[ ! "$line" =~ \[[0-9]+%\] ]] && continue

    # Admission reason = recall lane (marker prefixed at merge time; sem is unmarked)
    reason="sem"
    case "$line" in
        \[hyb\]*)  reason="hyb";  line="${line#\[hyb\]}" ;;
        \[ctx\]*)  reason="ctx";  line="${line#\[ctx\]}" ;;
        \[kw\]*)   reason="kw";   line="${line#\[kw\]}" ;;
        \[corr\]*) reason="corr"; line="${line#\[corr\]}" ;;
        \[xr\]*)   reason="xr";   line="${line#\[xr\]}" ;;
    esac

    if [[ "$_TERSE_NEGATION" -eq 1 ]]; then
        ((++_drop_unk)); continue
    fi

    # Extract confidence from anywhere in line
    conf=$(echo "$line" | grep -oE '\[[0-9]+%\]' | head -1 | tr -d '[]%')
    # CHITTA_ADMIT_DEBUG=1: one stderr line per candidate (lane, conf, C2,
    # query-token count, small-realm-relax flag) — the only way to see why a
    # memory was not admitted.
    [[ -n "${CHITTA_ADMIT_DEBUG:-${CC_SOUL_ADMIT_DEBUG:-}}" ]] && \
        printf '[admit-debug] lane=%s conf=%s c2=%s qtok=%s sr=%s | %s\n' "$reason" "${conf:-none}" "${_c2_pct:-none}" "${_QTOK_N:-0}" "${_c2_small_realm_relax:-0}" "${line:0:90}" >&2
    [[ -z "$conf" ]] && continue

    # BM25-backed lanes (hyb/kw) surface literal tokens (filenames, IDs, paths)
    # that semantic misses — allow lower confidence threshold
    if [[ "$reason" == "hyb" || "$reason" == "kw" ]]; then
        [[ "$conf" -lt 1 ]] && { ((++_drop_conf)); continue; }
    else
        [[ "$conf" -lt "$MIN_CONFIDENCE" ]] && { ((++_drop_conf)); continue; }
    fi

    # C2 UNKNOWN silence: smart_recall's per-item pct is max-normalized RRF
    # (field_memory_recall.cpp:335,359 — score/max_rrf), so rank-1 is ALWAYS 100%
    # for any query, even off-topic. That defeats MIN_CONFIDENCE. The honest
    # absolute signal is the Platt-calibrated maxrel (_c2_pct, same scalar the C2
    # tag is calibrated on). When it says UNKNOWN (below the confident band, 81),
    # the semantic + context lanes are noise. Keyword/hybrid results may still be
    # useful for an exact filename/ID lookup, but only when they share a distinctive
    # token with the current turn. This prevents stop-word BM25 matches from leaking
    # arbitrary memories while preserving the reason those lanes exist. The fuzzy
    # correction lane is also silenced here; deterministic correction_check output
    # is handled separately below and remains available. Only fires when
    # _c2_pct is actually measured (empty = hybrid timeout = no signal, don't gate).
    # Reversible via CHITTA_UNKNOWN_SILENCE=0.
    #
    # Small-realm relaxation (capability #3, default via CHITTA_C2_SMALL_REALM):
    # the cross-project maxrel calibration under-scores a genuinely small scoped
    # realm — a handful of on-topic memories can pin the calibrated band below 81
    # even though the top hybrid hit is a strong match. _c2_small_realm_relax was
    # set once above (hyb header: few results, in a non-brahman realm, confident
    # top hit). When it's set, extend the SAME token-share rule already applied
    # to kw/hyb/xr to sem/ctx too, instead of dropping them outright. corr stays
    # unconditionally silenced — the fuzzy correction lane's false-positive risk
    # (nack-worthy wrong corrections) isn't offset by a small-realm hit.
    if [[ "${CHITTA_UNKNOWN_SILENCE:-${CC_SOUL_UNKNOWN_SILENCE:-1}}" == "1" && -n "${_c2_pct:-}" && "$_c2_pct" -lt 81 ]]; then
        _unk_shares=0
        if [[ -n "${_QTOK_DISTINCT:-}" ]]; then
            _unk_ctok=$(printf '%s' "$line" | tr '[:upper:]' '[:lower:]' | \
                         grep -oE '[a-z0-9][a-z0-9_>/-]{3,}' | sort -u)
            if [[ -n "$_unk_ctok" ]] && \
               comm -12 <(printf '%s\n' "$_QTOK_DISTINCT") <(printf '%s\n' "$_unk_ctok") | grep -q .; then
                _unk_shares=1
            fi
        fi
        if [[ "$reason" == "sem" || "$reason" == "ctx" ]]; then
            if [[ "$_c2_small_realm_relax" -eq 1 && "$_unk_shares" -eq 1 ]]; then
                :  # small-realm relaxation: shares a distinctive token, let it through
            else
                ((++_drop_unk)); continue
            fi
        elif [[ "$reason" == "corr" ]]; then
            ((++_drop_unk)); continue
        elif [[ "$reason" == "kw" || "$reason" == "hyb" || "$reason" == "xr" ]]; then
            [[ "$_unk_shares" -eq 0 ]] && { ((++_drop_unk)); continue; }
        fi
    fi
    # A keyword row that rides on ONE distinctive token ("missing" in "what else
    # is missing") is a literal-word coincidence unless BM25 is confident.
    if [[ "$reason" == "kw" && "${_QTOK_DISTINCT_N:-0}" -lt 2 && "$conf" -lt "${CHITTA_KW_SINGLE_TOKEN_MIN:-60}" ]]; then
        ((++_drop_conf)); continue
    fi

    # Skip episode thinking-blocks — internal reasoning traces, never useful as injected context
    [[ "$line" =~ \[episode\].*\[thinking.block: ]] && { ((++_drop_meta)); continue; }

    # Skip [thought] synthesis memories — soul:meta artifacts, not domain knowledge.
    # Soul cycles that need [thought] memories query soul:meta explicitly.
    [[ "$line" =~ \[thought\] ]] && { ((++_drop_meta)); continue; }

    # Session counters and cache diagnostics are telemetry, not reusable domain
    # knowledge. Keep them queryable in the store for diagnostics, but never let
    # their generic words ("working", "tools", "session", "cache") win an
    # automatic recall slot.
    if printf '%s' "$line" | grep -qiE '\[session:[^]]+\][[:space:]]+(working|complete|completed|failed|blocked)→[0-9]+[[:space:]]+turns|\[cache:break\]'; then
        ((++_drop_meta)); continue
    fi

    # Checkpoint scaffolding is continuity data for explicit recap/resume flows,
    # not evidence for ordinary prompt-time recall.  Keyword search gives these
    # records an unfair advantage because they repeat generic words such as
    # "context" and "compact", so keep them in the shared store but never inject
    # them automatically into either frontend.
    if printf '%s' "$line" | grep -qiE 'pre-compact[[:space:]]+context:|\[pre-compact:'; then
        ((++_drop_meta)); continue
    fi

    # Discussion-room role prompts describe the frontend that produced an old
    # transcript; they are not durable user preferences.  Persisting one as
    # wisdom/correction can otherwise tell Codex it is Claude (or vice versa).
    # Current system/developer identity always wins, so exclude these artifacts
    # from every automatic recall lane while leaving the source transcript intact.
    if printf '%s' "$line" | grep -qiE 'you are ([*]{0,2})?(claude|codex)(:[^ ,*]+)?([*]{0,2})?.*multi-agent discussion'; then
        ((++_drop_meta)); continue
    fi

    # M0 content-quality floor (Fable-audited over 80 injection/reply pairs; 0 of
    # the matched records were USED). Two waste shapes carry no reusable content
    # for the triggering turn: completion-announcement stubs ("X DONE/complete
    # (Fable <hash>)" with no open question) and bare citation refs (a lone
    # Author_YEAR_ID token). Cheap reversible injection-time floor; the root fix is
    # store-time (don't index these). Off-topic/realm-bleed waste is the larger
    # bucket and is handled separately by the turn-entity anchor gate.
    if [[ "$line" =~ (DONE|complete|completed|launched|shipped).*Fable\ [0-9a-f]{6,} && "$line" != *'?'* ]]; then
        ((++_drop_meta)); continue
    fi
    if [[ ${#line} -lt 90 && "$line" =~ [A-Z][a-z]+_et_al_[0-9]{4}_[A-Za-z0-9]+ ]]; then
        ((++_drop_meta)); continue
    fi

    # Code symbol filtering is now done server-side via --partnership-only flag

    # Lifetime limit: skip memories already injected this session (they ride in history).
    # First injection is kept; re-injection on every subsequent turn is suppressed.
    # Uses a content hash (first 80 chars) since memory IDs aren't always present.
    _line_hash=$(printf '%s' "${line:0:80}" | md5sum | cut -c1-16)
    if [[ -n "$SESSION_ID" && "$SESSION_ID" != "unknown" && -f "$_INJECTED_FILE" ]]; then
        grep -qF "$_line_hash" "$_INJECTED_FILE" 2>/dev/null && { ((++_drop_dup)); continue; }
    fi

    # M0 SHADOW anchor-log (sem + ctx; log-only, does NOT drop). Records whether
    # this candidate shares a distinctive token with the TURN (shares) and with the
    # rolling context THREAD bag (sctx), keyed by the same first-80-char hash the
    # used-signal uses, so the two can be joined offline. The ctx lane is expected
    # to score shares=0 / sctx=1 on anaphoric turns ("do it") — that is the whole
    # point (correct thread recruit the literal query cannot make), and exactly why
    # shares>0 alone cannot validate it (opus's metric caveat).
    if [[ ( "$reason" == "sem" || "$reason" == "ctx" ) && "${_QTOK_N:-0}" -gt 0 ]]; then
        _ctok=$(printf '%s' "$line" | tr '[:upper:]' '[:lower:]' | grep -oE '[a-z0-9][a-z0-9_>/-]{3,}' | sort -u)
        _shares=0; _sctx=0
        if [[ -n "$_ctok" ]] && comm -12 <(printf '%s\n' "$_QTOK") <(printf '%s\n' "$_ctok") | grep -q .; then _shares=1; fi
        if [[ -n "$_ctok" && -n "$_CTXTOK" ]] && comm -12 <(printf '%s\n' "$_CTXTOK") <(printf '%s\n' "$_ctok") | grep -q .; then _sctx=1; fi
        _ah=$(printf '%s' "${line:0:80}" | md5sum | cut -c1-16)
        printf '{"h":"%s","conf":%s,"shares":%d,"sctx":%d,"lane":"%s","qn":%s,"s":"%s"}\n' "$_ah" "${conf:-0}" "$_shares" "$_sctx" "$reason" "${_QTOK_N:-0}" "${SESSION_ID:-unknown}" \
            >> "${MIND_PATH}/.inj_anchor_shadow.jsonl" 2>/dev/null || true
        # ENFORCE (opt-in, default OFF): drop off-topic SEM candidates that share
        # ZERO entity with a FOCUSED turn. NEVER applies to ctx — a correct ctx
        # recruit for "do it" inherently shares no turn-token, so gating it here
        # would delete exactly the anaphora win the lane exists to make. Query-size
        # window guards both ends: tiny qn can't anchor, huge qn is a paste/relay.
        # Only flip on after the join-validation clears BOTH gates (precision(shares=1)
        # >= 2x precision(shares=0) AND used-recall >= 90%) on a FRESH Fable audit.
        if [[ "$reason" == "sem" && "${CHITTA_ANCHOR_ENFORCE:-${CC_SOUL_ANCHOR_ENFORCE:-0}}" == "1" && "$_shares" -eq 0 \
              && "${_QTOK_N:-0}" -ge 3 && "${_QTOK_N:-0}" -le 50 ]]; then
            ((++_drop_meta)); continue
        fi
    fi

    _cand_reason+=("$reason"); _cand_line+=("$line")
done <<< "$memories"

# Phase 2: lane-fair fill (cap 3). Per-item calibrated scores are NOT exposed in
# the hook — smart_recall reports normalized ~100% while BM25 lanes report raw
# display_pct (17-40%), so a global sort by conf just re-monopolizes for sem (the
# exact defect the old reserved-slot hack patched around, and first-3-merge-order
# let sem take all 3). Instead: round-robin across lanes in value order
# (corr > kw > sem > ctx > hyb > xr), taking each lane's best-remaining — arrays
# are already daemon-ranked within lane. No lane takes a 2nd slot until every other
# lane has had a 1st, so exact-filename kw and corrections can't be evicted, sem
# still gets a fair slot instead of monopolizing, and the context lane gets a
# guaranteed shot when present. Subsumes the old reserved slot.
_n=${#_cand_reason[@]}
_sel=()
while (( ${#_sel[@]} < 3 )); do
    _added=0
    for _pref in corr kw sem ctx hyb xr; do
        for ((i = 0; i < _n; i++)); do
            [[ "${_cand_reason[$i]}" != "$_pref" ]] && continue
            _in=0; for j in "${_sel[@]}"; do [[ $j -eq $i ]] && { _in=1; break; }; done
            [[ $_in -eq 1 ]] && continue
            _sel+=("$i"); _added=1; break
        done
        (( ${#_sel[@]} >= 3 )) && break
    done
    (( _added == 0 )) && break
done
_drop_cap=$(( _n - ${#_sel[@]} ))
for i in "${_sel[@]}"; do
    reason="${_cand_reason[$i]}"; line="${_cand_line[$i]}"
    # Truncate and add, tagged with admission reason
    OUTPUT="${OUTPUT}[$reason]${line:0:150}
"
    ((++COUNT))
    case "$reason" in
        sem)  ((++_adm_sem)) ;;
        ctx)  ((++_adm_ctx)) ;;
        hyb)  ((++_adm_hyb)) ;;
        kw)   ((++_adm_kw)) ;;
        corr) ((++_adm_corr)) ;;
        xr)   ((++_adm_xr)) ;;
    esac
    # Record injected hash so this memory is suppressed on future turns
    if [[ -n "$SESSION_ID" && "$SESSION_ID" != "unknown" && -n "${MIND_PATH:-}" ]]; then
        _line_hash=$(printf '%s' "${line:0:80}" | md5sum | cut -c1-16)
        printf '%s\n' "$_line_hash" >> "$_INJECTED_FILE" 2>/dev/null || true
    fi
done

# Preserve the pre-compression lines for the EXIT telemetry finalizer. OUTPUT
# may later be sqz-compressed, which can replace memory ids with dedup refs.
_LEDGER_OUTPUT="$OUTPUT"

# C2 self-monitoring ("feeling of knowing"): bin the daemon's Platt-calibrated
# max_relevance (from the hybrid-lane header) into TWO honest bands.
#
# History: v5.70.0 shipped a 3-band KNOWN(>=86)/THIN(>=81)/UNKNOWN tag. It
# calibrated cleanly on an earlier store state, but store drift inverted the
# KNOWN-vs-THIN boundary (re-measured KNOWN gold-hit@3 0.62 < THIN 0.64), and no
# threshold restored monotonic separation at k=3. A "KNOWN=trust" tag that can be
# rank-1-wrong on a sparse store is worse than a coarser honest one, so we
# re-scoped to 2 bands: KNOWN+THIN merged into one "confident-but-verify" band
# (maxrel >= 81), UNKNOWN below. KNOWN-ish vs UNKNOWN IS monotonic (probes all
# UNKNOWN, hit@10 clean). Both bands carry a behavioral nudge — on a sparse store
# even high maxrel can be rank-1-wrong, so the confident band still says verify.
# (3-band sharpness is recoverable once #13 write-time densification lifts the
# unlinked golds out of the high-maxrel-but-wrong set — same sparse-edge root.)
_c2_tag=""; _c2_phrase=""
if [[ -n "${_c2_pct:-}" ]]; then
    if [[ "$_c2_pct" -ge 81 ]]; then
        _c2_tag="KNOWN"
        _c2_phrase=" | relevant memory present — verify before fully trusting; recall() more if it matters"
    else
        _c2_tag="UNKNOWN"
        _c2_phrase=" | boundary of known memory — do not lean on this; ask or recall explicitly"
    fi
fi

# C2 DISPLAY calibration. The daemon's raw Platt maxrel is over-confident as a
# hit-probability: on a frozen golden snapshot it read mean 0.844 while the true
# hit@3 was 0.533 (ECE 0.31, Brier 0.34). A logistic re-fit on gold-hit data
# (w=1.8029, c=-2.9263 over logit(maxrel)) cuts leave-one-out ECE to 0.159 and
# kills the mean over-confidence (0.844->0.53). We recalibrate the DISPLAYED
# percent only — the KNOWN/UNKNOWN band stays on raw maxrel (its KNOWN-vs-OOD
# separation is already clean), so behavior is unchanged; the number the reasoner
# reads just stops lying. (Daemon-side Platt re-fit is NO-GO: a hardcoded slope
# re-inherits the store-drift staleness that forced the 2-band revert.)
_c2_cal="$_c2_pct"
if [[ -n "${_c2_pct:-}" ]]; then
    _c2_cal=$(awk -v p="$_c2_pct" 'BEGIN{x=p/100; if(x<0.001)x=0.001; if(x>0.999)x=0.999; z=log(x/(1-x)); v=1/(1+exp(-(1.8029*z-2.9263))); printf "%d", v*100+0.5}' 2>/dev/null || echo "$_c2_pct")
fi

# [admit] summary: C2 tag + admitted per lane + rejected per cause. Only
# lanes/causes with nonzero counts are shown to keep the line minimal.
ADMIT_LINE=""
if [[ $COUNT -gt 0 || $((_drop_conf + _drop_dup + _drop_meta + _drop_cap + _drop_unk)) -gt 0 ]]; then
    _in=""
    [[ $_adm_sem  -gt 0 ]] && _in="$_in sem:$_adm_sem"
    [[ $_adm_ctx  -gt 0 ]] && _in="$_in ctx:$_adm_ctx"
    [[ $_adm_hyb  -gt 0 ]] && _in="$_in hyb:$_adm_hyb"
    [[ $_adm_kw   -gt 0 ]] && _in="$_in kw:$_adm_kw"
    [[ $_adm_corr -gt 0 ]] && _in="$_in corr:$_adm_corr"
    [[ $_adm_xr   -gt 0 ]] && _in="$_in xr:$_adm_xr"
    _out=""
    [[ $_drop_conf -gt 0 ]] && _out="$_out conf:$_drop_conf"
    [[ $_drop_dup  -gt 0 ]] && _out="$_out dup:$_drop_dup"
    [[ $_drop_meta -gt 0 ]] && _out="$_out meta:$_drop_meta"
    [[ $_drop_cap  -gt 0 ]] && _out="$_out cap:$_drop_cap"
    [[ $_drop_unk  -gt 0 ]] && _out="$_out unk:$_drop_unk"
    _sr_tag=""; [[ "${_c2_small_realm_relax:-0}" -eq 1 ]] && _sr_tag=" sr:on"
    ADMIT_LINE="[admit]${_ABLATE_LANES_RAW:+ abl:$_ABLATE_LANES_RAW}${_c2_tag:+ C2:$_c2_tag($_c2_cal%)}${_sr_tag}${_in:- none} | drop${_out:- none}"
fi

# ===========================================
# SUS: Log memory exposures for utility scoring (fire-and-forget)
# ===========================================
(
    _sus_ids="["
    _sus_ranks="["
    _sus_scores="["
    _sus_first=true
    _sus_rank=0

    while IFS= read -r _sus_line; do
        [[ -z "$_sus_line" ]] && continue
        [[ ! "$_sus_line" =~ \[[0-9]+%\] ]] && continue

        # Extract memory ID: #NNN at start of line (after any [lane] marker)
        _sus_line="${_sus_line#\[[a-z]*\]}"
        if [[ "$_sus_line" =~ ^#([0-9]+) ]]; then
            _sus_mid="${BASH_REMATCH[1]}"
        else
            continue
        fi

        # Extract confidence percentage
        _sus_pct=$(echo "$_sus_line" | grep -oE '\[[0-9]+%\]' | head -1 | tr -d '[]%')
        [[ -z "$_sus_pct" ]] && continue
        [[ "$_sus_pct" -lt "$MIN_CONFIDENCE" ]] && continue

        (( _sus_rank++ ))
        if [[ "$_sus_first" == "true" ]]; then
            _sus_first=false
        else
            _sus_ids+=","
            _sus_ranks+=","
            _sus_scores+=","
        fi
        _sus_ids+="$_sus_mid"
        _sus_ranks+="$_sus_rank"
        _sus_scores+="$(awk "BEGIN{printf \"%.2f\", $_sus_pct/100}")"

        [[ $_sus_rank -ge 3 ]] && break
    done <<< "$memories"

    _sus_ids+="]"
    _sus_ranks+="]"
    _sus_scores+="]"

    if [[ "$_sus_ids" != "[]" && -n "$SESSION_ID" ]]; then
        queue_write "log_exposure" "{\"session_id\":\"$SESSION_ID\",\"turn_id\":$TURN_INDEX,\"hook_type\":\"user_prompt\",\"memory_ids\":$_sus_ids,\"ranks\":$_sus_ranks,\"resonance_scores\":$_sus_scores}"
    fi
) 2>/dev/null || true

# v6.1: Predicate re-falsification — fire background predicate_run for recalled
# memories (confidence decay handled by store.rs). Surface cached STALE warnings.
#
# ceiling: this block is unreachable. `_sus_ids` is built entirely inside the
# `( ... )` subshell above, so it is always unset here and the guard is always
# false — no predicate_run has ever fired from the prompt hook. Reported by
# ShellCheck as SC2030/SC2031 on 2026-09-02. Left as-is deliberately: enabling
# it adds up to 10 daemon round-trips to every prompt, which is a latency
# change to measure on its own, not to smuggle into a cleanup.
# upgrade: hoist the accumulation out of the subshell (or have it write
# `$MIND_PATH/.sus_ids_$SESSION_ID`), then re-benchmark the prompt path.
if [[ -n "${_sus_ids:-}" && "${_sus_ids:-}" != "[]" ]]; then
    _pred_mids=$(echo "$_sus_ids" | tr -d '[]' | tr ',' '\n' | grep -E '^[0-9]+$' | head -10)
    _stale_warn=""
    for _mid in $_pred_mids; do
        [[ -z "$_mid" ]] && continue
        budget_left || break   # up to 10 × 0.3s; STALE warnings are advisory
        _pst=$(timeout 0.3 "$CHITTA_BIN" predicate_list --memory_id "$_mid" --json 2>/dev/null || true)
        _est=$(echo "$_pst" | jq -r '.epistemic_status // ""' 2>/dev/null || true)
        if [[ "$_est" == "Failed" || "$_est" == "Mixed" ]]; then
            _cmd=$(echo "$_pst" | jq -r '.predicates[0].check_cmd // ""' 2>/dev/null | cut -c1-60 || true)
            _stale_warn="${_stale_warn}[STALE:#${_mid}/${_est}] ${_cmd}"$'\n'
        fi
        "$CHITTA_BIN" predicate_run --memory_id "$_mid" >/dev/null 2>&1 &
    done
    if [[ -n "$_stale_warn" ]]; then
        LEARNING_HINTS="${LEARNING_HINTS:+$LEARNING_HINTS
}⚠ STALE MEMORIES (predicate failed — may be outdated):
${_stale_warn}"
    fi
fi

# Save exposed correction memory IDs for M metric (stop-hook reads this)
_sus_corr_ids="["
_sus_corr_first=true
_sus_last_mid=""
while IFS= read -r _sus_corr_line; do
    [[ -z "$_sus_corr_line" ]] && continue
    # Track memory IDs from #NNN lines
    if [[ "$_sus_corr_line" =~ ^#([0-9]+) ]]; then
        _sus_last_mid="${BASH_REMATCH[1]}"
        continue
    fi
    # Check if content line has [correction] marker
    if [[ -n "$_sus_last_mid" && "$_sus_corr_line" =~ ^[[:space:]]+\[correction\] ]]; then
        [[ "$_sus_corr_first" == "true" ]] && _sus_corr_first=false || _sus_corr_ids+=","
        _sus_corr_ids+="$_sus_last_mid"
        _sus_last_mid=""
    fi
done <<< "${memories:-}"
# Keyed lane (capability #2): a fired correction is deterministically injected to
# systemMessage, so it is definitively "exposed" — record its id for the M metric
# even though it never rode the fuzzy $memories lane.
if [[ -n "${_corrk_out:-}" && "$_corrk_out" =~ FIRED\ \(#([0-9]+) ]]; then
    _sus_kid="${BASH_REMATCH[1]}"
    [[ "$_sus_corr_first" == "true" ]] && _sus_corr_first=false || _sus_corr_ids+=","
    _sus_corr_ids+="$_sus_kid"
fi
_sus_corr_ids+="]"
if [[ "$_sus_corr_ids" != "[]" && -n "${SESSION_ID:-}" && -n "${MIND_PATH:-}" ]]; then
    printf '%s' "$_sus_corr_ids" > "${HOOK_STATE_DIR}/.exposed_corrections_${SESSION_ID}"
fi

# ===========================================
# PATTERN DETECTION: Detect learning opportunities
# fasttext model when available, regex fallback
# ===========================================
LEARNING_HINTS=""
_CLASSIFIER_MODEL="${MIND_PATH}/hook-classifier.bin"
_CLASSIFIER_THRESHOLD="0.55"
_CLASSIFIER_SCRIPT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../scripts" 2>/dev/null && pwd)/classify-intent.py"

_DETECTED_INTENT=""
_DETECTED_SCORE=""


# Skip intent detection when user is quoting/discussing hook output (feedback loop prevention)
if echo "$_INTENT_QUERY" | grep -qE "(\[LEARN\]|\[DISCIPLINE\]|UserPromptSubmit says|learn_correction NOW|context-bloat)"; then
    LEARNING_HINTS=""
    _skip_intent=1
else
    _skip_intent=0
fi

# Regex fallback for each category (used when model absent OR as safety net)
_regex_correction=0
_regex_preference=0
_regex_belief=0
_regex_milestone=0
_regex_frustration=0

if [[ $_skip_intent -eq 0 ]]; then
echo "$_INTENT_QUERY" | grep -qiE "(that'?s (wrong|incorrect|not right|not what)|you('re| are) (wrong|incorrect|mistaken|off)|use your memory|check.*your memory|did you forget|you forgot\b|you missed\b|that breaks\b|wrong order\b|not like that\b|not this way\b|before.*not after\b|I (said|meant) .{0,30}not\b|^no[,. ].{0,50}(instead|should|is|use|try|that|the)\b)" \
    && _regex_correction=1 || true
echo "$_INTENT_QUERY" | grep -qiE "(I (prefer|like|always|never|don'?t like)|please (don'?t|always|never)|stop doing|keep doing|from now on|in the future|more concise|always use\b|never use\b|don'?t use\b|use .* instead\b|prefer .* over\b|no inline\b|no comments\b|no stubs\b|no placeholders\b|^(always|never) [a-z])" \
    && _regex_preference=1 || true
echo "$_INTENT_QUERY" | grep -qiE "(I always|we always|I never|we never|our convention|our standard|we typically|we usually|by convention|in this (project|codebase|repo)|the standard (approach|way)|we (always|never) use|our (approach|workflow|setup) is)" \
    && _regex_belief=1 || true
echo "$_INTENT_QUERY" | grep -qiE "(it works|finally|success|done|shipped|released|completed|finished|passed|merged|deployed)" \
    && _regex_milestone=1 || true
echo "$_INTENT_QUERY" | grep -qiE "(frustrated|annoyed|confused|stuck|lost|this is (hard|difficult|confusing)|I give up|help me understand|what am I missing|tedious|repetitive|not sure|overthinking)" \
    && _regex_frustration=1 || true

# Model verdicts only affect storage when one of these regexes also matches.
# Avoid loading Python/fastText on every ordinary turn (including quoted hints).
if (( _regex_correction || _regex_preference || _regex_belief || _regex_milestone )) &&
   [[ -f "$_CLASSIFIER_MODEL" && -x "$_CLASSIFIER_SCRIPT" ]]; then
    _clf_out=$(echo "$_INTENT_QUERY" | python3 "$_CLASSIFIER_SCRIPT" "$_CLASSIFIER_MODEL" "$_CLASSIFIER_THRESHOLD" 2>/dev/null || true)
    if [[ -n "$_clf_out" ]]; then
        _DETECTED_INTENT="${_clf_out%% *}"
        _DETECTED_SCORE="${_clf_out##* }"
    fi
fi

# Merge: every auto-stored category requires BOTH model + regex — model-alone
# stored questions/probes as durable memories (echo chamber: ask about X twice
# and recall returns your own prompt as C2:KNOWN); tested equally leaky for
# milestone ("run the pipeline again" → [milestone] on model verdict alone).
_intent_correction=0
_intent_preference=0
_intent_belief=0
_intent_milestone=0

[[ "$_DETECTED_INTENT" == "correction" && $_regex_correction -eq 1 ]] && _intent_correction=1 || true
[[ "$_DETECTED_INTENT" == "preference" && $_regex_preference -eq 1 ]] && _intent_preference=1 || true
[[ "$_DETECTED_INTENT" == "belief"     && $_regex_belief -eq 1     ]] && _intent_belief=1     || true
[[ "$_DETECTED_INTENT" == "milestone"  && $_regex_milestone -eq 1  ]] && _intent_milestone=1  || true

# --- ACT on detected intents ---

if [[ $_intent_correction -eq 1 ]]; then
    correction_ctx=$(echo "$_INTENT_QUERY" | head -c 200 | tr '\n' ' ')
    LEARNING_HINTS="[LEARN] ⚠️ CORRECTION detected - call learn_correction NOW
  User said: \"${correction_ctx}\""
    echo "$_INTENT_QUERY" > "${HOOK_STATE_DIR}/.last_correction_context"
    _corr_payload=$(jq -n --arg c "[correction] $correction_ctx" --arg r "$REALM" \
        '{content: $c, category: "correction", realm: $r, tags: ["correction","auto"], visibility: 2}')
    queue_write "observe" "$_corr_payload" 2>/dev/null || true
fi

if [[ $_intent_preference -eq 1 ]]; then
    LEARNING_HINTS="${LEARNING_HINTS:+$LEARNING_HINTS; }[LEARN] Preference detected → use learn_preference tool"
    _pref_ctx=$(echo "$_INTENT_QUERY" | head -c 200 | tr '\n' ' ')
    _pref_payload=$(jq -n --arg c "[preference] $_pref_ctx" --arg r "$REALM" \
        '{content: $c, category: "preference", realm: $r, tags: ["preference","auto"], visibility: 2}')
    queue_write "observe" "$_pref_payload" 2>/dev/null || true
fi

if [[ $_intent_belief -eq 1 ]]; then
    _belief_ctx=$(echo "$_INTENT_QUERY" | head -c 200 | tr '\n' ' ')
    _belief_payload=$(jq -n --arg c "[belief] $_belief_ctx" --arg r "$REALM" \
        '{content: $c, category: "belief", realm: $r, tags: ["belief","auto"], visibility: 2}')
    queue_write "observe" "$_belief_payload" 2>/dev/null || true
fi

if [[ $_regex_frustration -eq 1 ]]; then
    LEARNING_HINTS="${LEARNING_HINTS:+$LEARNING_HINTS; }[LEARN] User state detected → use learn_approach if something helps"
fi

if [[ $_intent_milestone -eq 1 ]]; then
    milestone_text=$(echo "$_INTENT_QUERY" | head -c 300 | tr '\n' ' ')
    queue_write "observe" "{\"content\":\"[milestone] $milestone_text\",\"category\":\"milestone\",\"realm\":\"$REALM\",\"tags\":[\"milestone\"]}"
fi
fi  # end _skip_intent guard

# ===========================================
# TURN DISCIPLINE: Nudge if too many turns without storing
# Inspired by SAGE's 7-call enforcement. We warn, not block.
# ===========================================
STORE_INTERVAL="${CHITTA_STORE_INTERVAL:-${CC_SOUL_STORE_INTERVAL:-7}}"
LAST_STORE_FILE="${HOOK_STATE_DIR}/.last_store_turn_${SESSION_ID}"
# Initialize on first prompt of a session (state file absent = fresh or resumed session)
if [[ ! -f "$LAST_STORE_FILE" ]]; then
    echo "$TURN_INDEX" > "$LAST_STORE_FILE"
fi
last_store_turn=$(cat "$LAST_STORE_FILE" 2>/dev/null || echo "$TURN_INDEX")

# If post-bash-hook auto-stored a milestone recently, credit it as a store
AUTO_STORE_TS_FILE="${HOOK_STATE_DIR}/.last_auto_store_ts"
if [[ -f "$AUTO_STORE_TS_FILE" ]]; then
    _auto_ts=$(cat "$AUTO_STORE_TS_FILE" 2>/dev/null || echo 0)
    _age=$(( $(date +%s) - _auto_ts ))
    if [[ $_age -lt 120 ]]; then
        echo "$TURN_INDEX" > "$LAST_STORE_FILE"
        last_store_turn=$TURN_INDEX
    fi
fi

turns_since_store=$((TURN_INDEX - last_store_turn))
if [[ $turns_since_store -ge $STORE_INTERVAL && $TURN_INDEX -gt 0 ]]; then
    # Hard block at 3× interval when CHITTA_DISCIPLINE_ENFORCE=1
    if [[ $turns_since_store -ge $((STORE_INTERVAL * 3)) && "${CHITTA_DISCIPLINE_ENFORCE:-${CC_SOUL_DISCIPLINE_ENFORCE:-0}}" == "1" ]]; then
        printf '{"decision":"block","reason":"[DISCIPLINE] %d turns without a soul store. Call remember/learn_correction/learn_milestone before continuing — memories are the persistent layer that survives compaction."}\n' "$turns_since_store"
        exit 0
    fi
    LEARNING_HINTS="${LEARNING_HINTS:+$LEARNING_HINTS; }[DISCIPLINE] $turns_since_store turns without storing — consider remember/learn_correction/learn_milestone"
    # Auto-store is handled by the stop hook (PostToolUse) which has the full
    # assistant response and can produce a richer turn summary.
fi

# ===========================================
# NARRATIVE: Log user message (always) + get status (periodic)
# ===========================================
NARRATIVE_STATUS=""

# Log user_message event via CLI. Both discard their output, so nothing below
# reads them: run the pair in one background subshell (preserving gate_init →
# narrative_log ordering) instead of blocking the prompt for up to 1s.
summary=$(echo "$QUERY" | head -c 200 | tr '\n' ' ')
# Redirect the subshell itself, not just the commands inside it: a background child
# holding the hook's stdout open keeps the harness waiting for EOF on the context it
# reads from us, which would cost back the latency backgrounding is meant to save.
(
    timeout 0.5 "$CHITTA_BIN" gate_init --session_id "$SESSION_ID"
    timeout 0.5 "$CHITTA_BIN" narrative_log --session_id "$SESSION_ID" --kind "user_message" --summary "$summary"
) >/dev/null 2>&1 &

# Token diet: narrative/anticipation/habits/goals only fire every Nth turn
# Saves 4-6 daemon calls and ~200-400 output tokens on non-Nth turns
ENRICH_INTERVAL="${CHITTA_ENRICH_INTERVAL:-${CC_SOUL_ENRICH_INTERVAL:-5}}"
ENRICH_TURN=$(( TURN_INDEX % ENRICH_INTERVAL == 0 || TURN_INDEX <= 1 ? 1 : 0 ))
# The five enrichment calls below (narrative_status, anticipation_filter,
# anticipation_predict, habit_match, goal_list, curiosity_gaps) are serial and
# 1s-capped each. They are decoration: skip the whole set rather than lose the
# recall payload to the hook timeout.
budget_left || ENRICH_TURN=0

if [[ $ENRICH_TURN -eq 1 ]]; then
    response=$(timeout 1 "$CHITTA_BIN" narrative_status --session_id "$SESSION_ID" --json 2>/dev/null || true)
    if [[ -n "$response" ]]; then
        mode=$(echo "$response" | jq -r '.metadata.mode // "unknown"' 2>/dev/null)
        confidence=$(echo "$response" | jq -r '.metadata.confidence // 0' 2>/dev/null)
        if [[ "$mode" != "unknown" && "$mode" != "null" ]]; then
            conf_pct=$(awk "BEGIN {printf \"%.0f\", $confidence * 100}")
            NARRATIVE_STATUS="[narrative:$mode:$conf_pct%]"
        fi
    fi
fi

# ===========================================
# ANTICIPATION: Predict likely next actions (periodic — token diet)
# ===========================================
ANTICIPATIONS=""
PREDICTIONS_FILE="${HOOK_STATE_DIR}/.last_predictions.json"

# Clear old predictions
rm -f "$PREDICTIONS_FILE" 2>/dev/null

if [[ $ENRICH_TURN -eq 1 ]]; then
    # Call anticipation_filter via CLI to get gated predictions
    response=$(timeout 1 "$CHITTA_BIN" anticipation_filter --session_id "$SESSION_ID" --max 3 --json 2>/dev/null || true)

    if [[ -n "$response" ]]; then
        candidates=$(echo "$response" | jq -r '.metadata.candidates // []' 2>/dev/null)

        if [[ "$candidates" != "[]" && -n "$candidates" ]]; then
            echo "$candidates" > "$PREDICTIONS_FILE"

            while read -r candidate; do
                [[ -z "$candidate" ]] && continue
                prediction=$(echo "$candidate" | jq -r '.prediction // ""' 2>/dev/null)
                source=$(echo "$candidate" | jq -r '.source // "rule"' 2>/dev/null)
                confidence=$(echo "$candidate" | jq -r '.confidence // 0' 2>/dev/null)

                if [[ -n "$prediction" && "$prediction" != "null" ]]; then
                    conf_pct=$(awk "BEGIN {printf \"%.0f\", $confidence * 100}")
                    ANTICIPATIONS="${ANTICIPATIONS}[anticipate:$source:$conf_pct%] ${prediction:0:100}
"
                fi
            done <<< "$(echo "$candidates" | jq -c '.[]' 2>/dev/null)"
        fi
    fi

    # Fall back to old anticipation_predict if no candidates from filter
    if [[ -z "$ANTICIPATIONS" ]]; then
        context=$(echo "$QUERY" | tr '\n' ' ')
        response=$(timeout 1 "$CHITTA_BIN" anticipation_predict --context "$context" --limit 3 --json 2>/dev/null || true)

        if [[ -n "$response" ]]; then
            patterns=$(echo "$response" | jq -r '.metadata.patterns // []' 2>/dev/null)

            if [[ "$patterns" != "[]" && -n "$patterns" ]]; then
                while read -r pattern; do
                    [[ -z "$pattern" ]] && continue
                    freq=$(echo "$pattern" | jq -r '.frequency // 0' 2>/dev/null)
                    success=$(echo "$pattern" | jq -r '.success_count // 0' 2>/dev/null)
                    action=$(echo "$pattern" | jq -r '.action // ""' 2>/dev/null)

                    if [[ "$freq" -gt 2 || "$success" -gt 0 ]] && [[ -n "$action" ]]; then
                        ANTICIPATIONS="${ANTICIPATIONS}[anticipate] ${action:0:100}
"
                    fi
                done <<< "$(echo "$patterns" | jq -c '.[]' 2>/dev/null)"
            fi
        fi
    fi
fi

# ===========================================
# HABITS: Surface strong habits matching context (periodic — token diet)
# ===========================================
HABITS_OUTPUT=""
if [[ $ENRICH_TURN -eq 1 ]]; then
    context=$(echo "$QUERY" | head -c 100 | tr '\n' ' ')
    response=$(timeout 1 "$CHITTA_BIN" habit_match --context "$context" --min_strength 0.7 --json 2>/dev/null || true)

    if [[ -n "$response" ]]; then
        habits_array=$(echo "$response" | jq -c '.metadata.habits // []' 2>/dev/null)
        if [[ "$habits_array" != "[]" && -n "$habits_array" ]]; then
            while read -r habit; do
                [[ -z "$habit" ]] && continue
                response_text=$(echo "$habit" | jq -r '.response // ""' 2>/dev/null)
                strength=$(echo "$habit" | jq -r '.strength // 0' 2>/dev/null)
                if [[ -n "$response_text" && "$response_text" != "null" ]]; then
                    strength_pct=$(awk "BEGIN {printf \"%.0f\", $strength * 100}")
                    HABITS_OUTPUT="${HABITS_OUTPUT}[habit:${strength_pct}%] ${response_text:0:100}
"
                fi
            done <<< "$(echo "$habits_array" | jq -c '.[]' 2>/dev/null)"
        fi
    fi
fi

# ===========================================
# GOALS: Surface active goals for context (periodic — token diet)
# ===========================================
GOALS_OUTPUT=""
if [[ $ENRICH_TURN -eq 1 ]]; then
    response=$(timeout 1 "$CHITTA_BIN" goal_list --status "active" --limit 3 --json 2>/dev/null || true)
    if [[ -n "$response" ]]; then
        goals_array=$(echo "$response" | jq -c '.metadata.goals // []' 2>/dev/null)
        if [[ "$goals_array" != "[]" && -n "$goals_array" ]]; then
            while read -r goal; do
                [[ -z "$goal" ]] && continue
                id=$(echo "$goal" | jq -r '.id // ""' 2>/dev/null)
                title=$(echo "$goal" | jq -r '.title // ""' 2>/dev/null)
                progress=$(echo "$goal" | jq -r '.progress // 0' 2>/dev/null)
                if [[ -n "$title" && "$title" != "null" ]]; then
                    progress_pct=$(awk "BEGIN {printf \"%.0f\", $progress * 100}")
                    GOALS_OUTPUT="${GOALS_OUTPUT}[goal:${id}] ${title:0:80} (${progress_pct}%)
"
                fi
            done <<< "$(echo "$goals_array" | jq -c '.[]' 2>/dev/null)"
        fi
    fi
fi

# ===========================================
# CURIOSITY: Surface unresolved knowledge gaps (once per session)
# ===========================================
CURIOSITY_OUTPUT=""
if [[ ! -f "$MIND_PATH/.gaps_surfaced" ]]; then
    touch "$MIND_PATH/.gaps_surfaced"
    response=$(timeout 1 "$CHITTA_BIN" curiosity_gaps --limit 1 --json 2>/dev/null || true)
    if [[ -n "$response" ]]; then
        gaps_array=$(echo "$response" | jq -c '.metadata.gaps // []' 2>/dev/null)
        if [[ "$gaps_array" != "[]" && -n "$gaps_array" ]]; then
            while read -r gap; do
                [[ -z "$gap" ]] && continue
                content=$(echo "$gap" | jq -r '.content // ""' 2>/dev/null)
                if [[ -n "$content" && "$content" != "null" ]]; then
                    # Extract just the gap text, strip [gap] prefix if present
                    gap_text=$(echo "$content" | sed 's/^\[gap\] //' | head -c 150 | tr '\n' ' ')
                    CURIOSITY_OUTPUT="[curiosity] Unresolved: ${gap_text}"
                    break  # Only show one gap per session
                fi
            done <<< "$(echo "$gaps_array" | jq -c '.[]' 2>/dev/null)"
        fi
    fi
fi

# ===========================================
# SESSION CONTINUITY: Surface last session for context
# ===========================================
if [[ ! -f "$MIND_PATH/.session_active" ]]; then
    touch "$MIND_PATH/.session_active"
    # Surface last session summary for continuity
    recent_session=""
    if [[ -n "$_SESSION_PID" ]]; then
        wait "$_SESSION_PID" 2>/dev/null || true
        recent_session=$(<"$_ld/session")
    fi
    if [[ -n "$recent_session" && "$recent_session" != *"No memories"* ]]; then
        # Recall begins with a count/maxrel header, even when no body survives.
        # Only a result with nonblank content can supply continuity context.
        session_line=$(printf '%s\n' "$recent_session" | \
            grep -m1 -E '^#[0-9]+ \[[0-9]+%\] \[[^]]+\][[:space:]]+[^[:space:]]' | head -c 150)
        [[ -n "$session_line" ]] && ANTICIPATIONS="${ANTICIPATIONS}[last-session] ${session_line}
"
    fi
fi

# ===========================================
# CROSS-SESSION MESSAGING: Heartbeat and inbox check
# ===========================================
CROSS_SESSION_MSGS=""
# Use SESSION_ID from JSON input (already extracted above) - most reliable source
MSG_SESSION_ID="$SESSION_ID"
if [[ -n "$MSG_SESSION_ID" && "$MSG_SESSION_ID" != "default" ]]; then
    # Session register (upserts PID on each prompt - handles Claude restarts/resumes)
    # PPID is Claude's PID (hook runs as: Claude → bash → hook script)
    CLAUDE_PID=${PPID:-$$}
    # Output discarded and nothing below depends on it — don't block the prompt.
    timeout 0.3 "$CHITTA_BIN" session_register --session_id "$MSG_SESSION_ID" --realm "${REALM:-brahman}" --pid "$CLAUDE_PID" >/dev/null 2>&1 &

    # Check notification file from msg-notify daemon first (fast-poll messages)
    NOTIFY_FILE="${MIND_PATH}/.msg_notify/${MSG_SESSION_ID}"
    if [[ -f "$NOTIFY_FILE" && -s "$NOTIFY_FILE" ]]; then
        FAST_MSGS=$(cat "$NOTIFY_FILE" 2>/dev/null || true)
        : > "$NOTIFY_FILE"  # Clear after reading
        if [[ -n "$FAST_MSGS" ]]; then
            CROSS_SESSION_MSGS="${FAST_MSGS}"
        fi
    fi

    # Check for cross-session messages via daemon (authoritative)
    response=$(timeout 1 "$CHITTA_BIN" msg_inbox --session_id "$MSG_SESSION_ID" --limit 3 --min_priority 1 --json 2>/dev/null || true)
    if [[ -n "$response" ]]; then
        msg_count=$(echo "$response" | jq -r '.count // 0' 2>/dev/null)
        if [[ "$msg_count" -gt 0 ]]; then
            # Format messages based on priority:
            # priority 3 = [MSG:URGENT:sender], priority 2 = [MSG:important:sender], else = [msg:sender]
            CROSS_SESSION_MSGS=$(echo "$response" | jq -r '.messages[] |
                (.sender_session_id | if . == "" or . == null then "unknown" else . end) as $sender |
                (.sender_realm      | if . == "" or . == null then "" else "@\(.)" end) as $realm |
                (.sender_host       | if . == "" or . == null then "" else "/\(.)" end) as $host |
                (.memory_id | tostring) as $mid |
                "\($sender)\($realm)\($host)" as $from |
                if .score >= 3 then "[MSG:URGENT:\($from)|id:\($mid)] \(.content)"
                elif .score >= 2 then "[MSG:important:\($from)|id:\($mid)] \(.content)"
                else "[msg:\($from)|id:\($mid)] \(.content)"
                end' 2>/dev/null || true)
            # Ack all messages that were just read. Acks are writes whose output is
            # discarded and nothing below depends on them — don't spend up to 3 × 0.3s
            # of the prompt budget on them.
            # ceiling: if the hook exits before an ack lands, that message is shown
            # once more next prompt; upgrade: single batched msg_ack --message_ids.
            (echo "$response" | jq -r '.messages[].memory_id' | while read -r mid; do
                [[ -n "$mid" && "$mid" != "null" ]] && timeout 0.3 "$CHITTA_BIN" msg_ack --message_id "$mid" || true
            done) >/dev/null 2>&1 &
        fi
    fi
fi

# ===========================================
# OUTPUT — Priority-ordered, budget-capped (~500 chars ≈ ~125 tokens)
# ===========================================
# Token diet: accumulate into FINAL_OUTPUT, enforce hard cap.
# Priority order (highest first): learning hints > cross-session msgs >
# memories > narrative > goals > curiosity > habits > anticipations
# Cache-expired sessions: cache is already busted, expand the memory budget.
if [[ -n "$CACHE_WARN" ]]; then
    MAX_OUTPUT_CHARS="${CHITTA_MAX_OUTPUT_CHARS:-${CC_SOUL_MAX_OUTPUT_CHARS:-2000}}"
else
    MAX_OUTPUT_CHARS="${CHITTA_MAX_OUTPUT_CHARS:-${CC_SOUL_MAX_OUTPUT_CHARS:-500}}"
fi
FINAL_OUTPUT=""

_append() {
    local text="$1"
    [[ -z "$text" ]] && return
    local current_len=${#FINAL_OUTPUT}
    local remaining=$((MAX_OUTPUT_CHARS - current_len))
    [[ $remaining -le 20 ]] && return  # Not enough room for anything useful
    if [[ ${#text} -gt $remaining ]]; then
        text="${text:0:$remaining}"
    fi
    FINAL_OUTPUT="${FINAL_OUTPUT}${text}"
}

# 1. Learning hints (action items — highest priority)
_append "$LEARNING_HINTS"
[[ -n "$LEARNING_HINTS" ]] && _append $'\n'

# 2. Cross-session messages (time-sensitive)
if [[ -n "$CROSS_SESSION_MSGS" ]]; then
    _append "[cross-session messages]"$'\n'
    _append "$CROSS_SESSION_MSGS"$'\n'
    _append "[/cross-session messages]"$'\n'
fi

# 3. Memories (core value)
# Save full exposed memory content for implicit resonance detection (stop-hook)
if [[ ${COUNT:-0} -gt 0 && -n "${memories:-}" && -n "${MIND_PATH:-}" && -n "${SESSION_ID:-}" ]]; then
    printf '%s\n' "$memories" > "${HOOK_STATE_DIR}/.exposed_memories_${SESSION_ID}"
    # hint_recall_hit value signal (PR1): how often hint-origin memories surface in recall.
    # The denominator (this line fires per recall) gates whether PR2 is worth building.
    _hint_surf=$(printf '%s\n' "$memories" | grep -cF '[hint:realtime]' 2>/dev/null || true)
    printf '{"ts":"%s","backend":"recall","outcome":"recall","hint_surfaced":%s,"mem_count":%s}\n' \
        "$(date -u +%Y-%m-%dT%H:%M:%SZ)" "${_hint_surf:-0}" "${COUNT:-0}" \
        >> "${MIND_PATH}/hint_metrics.jsonl" 2>/dev/null || true
fi
if [[ -n "$ADMIT_LINE" ]]; then
    if [[ "$_RECALL_TELEMETRY_ACTIVE" -eq 1 ]]; then
        _render_lane_telemetry
    else
        _clock_ms
        _HOOK_ELAPSED_MS=$(( _NOW_MS - _HOOK_T0 ))
        _LANE_TIMING_FIELD=""
    fi
    ADMIT_LINE+=" | t:${_LANE_TIMING_FIELD}${_LANE_TIMING_FIELD:+,}total=${_HOOK_ELAPSED_MS}${_c2_phrase}"
fi
if [[ -n "$OUTPUT" && $COUNT -gt 0 ]]; then
    # #5: sqz intra-turn dedup — collapse repeated memory text seen earlier this session.
    # Only applies within this single turn's recall batch; cross-turn suppression is #1.
    # Opt-in only (CHITTA_SQZ_DEDUP=1): sqz's dedup replaces a memory line it has
    # seen before with a "§ref:HASH§" token the model cannot expand from a hook,
    # so the injected context silently loses the memory. This was masked for
    # weeks by sqz's broken session store (every call failed open) and surfaced
    # on 2026-09-14 the moment the store was repaired.
    _SQZ_BIN="${HOME}/.claude/bin/sqz"
    if [[ "${CHITTA_SQZ_DEDUP:-0}" == "1" && -x "$_SQZ_BIN" ]]; then
        OUTPUT=$(printf '%s' "$OUTPUT" | "$_SQZ_BIN" compress --cmd soul 2>/dev/null || printf '%s' "$OUTPUT")
    fi
    _append "[soul]"$'\n'
    # Reserve budget for the [admit]/C2 line BEFORE appending memories: a
    # truncated self-monitoring signal is worse than slightly shorter memory
    # text (the 500-char cap was silently cutting it mid-line).
    if [[ -n "$ADMIT_LINE" ]]; then
        _admit_need=$(( ${#ADMIT_LINE} + 2 ))
        _room=$(( MAX_OUTPUT_CHARS - ${#FINAL_OUTPUT} - _admit_need ))
        (( _room < 0 )) && _room=0
        [[ ${#OUTPUT} -gt $_room ]] && OUTPUT="${OUTPUT:0:$_room}"
    fi
    _append "$OUTPUT"
    # OUTPUT may lose its trailing newline through sqz compress — guarantee a
    # break before the [admit] summary so it renders on its own line.
    [[ -n "$ADMIT_LINE" ]] && _append $'\n'"${ADMIT_LINE}"$'\n'
    _RECALL_CONTEXT_EMITTED=1
elif [[ "$_c2_tag" == "UNKNOWN" && -n "$ADMIT_LINE" ]]; then
    # Nothing cleared the bar AND recall says this is outside known memory:
    # the boundary itself is the signal — render just the admit line so the
    # agent knows recall ran and came back empty-handed, not that it was skipped.
    _append "[soul]"$'\n'"${ADMIT_LINE}"$'\n'
fi

# 4. Narrative status (one-liner, cheap)
[[ -n "$NARRATIVE_STATUS" ]] && _append "${NARRATIVE_STATUS}"$'\n'

# 5. Goals
[[ -n "$GOALS_OUTPUT" ]] && _append "$GOALS_OUTPUT"

# 6. Curiosity
[[ -n "$CURIOSITY_OUTPUT" ]] && _append "${CURIOSITY_OUTPUT}"$'\n'

# 7. Habits
[[ -n "$HABITS_OUTPUT" ]] && _append "$HABITS_OUTPUT"

# 8. Anticipations (lowest priority)
[[ -n "$ANTICIPATIONS" ]] && _append "$ANTICIPATIONS"

# 9. CEC: Involuntary failure-pattern injection (Phase 3)
# If the CDAWG has a state with fail_ratio > 0.7 and fail_count >= 3,
# inject a one-line warning into systemMessage so it cannot be ignored.
CEC_WARN=""
if command -v jq &>/dev/null && budget_left; then
    _cec_raw=$(timeout 1 "$CHITTA_BIN" recall_failure_pattern --k 1 --json 2>/dev/null || true)
    if [[ -n "$_cec_raw" ]]; then
        _cec_ratio=$(echo "$_cec_raw" | jq -r '.patterns[0].fail_ratio // 0' 2>/dev/null || echo "0")
        _cec_count=$(echo "$_cec_raw" | jq -r '.patterns[0].fail_count // 0' 2>/dev/null || echo "0")
        if awk "BEGIN{exit !($_cec_ratio > 0.7 && $_cec_count >= 3)}" 2>/dev/null; then
            _cec_content=$(echo "$_cec_raw" | jq -r '.patterns[0].content // ""' 2>/dev/null | cut -c1-200 || true)
            [[ -n "$_cec_content" ]] && CEC_WARN="⚠ CEC: ${_cec_content}"
        fi
    fi
fi

# ===========================================
# EMIT: Structured JSON hookSpecificOutput
# JSON output → Claude Code creates hook_additional_context attachment
# (managed independently during compaction, not just system-reminder text)
# Plain text fallback if jq unavailable
# ===========================================
if [[ -n "$FINAL_OUTPUT" || -n "$CACHE_WARN" || -n "$SESSION_WARN" || \
      "${_corrk_out:-}" == CORRECTION\ FIRED* ]]; then
    # Strip trailing whitespace
    FINAL_OUTPUT=$(echo -n "$FINAL_OUTPUT" | sed 's/[[:space:]]*$//')

    if command -v jq &>/dev/null; then
        # Emit as JSON hookSpecificOutput for UserPromptSubmit
        # systemMessage: urgent items shown as warning to user (corrections)
        # additionalContext: everything else, injected as context attachment
        SYSTEM_MSG=""
        [[ -n "$CACHE_WARN" ]] && SYSTEM_MSG="${CACHE_WARN}"
        [[ -n "$SESSION_WARN" ]] && SYSTEM_MSG="${SYSTEM_MSG:+$SYSTEM_MSG | }${SESSION_WARN}"
        [[ -n "$CEC_WARN" ]] && SYSTEM_MSG="${SYSTEM_MSG:+$SYSTEM_MSG | }${CEC_WARN}"
        if [[ -n "$LEARNING_HINTS" && "$LEARNING_HINTS" == *"CORRECTION"* ]]; then
            SYSTEM_MSG="${SYSTEM_MSG:+$SYSTEM_MSG | }${LEARNING_HINTS}"
        fi
        # Escalate DISCIPLINE to systemMessage at 2× interval so it can't be ignored
        if [[ $turns_since_store -ge $((STORE_INTERVAL * 2)) && -n "$LEARNING_HINTS" && "$LEARNING_HINTS" == *"DISCIPLINE"* ]]; then
            SYSTEM_MSG="${SYSTEM_MSG:+$SYSTEM_MSG | }[DISCIPLINE] ${turns_since_store} turns without storing — call remember/learn/milestone NOW"
        fi
        # Promote matched [correction] memories to systemMessage so model cannot ignore them.
        # Guard: only short correction tags (<400 chars total) — never escalate bulk memory
        # volume to systemMessage as that busts the prefix cache at 1.25× write cost.
        # RESERVED SLOT (capability #2): the deterministic keyed lane fires first
        # and unconditionally. A correction whose trigger recurs in this turn is
        # promoted to systemMessage even when the fuzzy --tag lane ranked it out
        # of the top-3 — this is the enforcement that ends the ~99% correction miss.
        _corr_sys=""
        # The negative response also contains the literal word "[correction]"
        # ("NO CORRECTION — no stored [correction] trigger..."). Require the
        # daemon's positive sentinel or every miss becomes a bogus warning.
        if [[ -n "$_corrk_out" && "$_corrk_out" == CORRECTION\ FIRED* ]]; then
            _corr_sys=$(printf '%s' "$_corrk_out" | grep -oE '\[correction\][^|]+' | head -3 | tr '\n' ' ' | cut -c1-400 || true)
        fi
        # Fuzzy lane fills any remaining budget only when the absolute C2 signal
        # says the topic is known. Otherwise an arbitrary high-strength
        # correction can become an urgent system message for an unrelated turn.
        if [[ -z "$_corr_sys" && -n "$_corr_out" && "$_corr_out" != *"No memories"* && \
              -n "${_c2_pct:-}" && "$_c2_pct" -ge 81 ]]; then
            _corr_sys=$(printf '%s' "$_corr_out" | grep -oE '\[correction\][^|]+' | head -3 | tr '\n' ' ' | cut -c1-400 || true)
        fi
        if [[ -n "$_corr_sys" ]]; then
            _corr_sys=${_corr_sys:0:400}
            SYSTEM_MSG="${SYSTEM_MSG:+$SYSTEM_MSG | }CORRECTION: ${_corr_sys}"
        fi
        # Cache-expired: prefix cache is already busted, so escalating memories to systemMessage
        # costs nothing extra. Without this the model has no learned behaviour to check additionalContext
        # and defaults to "I don't know" for project-specific terms.
        if [[ -n "$CACHE_WARN" && ${COUNT:-0} -gt 0 && -n "${OUTPUT:-}" ]]; then
            _top_facts=$(printf '%s' "$OUTPUT" | grep -v '^\[thought\]' | head -4 | \
                sed 's/^\[hyb\]//;s/^\[corr\]//' | cut -c1-600 || true)
            [[ -n "$_top_facts" ]] && SYSTEM_MSG="${SYSTEM_MSG:+$SYSTEM_MSG
}[soul-context]
${_top_facts}
[/soul-context]"
        fi

        JSON_OUT=$(jq -n \
            --arg ctx "$FINAL_OUTPUT" \
            --arg sys "$SYSTEM_MSG" \
            '{
                hookSpecificOutput: {
                    hookEventName: "UserPromptSubmit",
                    additionalContext: $ctx
                }
            } + (if $sys != "" then {systemMessage: $sys} else {} end)' 2>/dev/null)

        if [[ -n "$JSON_OUT" && "$JSON_OUT" == "{"* ]]; then
            echo "$JSON_OUT"
        else
            # jq failed at runtime — fall back to plain text
            echo "$FINAL_OUTPUT"
        fi
    else
        # Fallback: plain text (starts without {, so Claude Code treats as plain text)
        echo "$FINAL_OUTPUT"
    fi
fi

# Real-time hint extraction: every 6 turns starting at turn 6
_HINT_INTERVAL=6
_HINT_MODEL="${CHITTA_HINT_MODEL:-$HOME/.claude/models/chitta-hint-qwen-q4_k_m.gguf}"
_HINT_SCRIPT="$(dirname "${BASH_SOURCE[0]}")/../scripts/hint_realtime.py"
if [[ $((TURN_INDEX % _HINT_INTERVAL)) -eq 0 && ${TURN_INDEX:-0} -ge 6 \
      && -n "${TRANSCRIPT_PATH:-}" && -f "${TRANSCRIPT_PATH:-}" \
      && -f "$_HINT_MODEL" && -f "$_HINT_SCRIPT" ]]; then
    # timeout -k 5 35: hard wall-clock backstop. signal.alarm(30) inside the
    # script cannot interrupt a native llama_cpp hang (GIL held), so without this
    # a hung fire would hold the in-flight flock forever and disable hints all
    # session. SIGTERM at 35s, SIGKILL 5s later → kernel reaps the lock.
    timeout -k 5 35 python3 "$_HINT_SCRIPT" \
        --transcript "$TRANSCRIPT_PATH" \
        --session "$SESSION_ID" \
        --turns 5 \
        --model "$_HINT_MODEL" \
        --chitta-bin "$CHITTA_BIN" \
        --mind-path "$MIND_PATH" \
        &>/dev/null &
fi

exit 0
