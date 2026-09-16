#!/bin/bash
# PreCompact hook: Save structured session state before context compaction
#
# Extracts operational truth from transcript:
# - User intent (from user messages, not assistant prose)
# - Data/code paths (from tool args + text patterns)
# - Commands executed (from Bash tool calls)
# - Progress signals (done/next/blockers from natural language)
# - Decisions (from natural language, not just [DECISION] markers)

# Don't use set -e: we want to save as much state as possible even if parts fail

CHITTA_BIN="${CHITTA_BIN:-$HOME/.claude/bin/chitta}"
MAX_WAIT="${CHITTA_MAX_WAIT:-${CC_SOUL_MAX_WAIT:-2}}"

# Source shared library (provides queue_write with ack_id, get_queue_file, etc.)
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${SCRIPT_DIR}/lib.sh"

# Parse JSON input
INPUT=$(cat)
TRIGGER=$(echo "$INPUT" | jq -r '.trigger // "auto"')
TRANSCRIPT_PATH=$(echo "$INPUT" | jq -r '.transcript_path // empty')
REAL_SESSION_ID=$(echo "$INPUT" | jq -r '.session_id // empty')
# Clear 65% compact sentinel so the next session (same or new id) can block again
[[ -n "$REAL_SESSION_ID" ]] && rm -f "${MIND_PATH}/.compact_advised_${REAL_SESSION_ID}" 2>/dev/null || true

# Check chitta CLI exists
[[ ! -x "$CHITTA_BIN" ]] && exit 0

# Derive project directory from transcript path
PROJECT_DIR=""
if [[ -n "$TRANSCRIPT_PATH" ]]; then
    PROJECT_ENCODED=$(dirname "$TRANSCRIPT_PATH" | xargs basename)
    PROJECT_DIR=$(decode_project_path "$PROJECT_ENCODED")
fi

# Detect realm from project directory
REALM=$(detect_project_realm "$PROJECT_DIR")

SESSION_ID="compact-$(date +%Y%m%d-%H%M%S)"

# ═══════════════════════════════════════════════════════════════════════════
# Extract session state from transcript
# ═══════════════════════════════════════════════════════════════════════════

ACTIVE_FILES="[]"
DECISIONS="[]"
TODOS="[]"
BLOCKERS="[]"
DISCOVERIES="[]"
NEXT_STEPS="[]"
SNAPSHOT=""

