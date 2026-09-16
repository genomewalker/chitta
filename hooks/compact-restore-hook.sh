#!/bin/bash
# SessionStart hook for source=compact: Structured context restoration
#
# After compaction, Claude Code re-runs SessionStart hooks. The full session-start-hook.sh
# is expensive (topology queries, corrections, auto-index, realm-retag, behavioral probes).
# Post-compact we restore: ledger state (authoritative) + targeted smart_context (enrichment).
#
# Anti-confabulation: if data paths or executed commands are in the ledger,
# we emit explicit guardrails telling Claude not to suggest redoing them.

CHITTA_BIN="${CHITTA_BIN:-$HOME/.claude/bin/chitta}"
MAX_WAIT="${CHITTA_MAX_WAIT:-${CC_SOUL_MAX_WAIT:-2}}"
MIND_PATH="${CHITTA_DB_PATH:-${HOME}/.claude/mind}"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${SCRIPT_DIR}/lib.sh"

# Parse JSON input
INPUT=$(cat)
SESSION_ID=$(echo "$INPUT" | jq -r '.session_id // empty')
TRANSCRIPT_PATH=$(echo "$INPUT" | jq -r '.transcript_path // empty')

[[ ! -x "$CHITTA_BIN" ]] && exit 0
daemon_available || exit 0

# Detect realm (fast, needed for ledger_load)
# Derive project directory from transcript path (must match pre-compact logic)
PROJECT_DIR=""
if [[ -n "$TRANSCRIPT_PATH" ]]; then
    PROJECT_ENCODED=$(dirname "$TRANSCRIPT_PATH" | xargs basename)
    PROJECT_DIR=$(decode_project_path "$PROJECT_ENCODED")
fi

REALM=$(detect_project_realm "$PROJECT_DIR")

# Re-register session (PID survives compaction but session state needs refresh)
if [[ -n "$SESSION_ID" ]]; then
    CLAUDE_PID=${PPID:-$$}
    timeout "$MAX_WAIT" "$CHITTA_BIN" session_register --session_id "$SESSION_ID" --realm "$REALM" --pid "$CLAUDE_PID" >/dev/null 2>&1 || true
fi

# Re-register transcript
if [[ -n "$TRANSCRIPT_PATH" && -f "$TRANSCRIPT_PATH" ]]; then
    queue_write "transcript_register" "{\"session_id\":\"$SESSION_ID\",\"transcript_path\":$(echo "$TRANSCRIPT_PATH" | jq -Rs .),\"realm\":\"$REALM\"}"
fi

# ═══════════════════════════════════════════════════════════════════════════
# Load ledger state (authoritative truth)
# ═══════════════════════════════════════════���═══════════════════════════════

LEDGER_JSON=$(timeout "$MAX_WAIT" "$CHITTA_BIN" ledger_load --project "$REALM" --json 2>/dev/null || echo "{}")

CONTEXT_PARTS=()