if [[ -n "$TRANSCRIPT_PATH" && -f "$TRANSCRIPT_PATH" ]]; then

    # ── Extract user messages (intent source) ─────────────────────────────
    USER_TEXT=$(jq -r '
        select(.type=="user") |
        (if (.message.content|type)=="array"
         then (.message.content | map(.text // "") | join(" "))
         else (.message.content // "" | tostring)
         end)
    ' "$TRANSCRIPT_PATH" 2>/dev/null \
        | sed '/^\s*$/d' \
        | grep -vE '<(system-reminder|command-name|command-message|task-notification|local-command)' \
        | grep -v '^\[Request interrupted' \
        | tail -20)

    # ── Extract assistant text ────────────────────────────────────────────
    ASSISTANT_TEXT=$(jq -r '
        select(.type=="assistant") |
        .message.content[]? | select(.type=="text") | .text
    ' "$TRANSCRIPT_PATH" 2>/dev/null | tail -5000)

    ALL_TEXT=$(printf '%s\n%s\n' "$USER_TEXT" "$ASSISTANT_TEXT")

    # ── Extract paths (fix file_path + filePath mismatch) ─────────────────
    # From tool call arguments
    TOOL_PATHS=$(jq -r '.. | objects | (.filePath // .file_path // .transcript_path // empty)' \
        "$TRANSCRIPT_PATH" 2>/dev/null | sort -u | tail -40)

    # From text content (data file extensions)
    TEXT_PATHS=$(printf '%s\n' "$ALL_TEXT" | \
        grep -oE '(/[^[:space:]"'\''<>|]+\.(bam|cram|sam|vcf|bcf|fastq|fastq\.gz|fq|fq\.gz|tsv|csv|jsonl|parquet|mcaf|ktax|py|rs|sh|cpp|hpp|h|c|ts|js|toml|yaml|yml))' \
        2>/dev/null | sort -u | tail -30)

    # From Bash command arguments (paths referenced in commands)
    CMD_PATHS=$(jq -r '
        select(.type=="assistant") |
        .message.content[]? |
        select(.type=="tool_use" and .name=="Bash") |
        .input.command // empty
    ' "$TRANSCRIPT_PATH" 2>/dev/null \
        | grep -oE '(/[^[:space:]"'\''<>|]+\.(bam|cram|sam|vcf|bcf|fastq|fastq\.gz|fq|fq\.gz|tsv|csv|jsonl|parquet|mcaf|ktax))' \
        2>/dev/null | sort -u | tail -20)

    # Combine all paths, split into data vs code
    ALL_PATHS=$(printf '%s\n%s\n%s\n' "$TOOL_PATHS" "$TEXT_PATHS" "$CMD_PATHS" | sort -u | grep -v '^\s*$')

    DATA_PATHS_RAW=$(printf '%s\n' "$ALL_PATHS" | \
        grep -Ei '\.(bam|cram|sam|vcf|bcf|fastq|fastq\.gz|fq|fq\.gz|tsv|csv|parquet|mcaf|ktax)$' | head -20)
    CODE_PATHS_RAW=$(printf '%s\n' "$ALL_PATHS" | \
        grep -Ei '\.(py|rs|sh|cpp|hpp|h|c|ts|js|toml|yaml|yml)$' | head -20)

    # Merge data + code into active_files (data first — higher priority)
    if [[ -n "$DATA_PATHS_RAW" || -n "$CODE_PATHS_RAW" ]]; then
        ACTIVE_FILES=$(printf '%s\n%s\n' "$DATA_PATHS_RAW" "$CODE_PATHS_RAW" | \
            grep -v '^\s*$' | head -30 | jq -R . | jq -s .)
    else
        # Fallback: tool file_path args
        FILES_RAW=$(grep -oE '"file_path"\s*:\s*"[^"]+"' "$TRANSCRIPT_PATH" 2>/dev/null | \
            sed 's/"file_path"\s*:\s*"//' | sed 's/"$//' | sort -u | tail -20)
        if [[ -n "$FILES_RAW" ]]; then
            ACTIVE_FILES=$(echo "$FILES_RAW" | jq -R . | jq -s .)
        fi
    fi

    # ── Extract commands run (from Bash tool calls) ───────────────────────
    CMDS_RUN=$(jq -r '
        select(.type=="assistant") |
        .message.content[]? |
        select(.type=="tool_use" and .name=="Bash") |
        .input.command // empty
    ' "$TRANSCRIPT_PATH" 2>/dev/null \
        | grep -v '^\s*$' \
        | grep -vE '^(cat|echo|ls |pwd|cd )' \
        | tail -30 \
        | awk '{ line=substr($0, 1, 200); if (!seen[line]++) print line }' \
        | tail -15)

    # ── Extract progress signals (natural language) ───────────────────────
    # "Done" signals — things already completed/tested
    DONE_LINES=$(printf '%s\n' "$ALL_TEXT" | \
        grep -iE '\b(done|completed|finished|already (ran|tested|running|profiled|built|created)|passed|success(fully)?|works|working|resolved|confirmed|verified)\b' \
        2>/dev/null | grep -v '^\s*$' | \
        awk '{ line=substr($0, 1, 200); if (!seen[line]++) print line }' | tail -10)

    # "Next" signals
    NEXT_LINES=$(printf '%s\n' "$ALL_TEXT" | \
        grep -iE '\b(next|todo|remaining|follow[- ]?up|then we|plan to|need to|should|still need)\b' \
        2>/dev/null | grep -v '^\s*$' | \
        awk '{ line=substr($0, 1, 200); if (!seen[line]++) print line }' | tail -10)

    # "Blocker" signals
    BLOCKER_LINES=$(printf '%s\n' "$ALL_TEXT" | \
        grep -iE '\b(blocked|blocker|fail(ed|ure|s)|error|cannot|can'\''t|stuck|timeout|broken|crash|missing)\b' \
        2>/dev/null | grep -v '^\s*$' | \
        awk '{ line=substr($0, 1, 200); if (!seen[line]++) print line }' | tail -8)

    # "Decision" signals (natural language, not just [DECISION] markers)
    DECISION_LINES=$(printf '%s\n' "$ALL_TEXT" | \
        grep -iE '\b(we (chose|decided|switched|went with|use)|decision|instead of|prefer|better to|approach:|strategy:)\b' \
        2>/dev/null | grep -v '^\s*$' | \
        awk '{ line=substr($0, 1, 200); if (!seen[line]++) print line }' | tail -8)
    # Also pick up explicit markers if present
    MARKER_DECISIONS=$(printf '%s\n' "$ASSISTANT_TEXT" | \
        grep -oE '^\[(DECISION|SOLUTION|GOTCHA)\].*$' | head -5)
    if [[ -n "$MARKER_DECISIONS" ]]; then
        DECISION_LINES=$(printf '%s\n%s' "$DECISION_LINES" "$MARKER_DECISIONS")
    fi

    # Build JSON arrays — strip control chars before jq sees them
    _sanitize() { tr -d '\000-\010\013\014\016-\037' | sed 's/[^[:print:]\t]//g'; }
    DECISIONS=$(printf '%s\n' "$DECISION_LINES" | grep -v '^\s*$' | head -10 | _sanitize | jq -R . | jq -s .)
    BLOCKERS=$(printf '%s\n' "$BLOCKER_LINES"   | grep -v '^\s*$' | head -8  | _sanitize | jq -R . | jq -s .)
    NEXT_STEPS=$(printf '%s\n' "$NEXT_LINES"    | grep -v '^\s*$' | head -8  | _sanitize | jq -R . | jq -s .)

    # ── Build discoveries: what's already been done/tested ────────────────
    # This is the ANTI-CONFABULATION field — things that must not be suggested as pending
    ALREADY_DONE=()
    if [[ -n "$DATA_PATHS_RAW" ]]; then
        ALREADY_DONE+=("Real data files actively used: $(echo "$DATA_PATHS_RAW" | head -5 | tr '\n' ', ' | sed 's/, $//')")
    fi
    if [[ -n "$CMDS_RUN" ]]; then
        ALREADY_DONE+=("Commands executed this session (do NOT suggest re-running):")
        while IFS= read -r cmd; do
            ALREADY_DONE+=("  ran: $cmd")
        done <<< "$(echo "$CMDS_RUN" | tail -8)"
    fi
    if [[ -n "$DONE_LINES" ]]; then
        while IFS= read -r line; do
            ALREADY_DONE+=("$line")
        done <<< "$(echo "$DONE_LINES" | head -5)"
    fi
    DISCOVERIES=$(printf '%s\n' "${ALREADY_DONE[@]}" | grep -v '^\s*$' | _sanitize | jq -R . | jq -s .)

    # ── Build structured snapshot ─────────────────────────────────────────
    # User intent from last meaningful user messages
    INTENT=$(printf '%s\n' "$USER_TEXT" | tail -5 | tr '\n' ' ' | sed 's/[[:space:]]\+/ /g' | head -c 500)

    # Data paths summary
    DATA_SUMMARY=""
    if [[ -n "$DATA_PATHS_RAW" ]]; then
        DATA_SUMMARY="Data in active use: $(echo "$DATA_PATHS_RAW" | head -8 | tr '\n' ', ' | sed 's/, $//')"
    fi

    # Commands summary
    CMD_SUMMARY=""
    if [[ -n "$CMDS_RUN" ]]; then
        CMD_SUMMARY="Already executed: $(echo "$CMDS_RUN" | tail -8 | tr '\n' '; ' | sed 's/; $//' | head -c 600)"
    fi

    # Done summary
    DONE_SUMMARY=""
    if [[ -n "$DONE_LINES" ]]; then
        DONE_SUMMARY="Completed: $(echo "$DONE_LINES" | head -5 | tr '\n' '; ' | sed 's/; $//' | head -c 400)"
    fi

    # Next summary
    NEXT_SUMMARY=""
    if [[ -n "$NEXT_LINES" ]]; then
        NEXT_SUMMARY="Pending: $(echo "$NEXT_LINES" | head -5 | tr '\n' '; ' | sed 's/; $//' | head -c 400)"
    fi

    # Assemble snapshot as structured text (this is what restore hook uses for smart_context query)
    SNAPSHOT_PARTS=()
    [[ -n "$INTENT" ]] && SNAPSHOT_PARTS+=("Goal: $INTENT")
    [[ -n "$DATA_SUMMARY" ]] && SNAPSHOT_PARTS+=("$DATA_SUMMARY")
    [[ -n "$CMD_SUMMARY" ]] && SNAPSHOT_PARTS+=("$CMD_SUMMARY")
    [[ -n "$DONE_SUMMARY" ]] && SNAPSHOT_PARTS+=("$DONE_SUMMARY")
    [[ -n "$NEXT_SUMMARY" ]] && SNAPSHOT_PARTS+=("$NEXT_SUMMARY")

    SNAPSHOT=$(printf '%s\n' "${SNAPSHOT_PARTS[@]}" | head -c 2000)

    # Fallback if snapshot is too short
    if [[ ${#SNAPSHOT} -lt 100 ]]; then
        SNAPSHOT="Context compacted ($TRIGGER). Files: $(echo "$ACTIVE_FILES" | jq -r '.[:5] | join(", ")')"
    fi

    # ── Persist key contextual facts as memories ──────────────────────────
    FACTS_RAW=$(printf '%s\n' "$ASSISTANT_TEXT" | grep -iE \
        '(use [a-z]+ (for|to|via|through)|is the (working|correct|proper|right)|works? (via|through|by|with)|connect (to|through|via)|host[: ][a-z]|server[: ][a-z]|proxy|socks|ssh [^$]|rclone|important:|note:|remember:)' \
        2>/dev/null | grep -v '^\s*$' | head -10)

    ALL_FACTS=$(printf "%s\n" "$FACTS_RAW" | sort -u | grep -v '^\s*$' | head -15)

    if [[ -n "$ALL_FACTS" ]]; then
        FACT_CONTENT=$(printf "[pre-compact:%s] Key context from session\n%s" "$REALM" "$ALL_FACTS")
        FACT_ARGS=$(jq -n \
            --arg category "wisdom" \
            --arg title "Pre-compact context: $SESSION_ID" \
            --arg content "$FACT_CONTENT" \
            --arg realm "$REALM" \
            '{category: $category, title: $title, content: $content, realm: $realm, confidence: 0.85}')
        queue_write "observe" "$FACT_ARGS"
    fi
fi

# ═══════════════════════════════════════════════════════════════════════════
# Check for active TaskList todos
# ═══════════════════════════════════════════════════════════════════════════

# Use jq to safely extract task subjects from any JSON object in the transcript
# that has subject+status fields — avoids manual string interpolation issues.
TODOS=$(jq -sc '[
    .. | objects |
    select(has("subject") and has("status")) |
    select(.status == "pending" or .status == "in_progress") |
    {content: (.subject | tostring), status: .status}
] | unique_by(.content) | .[-5:]' "$TRANSCRIPT_PATH" 2>/dev/null || echo "[]")

# ═══════════════════════════════════════════════════════════════════════════
# Queue comprehensive ledger save
# ═══════════════════════════════════════════════════════════════════════════

# Validate each JSON array; fall back to [] if invalid/empty
_safe_arr() { echo "${1:-[]}" | jq -c 'if type=="array" then . else [] end' 2>/dev/null || echo "[]"; }
_AF=$(  _safe_arr "$ACTIVE_FILES")
_DEC=$( _safe_arr "$DECISIONS")
_TOD=$( _safe_arr "$TODOS")
_BLK=$( _safe_arr "$BLOCKERS")
_DIS=$( _safe_arr "$DISCOVERIES")
_NS=$(  _safe_arr "$NEXT_STEPS")
# Sanitize snapshot: strip control chars, keep under 2000 chars
_SNAP=$(printf '%s' "${SNAPSHOT:-}" | tr -d '\000-\010\013\014\016-\037' | head -c 2000)

LEDGER_ARGS=$(jq -n \
    --arg session_id "$SESSION_ID" \
    --arg project "$REALM" \
    --arg mood "pre-compact" \
    --argjson active_files "$_AF" \
    --argjson decisions "$_DEC" \
    --argjson todos "$_TOD" \
    --argjson blockers "$_BLK" \
    --argjson discoveries "$_DIS" \
    --argjson next_steps "$_NS" \
    --arg snapshot "$_SNAP" \
    '{
        session_id: $session_id,
        project: $project,
        mood: $mood,
        active_files: $active_files,
        decisions: $decisions,
        todos: $todos,
        blockers: $blockers,
        discoveries: $discoveries,
        next_steps: $next_steps,
        snapshot: $snapshot
    }')

if [[ -n "$LEDGER_ARGS" ]]; then
    queue_write "ledger_save" "$LEDGER_ARGS"
else
    echo "[ledger_save] skipped: jq failed to build args (control chars in transcript?)" >&2
fi

# Report what was captured
file_count=$(echo "$ACTIVE_FILES" | jq 'length')
decision_count=$(echo "$DECISIONS" | jq 'length')
discovery_count=$(echo "$DISCOVERIES" | jq 'length')
todo_count=$(echo "$TODOS" | jq 'length')

echo "[checkpoint] $SESSION_ID: files=$file_count decisions=$decision_count discoveries=$discovery_count todos=$todo_count" >&2

# ═══════════════════════════════════════════════════════════════════════════
# Trigger immediate distillation via daemon
# ═══════════════════════════════════════════════════════════════════════════

DISTILL_SESSION_ID="$REAL_SESSION_ID"
if [[ -z "$DISTILL_SESSION_ID" && -n "$TRANSCRIPT_PATH" ]]; then
    DISTILL_SESSION_ID=$(basename "$TRANSCRIPT_PATH" .jsonl)
fi

if [[ -n "$DISTILL_SESSION_ID" && -n "$TRANSCRIPT_PATH" ]]; then
    queue_write "transcript_register" "{\"session_id\":\"$DISTILL_SESSION_ID\",\"transcript_path\":$(echo "$TRANSCRIPT_PATH" | jq -Rs .),\"realm\":\"$REALM\"}"
    queue_write "distill_trigger" "{\"session_id\":\"$DISTILL_SESSION_ID\"}"
    echo "[distill] Triggered for $DISTILL_SESSION_ID" >&2
fi

# ═══════════════════════════════════════════════════════════════════════════
# COMPACT_CONTEXT: Memory-aware turn scoring before compaction
# ═══════════════════════════════════════════════════════════════════════════
if [[ -n "$TRANSCRIPT_PATH" && -f "$TRANSCRIPT_PATH" && -x "$CHITTA_BIN" ]]; then
    MESSAGES_JSON=$(jq -sc '[
        .[] |
        select(.type == "user" or .type == "assistant") |
        {
            role: .type,
            content: (
                if .type == "assistant" then
                    ((.message.content // []) | map(select(.type == "text") | .text) | join(" "))
                else
                    (.message.content // "" | if type == "array" then (map(.text // "") | join(" ")) else tostring end)
                end
            )
        } |
        select((.content | length) > 0)
    ] | .[-60:]' "$TRANSCRIPT_PATH" 2>/dev/null || echo "[]")

    if [[ "$MESSAGES_JSON" != "[]" && "$MESSAGES_JSON" != "null" ]]; then
        COMPACT_RPC=$(jq -n \
            --argjson messages "$MESSAGES_JSON" \
            --arg query "${SNAPSHOT:0:300}" \
            '{"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"compact_context","arguments":{
                "messages": $messages,
                "query": $query,
                "target_ratio": 0.4
            }}}')

        COMPACT_RESULT=$(printf '%s' "$COMPACT_RPC" | \
            timeout 15 "$CHITTA_BIN" 2>/dev/null || true)

        if [[ -n "$COMPACT_RESULT" ]]; then
            before=$(printf '%s' "$COMPACT_RESULT" | jq -r '.result.stats.before_tokens // 0' 2>/dev/null || echo 0)
            after=$(printf '%s' "$COMPACT_RESULT"  | jq -r '.result.stats.after_tokens // 0' 2>/dev/null || echo 0)
            dropped=$(printf '%s' "$COMPACT_RESULT" | jq -r '.result.stats.dropped_pct // 0' 2>/dev/null || echo 0)
            embedded=$(printf '%s' "$COMPACT_RESULT" | jq -r '.result.stats.embedding // false' 2>/dev/null || echo false)

            echo "[compact_context] ${before}→${after} tok | ${dropped}% memory-covered drops | embedding=${embedded}" >&2

            # Only write a real compaction record. A multi-object COMPACT_RESULT
            # makes jq emit one "0" per object ("0\n0\n0…"), which is non-empty
            # and != "0" — the old guard let that through and wrote the
            # "Memory-aware compaction: 0\n0\n0" stubs. Require a single positive
            # integer (collapses the multi-line/non-numeric junk to a reject).
            if [[ "$before" =~ ^[0-9]+$ && "$before" -gt 0 ]]; then
                queue_write "observe" "$(jq -n \
                    --arg realm "$REALM" \
                    --arg sid "${REAL_SESSION_ID:-$SESSION_ID}" \
                    --arg before "$before" \
                    --arg after "$after" \
                    --arg dropped "$dropped" \
                    '{category: "episode",
                      content: ("[pre-compact:\($realm)] Memory-aware compaction: \($before)→\($after) tokens (\($dropped)% dropped as memory-covered). Session: \($sid)"),
                      realm: $realm,
                      confidence: 0.7}')"
            fi
        fi
    fi
fi

exit 0