if [[ -n "$LEDGER_JSON" && "$LEDGER_JSON" != "{}" ]]; then
    # ── Resume directive (imperative, high-salience) ───────────────────────
    # Fires FIRST so it isn't buried under state blocks.
    SNAPSHOT=$(echo "$LEDGER_JSON" | jq -r '.snapshot // empty')
    _resume_goal=$(echo "$SNAPSHOT" | grep -m1 '^Goal:' | sed 's/^Goal:[[:space:]]*//' | head -c 200)
    [[ -z "$_resume_goal" ]] && _resume_goal=$(echo "$SNAPSHOT" | head -1 | head -c 200)
    _resume_next=$(echo "$LEDGER_JSON" | jq -r '.next_steps // [] | .[0] // empty' 2>/dev/null | head -c 150)
    _resume_done=$(echo "$LEDGER_JSON" | jq -r '.discoveries // [] | .[0] // empty' 2>/dev/null | head -c 100)

    if [[ -n "$_resume_goal" || -n "$_resume_next" ]]; then
        _directive="[resume-directive] Session resumed after compaction."
        _directive="${_directive} Start your FIRST reply with one sentence: \""
        [[ -n "$_resume_goal" ]] && _directive="${_directive}We were working on: ${_resume_goal}."
        [[ -n "$_resume_done" ]] && _directive="${_directive} Done: ${_resume_done}."
        [[ -n "$_resume_next" ]] && _directive="${_directive} Next: ${_resume_next}."
        _directive="${_directive}\" Then continue from that next step immediately."
        _directive="${_directive} If the user's message clearly starts a new topic, skip the recap. [/resume-directive]"
        CONTEXT_PARTS+=("$_directive")
    fi

    # ── Structured state restoration ──────────────────────────────────────
    RESTORE_TEXT="[session-state]"

    if [[ -n "$SNAPSHOT" && ${#SNAPSHOT} -gt 20 ]]; then
        RESTORE_TEXT="${RESTORE_TEXT}\n${SNAPSHOT:0:1500}"
    fi

    # Active files (data paths are highest priority)
    ACTIVE_FILES=$(echo "$LEDGER_JSON" | jq -r '.active_files // [] | .[:15] | .[]' 2>/dev/null)
    if [[ -n "$ACTIVE_FILES" ]]; then
        RESTORE_TEXT="${RESTORE_TEXT}\nActive files: $(echo "$ACTIVE_FILES" | tr '\n' ', ' | sed 's/, $//')"
    fi

    # Decisions
    DECISIONS=$(echo "$LEDGER_JSON" | jq -r '.decisions // [] | .[:5] | .[]' 2>/dev/null)
    if [[ -n "$DECISIONS" ]]; then
        RESTORE_TEXT="${RESTORE_TEXT}\nDecisions: $(echo "$DECISIONS" | tr '\n' '; ' | sed 's/; $//')"
    fi

    # Next steps
    NEXT=$(echo "$LEDGER_JSON" | jq -r '.next_steps // [] | .[:5] | .[]' 2>/dev/null)
    if [[ -n "$NEXT" ]]; then
        RESTORE_TEXT="${RESTORE_TEXT}\nNext steps: $(echo "$NEXT" | tr '\n' '; ' | sed 's/; $//')"
    fi

    # Blockers
    BLOCKERS=$(echo "$LEDGER_JSON" | jq -r '.blockers // [] | .[:3] | .[]' 2>/dev/null)
    if [[ -n "$BLOCKERS" ]]; then
        RESTORE_TEXT="${RESTORE_TEXT}\nBlockers: $(echo "$BLOCKERS" | tr '\n' '; ')"
    fi

    # Todos
    TODOS=$(echo "$LEDGER_JSON" | jq -r '.todos // [] | .[] | "[\(.status)] \(.content)"' 2>/dev/null)
    if [[ -n "$TODOS" ]]; then
        RESTORE_TEXT="${RESTORE_TEXT}\nTasks: $(echo "$TODOS" | head -5 | tr '\n' '; ' | sed 's/; $//')"
    fi

    RESTORE_TEXT="${RESTORE_TEXT}\n[/session-state]"
    CONTEXT_PARTS+=("$RESTORE_TEXT")

    # ── Anti-confabulation guardrails ─────────────────────────────────────
    # The discoveries field contains "already done" items from pre-compact
    DISCOVERIES=$(echo "$LEDGER_JSON" | jq -r '.discoveries // [] | .[]' 2>/dev/null)
    HAS_DATA_PATHS=$(echo "$LEDGER_JSON" | jq -r '.active_files // [] | map(select(test("\\.(bam|cram|sam|vcf|bcf|fastq|mcaf|ktax|tsv|csv|parquet)$"; "i"))) | length' 2>/dev/null)

    GUARDRAILS=""
    if [[ -n "$DISCOVERIES" || "${HAS_DATA_PATHS:-0}" -gt 0 ]]; then
        GUARDRAILS="[anti-confab] MANDATORY RULES FOR THIS RESUMED SESSION:"
        GUARDRAILS="${GUARDRAILS}\n1. [session-state] above is AUTHORITATIVE. Do not contradict it."
        GUARDRAILS="${GUARDRAILS}\n2. Never propose as pending anything listed under 'Already done' or 'Already executed'."
        GUARDRAILS="${GUARDRAILS}\n3. If uncertain, ask — do not guess or confabulate session history."

        if [[ "${HAS_DATA_PATHS:-0}" -gt 0 ]]; then
            GUARDRAILS="${GUARDRAILS}\n4. FORBIDDEN CLAIM: 'ready to test on real data' / 'when you have files' — real data IS present and WAS used."
        fi

        if [[ -n "$DISCOVERIES" ]]; then
            GUARDRAILS="${GUARDRAILS}\nAlready done:"
            # Compact format: one line per item
            while IFS= read -r line; do
                [[ -n "$line" ]] && GUARDRAILS="${GUARDRAILS}\n- $line"
            done <<< "$(echo "$DISCOVERIES" | head -10)"
        fi

        GUARDRAILS="${GUARDRAILS}\n[/anti-confab]"
        CONTEXT_PARTS+=("$GUARDRAILS")
    fi
fi

# ═════════════════════════════════════════���═════════════════════════════════
# Targeted smart_context enrichment (secondary — not truth source)
# ═══════════════════════════════════════════════════════════════════════════
# Use the actual snapshot content as the query, not a generic string

# Multi-facet parallel smart_context queries using actual session content
SC_TMP=$(mktemp -d)

# Extract query seeds from ledger
SC_GOAL=$(echo "$SNAPSHOT" | grep -m1 '^Goal:' | head -c 300)
SC_DATA=$(echo "$LEDGER_JSON" | jq -r '.active_files // [] | .[:5] | join(", ")' 2>/dev/null)
SC_DECISIONS=$(echo "$LEDGER_JSON" | jq -r '.decisions // [] | .[:3] | join("; ")' 2>/dev/null)

# Run parallel facet queries (each capped at MAX_WAIT)
if [[ -n "$SC_GOAL" ]]; then
    timeout "$MAX_WAIT" "$CHITTA_BIN" smart_context --task "$SC_GOAL" --mode fast --limit 200 \
        2>/dev/null > "$SC_TMP/goal.txt" &
fi
if [[ -n "$SC_DATA" ]]; then
    timeout "$MAX_WAIT" "$CHITTA_BIN" smart_context --task "data artifacts: $SC_DATA" --mode fast --limit 200 \
        2>/dev/null > "$SC_TMP/data.txt" &
fi
if [[ -n "$SC_DECISIONS" ]]; then
    timeout "$MAX_WAIT" "$CHITTA_BIN" smart_context --task "decisions: $SC_DECISIONS" --mode fast --limit 200 \
        2>/dev/null > "$SC_TMP/decisions.txt" &
fi
# Fallback: if no seeds, use realm-level query
if [[ -z "$SC_GOAL" && -z "$SC_DATA" ]]; then
    timeout "$MAX_WAIT" "$CHITTA_BIN" smart_context --task "session continuation: $REALM project" --mode fast --limit 300 \
        2>/dev/null > "$SC_TMP/fallback.txt" &
fi
wait

# Merge, dedupe, cap
SMART_CTX=$(cat "$SC_TMP"/*.txt 2>/dev/null | sed '/^\s*$/d' | sed '/No memories/d' | awk '!seen[$0]++' | head -25 | head -c 800)
rm -rf "$SC_TMP"

if [[ -n "$SMART_CTX" ]]; then
    CONTEXT_PARTS+=("[soul-context]\n${SMART_CTX}\n[/soul-context]")
fi

# ═══════════════════════════════════════════════════════════════════════════
# Subagent budget carry-forward
# ═════════════════════════════════════════════��═════════════════════════════
if [[ -n "$SESSION_ID" ]]; then
    AGENT_COUNT_FILE="$MIND_PATH/.subagent_count_${SESSION_ID}"
    AGENT_COUNT=$(cat "$AGENT_COUNT_FILE" 2>/dev/null || echo 0)
    if [[ $AGENT_COUNT -gt 15 ]]; then
        CONTEXT_PARTS+=("[token-budget] ${AGENT_COUNT} subagents spawned pre-compact. Consider batching remaining work or starting fresh with /recap.")
    fi
fi

# ═══════════════════════════════════════════════════════════════════════════
# Emit as JSON hookSpecificOutput for SessionStart
# ═══════════════════════════════════════════════════════════════════════════
if [[ ${#CONTEXT_PARTS[@]} -gt 0 ]]; then
    FULL_CONTEXT=""
    for part in "${CONTEXT_PARTS[@]}"; do
        FULL_CONTEXT="${FULL_CONTEXT}${part}\n"
    done

    # Build watchPaths for key project files
    WATCH_PATHS="[]"
    if [[ -n "$PROJECT_DIR" && -d "$PROJECT_DIR" ]]; then
        PATHS_ARRAY="["
        FIRST=true
        for f in Snakefile Nextfile pyproject.toml Cargo.toml CMakeLists.txt package.json go.mod; do
            if [[ -f "$PROJECT_DIR/$f" ]]; then
                $FIRST && FIRST=false || PATHS_ARRAY+=","
                PATHS_ARRAY+="\"$PROJECT_DIR/$f\""
            fi
        done
        PATHS_ARRAY+="]"
        WATCH_PATHS="$PATHS_ARRAY"
    fi

    printf '%s' "$(jq -n \
        --arg ctx "$(printf '%b' "$FULL_CONTEXT")" \
        --argjson wp "$WATCH_PATHS" \
        '{
            hookSpecificOutput: {
                hookEventName: "SessionStart",
                additionalContext: $ctx,
                watchPaths: $wp
            }
        }')"
else
    printf '%s' '{"hookSpecificOutput":{"hookEventName":"SessionStart"}}'
fi

exit 0
