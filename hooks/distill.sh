#!/bin/bash
# distill.sh - Extract learnings from conversation using SSL format
#
# Called by chittad with a temp file containing:
#   SESSION_ID=<session>
#   REALM=<realm>
#   MODEL=<model>
#   ---
#   [user]
#   <content>
#   [assistant]
#   <content>
#   ...
#
# Outputs SSL-formatted learnings with typed markers

# Don't use set -e: we want the script to continue even if parts fail

TEMP_FILE="$1"
if [[ -z "$TEMP_FILE" || ! -f "$TEMP_FILE" ]]; then
    echo "[distill] Error: No input file provided" >&2
    exit 0  # Exit gracefully - don't fail the hook
fi

# Parse header
SESSION_ID=$(head -5 "$TEMP_FILE" | grep '^SESSION_ID=' | cut -d= -f2)
REALM=$(head -5 "$TEMP_FILE" | grep '^REALM=' | cut -d= -f2)
MODEL=$(head -5 "$TEMP_FILE" | grep '^MODEL=' | cut -d= -f2)

# Extract conversation (everything after ---)
CONVERSATION=$(sed -n '/^---$/,$ p' "$TEMP_FILE" | tail -n +2)

if [[ -z "$CONVERSATION" ]]; then
    echo "[distill] No conversation content" >&2
    exit 0
fi

CHITTA_BIN="${CHITTA_BIN:-$HOME/.claude/bin/chitta}"

# Smart truncation: preserve head (context) + tail (conclusions)
# Total 80k: 25k head + 55k tail
MAX_CHARS=80000
HEAD_CHARS=25000
TAIL_CHARS=55000

if [[ ${#CONVERSATION} -gt $MAX_CHARS ]]; then
    echo "[distill] Truncating from ${#CONVERSATION} chars (preserving head+tail)"
    head_part="${CONVERSATION:0:$HEAD_CHARS}"
    tail_part="${CONVERSATION: -$TAIL_CHARS}"
    CONVERSATION="${head_part}

[... truncated ${#CONVERSATION} chars, keeping first ${HEAD_CHARS} + last ${TAIL_CHARS} ...]

${tail_part}"
fi

# SSL v0.4 prompt with typed markers, affect, flags, granularity, derivation, and source
PROMPT='Extract learnings from this conversation in SSL v0.4 format.

## SSL v0.4 Format

Two tiers depending on memory type:

### Tier 1: Code-bearing (SOLUTION, GOTCHA, PATTERN)
```
[TYPE] [domain] subject→action→result @location G:N F:FLAG A:v,a <=@ref1,ref2 src:loc →@ref
[ε] exact_command_or_code_verbatim
```

### Tier 2: Narrative (DECISION, PREFERENCE, FAILURE) — denser, no [ε] needed
```
[TYPE] [domain] choice>alternative|reason+context G:N F:FLAG A:v,a <=@ref1,ref2 src:loc →@ref
```

## Types (choose most specific)

| Type | Tier | Use for |
|------|------|---------|
| [SOLUTION] | 1 | What worked: commands, fixes, approaches that succeeded |
| [GOTCHA] | 1 | Traps: counterintuitive behavior, silent failures, edge cases |
| [PATTERN] | 1 | Reusable techniques: approaches that generalize |
| [DECISION] | 2 | Design choices: why X over Y, tradeoffs considered |
| [PREFERENCE] | 2 | User preferences: workflow, style, communication |
| [FAILURE] | 2 | What did not work and why (valuable negative knowledge) |

## SSL Symbols

| Symbol | Meaning | Example |
|--------|---------|---------|
| → | produces/leads to | cmake→build→binary |
| > | chose over (Tier 2) | sqlite>postgres |
| \| | or/alternative/reason | patch\|minor\|major |
| + | with/and | config+flags |
| @ | location | @simple_cli.cpp:720 |
| ! | negation | →!working |
| ? | uncertainty | regulates? |

## Annotations (append to TYPE line, space-separated)

| Annotation | Meaning | Values |
|------------|---------|--------|
| G:N | Granularity tier | 0=atom, 1=episode, 2=claim, 3=operator, 4=boundary |
| F:FLAG | Structural importance | ORIGIN, CORE, PIVOT, GENESIS, TURNING |
| A:v,a | Affect (valence,arousal) | v: -1.0..+1.0, a: 0.0..1.0 |
| <=@refs | Derived from (provenance) | comma-separated memory IDs this abstracted from; required at G:1+ |
| src:loc | External source grounding | file:line, URL slug, paper ID |
| →@ref | Cross-reference | tag name linking to related memory |

### Granularity guide
- G:0 atom — single fact, command, threshold, or name (default when unsure)
- G:1 episode — what happened in a specific session or event
- G:2 claim — abstraction over multiple episodes (use <=@ to cite sources)
- G:3 operator — reusable procedure/pattern distilled from claims (use <=@)
- G:4 boundary — architectural invariant or hard constraint (use <=@)

### Derivation rule
- G:0 atoms: <=@ is optional (no provenance needed for raw facts)
- G:1+ abstract: <=@ is REQUIRED — name the episode/atom IDs this was inferred from

### Flag guide
- ORIGIN: where an idea first appeared
- CORE: foundational to the project/system
- PIVOT: changed direction or approach
- GENESIS: birth of a component/feature
- TURNING: breakthrough moment

### Affect guide
- Positive valence (+): success, satisfaction, relief
- Negative valence (-): frustration, failure, confusion
- High arousal (>0.5): breakthrough, urgent fix, critical discovery
- Low arousal (<0.3): routine, minor preference, background pattern

### Source grounding (src:)
- Use when the learning traces back to a specific external artifact
- File: src:simple_cli.cpp:720
- Doc section: src:CLAUDE.md#build
- Paper/RFC: src:RFC7540#5.1

## Relationships

```
[TRIPLET] subject predicate object
```

Use for: calls, uses, contains, implements, depends_on, derived_from, supersedes

## Rules

1. **Preserve verbatim**: Commands, code, formulas, thresholds go in [ε] lines (Tier 1 only)
2. **Compress prose**: Tier 2 types use dense symbol chains — no [ε] line
3. **Be specific**: Include file paths, line numbers, exact values
4. **No fluff**: Skip obvious/trivial learnings
5. **High signal**: Each learning should be reconstructable from SSL alone
6. **Affect required**: Every learning must have A:v,a — estimate from conversation tone
7. **Granularity required**: Every learning must have G:N — use G:0 for raw facts, G:1+ for abstractions
8. **Derivation required at G:1+**: Abstractions must cite <=@source_ids they generalise from
9. **Flags when applicable**: Add F: only when structurally significant (not every memory)
10. **Cross-ref when related**: Use →@tag to link learnings that reference each other
11. **Source when traceable**: Add src: when the learning has a clear external origin

## Good Examples

### Tier 1 (code-bearing):
```
[SOLUTION] [chitta] parallel-build→4x-faster @cmake G:0 A:+0.6,0.3
[ε] cmake --build build --parallel

[GOTCHA] [daemon] thread_pool→blocks-if-handler-throws @simple_cli.cpp:776 G:0 A:-0.4,0.7 F:CORE src:simple_cli.cpp:776
[ε] wrap handler.handle() in try-catch, return error JSON

[PATTERN] [hooks] fire-and-forget→queue-file→daemon-processes-async G:3 A:+0.3,0.2 <=@parallel-build,thread-pool-fix →@queue-architecture
[ε] echo json >> /tmp/chitta-queue.jsonl
```

### Tier 2 (narrative — dense, no [ε]):
```
[DECISION] [arch] sqlite>postgres|metadata|single-file+no-daemon+<100k G:2 A:+0.5,0.4 F:PIVOT <=@schema-choice-episode
[PREFERENCE] [partnership] no-shortcuts+proper-solutions+no-stubs G:0 A:+0.2,0.1 F:CORE
[FAILURE] [http] http-daemon>unix-socket|200ms-latency+hooks-need-<50ms G:1 A:-0.3,0.6 →@queue-architecture
```

### Triplets:
```
[TRIPLET] ThreadPool contains worker_loop
[TRIPLET] daemon uses ThreadPool
[TRIPLET] health_check bypasses ThreadPool
```

## Bad (avoid)

- Generic summaries without specifics
- Learnings without [ε] when code/commands are involved (Tier 1)
- Obvious things (e.g., "files should be saved")
- Duplicating what is already in code comments
- Missing A: annotations
- Missing G: annotations
- G:1+ without <=@ provenance
- Verbose Tier 2 entries (use > and | instead of prose)
- **[BELIEF] markers** — NEVER emit [BELIEF]. User questions, user instructions, and anything ending in "?" are NOT beliefs — skip them entirely. The [BELIEF] SSL type is a parser artefact; do not generate it.
- Storing raw user questions as any SSL type — filter them out

---

CONVERSATION:
'"$CONVERSATION"'

---

Output ONLY SSL-formatted learnings (no explanations, no markdown headers):'

# Discover GPU endpoint (cached URL files → SLURM → localhost)
ENDPOINT=""
for f in /tmp/ollama-server-*.url; do
    [[ -f "$f" ]] || continue
    url=$(cat "$f" 2>/dev/null | tr -d '\n')
    if curl -sL --max-time 3 "$url/v1/models" 2>/dev/null | grep -q "data"; then
        ENDPOINT="$url"; break
    fi
done
[[ -z "$ENDPOINT" ]] && curl -sL --max-time 3 "http://localhost:11434/v1/models" 2>/dev/null | grep -q "data" && ENDPOINT="http://localhost:11434"

if [[ -z "$ENDPOINT" ]]; then
    echo "[distill] No GPU endpoint — using deterministic SSL fallback" >&2

    # Deterministic fallback: rule-based pattern extraction when no LLM is available.
    # Extracts basic SSL from conversation using regex patterns. Lower quality than
    # LLM distillation but ensures memories are always captured.
    FALLBACK_RESULT=""

    # Pattern: "fixed X" / "fix for X" / "the fix was X"
    while IFS= read -r match; do
        [[ -z "$match" ]] && continue
        clean=$(echo "$match" | sed 's/^[[:space:]]*//' | head -c 200)
        FALLBACK_RESULT+="[SOLUTION] [${REALM}] ${clean} G:0 A:+0.5,0.4"$'\n'
    done <<< "$(echo "$CONVERSATION" | grep -ioP '(?:fixed|fix for|the fix was|resolved by|solved by)\s+\K[^\n.]{10,}' | head -5)"

    # Pattern: "chose X over Y" / "decided to X" / "went with X"
    while IFS= read -r match; do
        [[ -z "$match" ]] && continue
        clean=$(echo "$match" | sed 's/^[[:space:]]*//' | head -c 200)
        FALLBACK_RESULT+="[DECISION] [${REALM}] ${clean} G:0 A:+0.3,0.3"$'\n'
    done <<< "$(echo "$CONVERSATION" | grep -ioP '(?:chose|decided to|went with|picked|selected)\s+\K[^\n.]{10,}' | head -5)"

    # Pattern: "prefer X" / "always X" / "never X"
    while IFS= read -r match; do
        [[ -z "$match" ]] && continue
        clean=$(echo "$match" | sed 's/^[[:space:]]*//' | head -c 200)
        FALLBACK_RESULT+="[PREFERENCE] [partnership] ${clean} G:0 A:+0.2,0.1"$'\n'
    done <<< "$(echo "$CONVERSATION" | grep -ioP '(?:I prefer|always |never |don.t ever)\s*\K[^\n.]{10,}' | head -5)"

    # Pattern: "watch out for X" / "careful with X" / "gotcha:" / "trap:"
    while IFS= read -r match; do
        [[ -z "$match" ]] && continue
        clean=$(echo "$match" | sed 's/^[[:space:]]*//' | head -c 200)
        FALLBACK_RESULT+="[GOTCHA] [${REALM}] ${clean} G:0 A:-0.3,0.5"$'\n'
    done <<< "$(echo "$CONVERSATION" | grep -ioP '(?:watch out for|careful with|gotcha:|trap:|beware of)\s*\K[^\n.]{10,}' | head -5)"

    # Pattern: "failed because" / "didn't work" / "error was"
    while IFS= read -r match; do
        [[ -z "$match" ]] && continue
        clean=$(echo "$match" | sed 's/^[[:space:]]*//' | head -c 200)
        FALLBACK_RESULT+="[FAILURE] [${REALM}] ${clean} G:0 A:-0.4,0.5"$'\n'
    done <<< "$(echo "$CONVERSATION" | grep -ioP '(?:failed because|didn.t work|error was|broke because)\s*\K[^\n.]{10,}' | head -5)"

    if [[ -z "$FALLBACK_RESULT" ]]; then
        echo "[distill] Deterministic fallback: no patterns matched" >&2
        exit 0
    fi

    echo "[distill] Deterministic fallback: extracted patterns" >&2
    RESULT="$FALLBACK_RESULT"

    # Fall through to the same parsing logic below
    # (skip the LLM call section by jumping past it)
    SKIP_LLM=true
fi

# Gate: skip LLM call if fallback already produced results
if [[ "${SKIP_LLM:-false}" != "true" ]]; then

    # Persona injection: select a reasoning persona based on session goal/realm
    SCRIPT_DIR_PRE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
    SESSION_GOAL="${SESSION_ID:-distillation}"
    PERSONA_INJECT=$(bash "${SCRIPT_DIR_PRE}/inject-persona.sh" "$SESSION_GOAL $REALM" 2>/dev/null || echo "")
    DISTILL_SYSTEM_PROMPT="You are a knowledge distiller. Extract learnings in SSL v0.4 format. Output ONLY SSL-formatted learnings with G:N granularity and A:v,a affect annotations. G:1+ memories require <=@provenance."
    if [[ -n "$PERSONA_INJECT" ]]; then
        DISTILL_SYSTEM_PROMPT="${PERSONA_INJECT} ${DISTILL_SYSTEM_PROMPT}"
        echo "[distill] Persona injected: ${PERSONA_INJECT:0:60}..." >&2
    fi

    echo "[distill] Session $SESSION_ID: Calling $MODEL via $ENDPOINT..."

    # Build JSON request
    TMPJSON="/tmp/chitta-distill-$$.json"
    python3 -c "
import json, sys
prompt = sys.stdin.read()
req = {'model': '$MODEL', 'messages': [
    {'role': 'system', 'content': sys.argv[1]},
    {'role': 'user', 'content': prompt}
], 'temperature': 0.3, 'max_tokens': 4096}
json.dump(req, open('$TMPJSON', 'w'), ensure_ascii=True)
" "$DISTILL_SYSTEM_PROMPT" <<< "$PROMPT"

    RESPONSE=$(curl -sL --max-time 180 \
        -H "Content-Type: application/json" \
        -d "@$TMPJSON" \
        "$ENDPOINT/v1/chat/completions" 2>/dev/null || echo "")
    rm -f "$TMPJSON"

    RESULT=$(echo "$RESPONSE" | python3 -c "
import json, sys
try:
    r = json.load(sys.stdin)
    print(r['choices'][0]['message']['content'])
except: pass
" 2>/dev/null || echo "")

    if [[ -z "$RESULT" ]]; then
        echo "[distill] No result from LLM" >&2
        exit 0
    fi
fi  # end SKIP_LLM gate

# Source shared library for parse_ssl_annotations
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${SCRIPT_DIR}/lib.sh" 2>/dev/null || true

echo "[distill] Processing SSL v0.4 results..."

# Get turn range for this session (for hierarchical retrieval)
TURN_RANGE=$("$CHITTA_BIN" sql_query --query "SELECT MIN(turn_index), MAX(turn_index) FROM conversation_turn WHERE session_id = '$SESSION_ID'" 2>/dev/null | grep -E '^[0-9]' || echo "0 0")
START_TURN=$(echo "$TURN_RANGE" | awk -F'|' '{gsub(/[^0-9]/,"",$1); print $1+0}')
END_TURN=$(echo "$TURN_RANGE" | awk -F'|' '{gsub(/[^0-9]/,"",$2); print $2+0}')
[[ -z "$START_TURN" ]] && START_TURN=0
[[ -z "$END_TURN" ]] && END_TURN=0

# Create dialogue episode with turn range (for hierarchical retrieval: SSL → episode → turns)
EPISODE_RESP=$("$CHITTA_BIN" create_episode --session_id "$SESSION_ID" --title "Distillation $(date +%Y-%m-%d)" --start_turn "$START_TURN" --end_turn "$END_TURN" --episode_type "distillation" --realm "$REALM" --json 2>/dev/null || echo "")
EPISODE_ID=$(echo "$EPISODE_RESP" | grep -oP '"episode_id"\s*:\s*\K[0-9]+' || echo "")
[[ -n "$EPISODE_ID" ]] && echo "[distill]   Episode: $EPISODE_ID (turns $START_TURN-$END_TURN)"

# Process markers - combine TYPE line with following [ε] lines
STORED=0
TRIPLETS=0
CURRENT_TYPE=""
CURRENT_CONTENT=""

store_current() {
    [[ -z "$CURRENT_TYPE" || -z "$CURRENT_CONTENT" ]] && return

    local cat
    case "$CURRENT_TYPE" in
        SOLUTION) cat="solution" ;;
        GOTCHA) cat="gotcha" ;;
        DECISION) cat="decision" ;;
        PATTERN) cat="pattern" ;;
        PREFERENCE) cat="preference" ;;
        FAILURE) cat="failure" ;;
        *) cat="wisdom" ;;
    esac

    # Parse SSL v0.4 annotations from first line
    local first_line="${CURRENT_CONTENT%%$'\n'*}"
    parse_ssl_annotations "$first_line"

    # Replace first line with cleaned version (annotations stripped from content)
    if [[ "$CURRENT_CONTENT" == *$'\n'* ]]; then
        CURRENT_CONTENT="${_SSL_CLEAN}"$'\n'"${CURRENT_CONTENT#*$'\n'}"
    else
        CURRENT_CONTENT="${_SSL_CLEAN}"
    fi

    # Extract title (first line, truncated)
    local title="${CURRENT_CONTENT%%$'\n'*}"
    title="${title:0:100}"

    # Build observe args with optional v0.4 fields.
    # Array + direct invocation (no eval): title/content come from LLM-distilled
    # transcript text, which must never reach shell parsing.
    # --source labels the writer: the learning experiment classifies the
    # automatic cohort by provenance triplet, never by kind or text.
    local -a observe_args=(--category "$cat" --title "$title" --content "$CURRENT_CONTENT" --realm "$REALM" --source distillation --json)
    [[ -n "$_SSL_VALENCE" ]] && observe_args+=(--valence "$_SSL_VALENCE")
    [[ -n "$_SSL_AROUSAL" ]] && observe_args+=(--arousal "$_SSL_AROUSAL")
    [[ -n "$_SSL_FLAGS" ]] && observe_args+=(--flags "$_SSL_FLAGS")
    [[ -n "$_SSL_REFS" ]] && observe_args+=(--refs "$_SSL_REFS")

    # Store via observe (creates memory with proper category + affect/flags/refs)
    local resp=$("$CHITTA_BIN" observe "${observe_args[@]}" 2>/dev/null || echo "")

    if echo "$resp" | grep -q '"id"'; then
        local id=$(echo "$resp" | grep -oP '"id"\s*:\s*"\K[^"]+' | head -1)
        local affect_info=""
        [[ -n "$_SSL_VALENCE" ]] && affect_info=" A:${_SSL_VALENCE},${_SSL_AROUSAL}"
        local flag_info=""
        [[ -n "$_SSL_FLAGS" ]] && flag_info=" F:${_SSL_FLAGS}"
        local gran_info=""
        [[ -n "$_SSL_GRANULARITY" ]] && gran_info=" G:${_SSL_GRANULARITY}"
        echo "[distill]   +${CURRENT_TYPE,,}: ${title:0:50}...${affect_info}${flag_info}${gran_info}"
        ((STORED++)) || true

        # Link to episode if we have one
        if [[ -n "$EPISODE_ID" && -n "$id" ]]; then
            "$CHITTA_BIN" connect --subject "$id" --predicate "derived_from" --object "$EPISODE_ID" 2>/dev/null || true
        fi

        # SSL v0.4 triplets: granularity, derivation provenance, source grounding
        if [[ -n "$_SSL_GRANULARITY" ]]; then
            "$CHITTA_BIN" connect --subject "$id" --predicate "granularity" --object "$_SSL_GRANULARITY" 2>/dev/null || true
        fi
        if [[ -n "$_SSL_DERIVATION" ]]; then
            IFS=',' read -ra _drefs <<< "$_SSL_DERIVATION"
            for _dref in "${_drefs[@]}"; do
                [[ -n "$_dref" ]] && "$CHITTA_BIN" connect --subject "$id" --predicate "abstracted_from" --object "$_dref" 2>/dev/null || true
            done
        fi
        if [[ -n "$_SSL_SOURCE" ]]; then
            "$CHITTA_BIN" connect --subject "$id" --predicate "source_loc" --object "$_SSL_SOURCE" 2>/dev/null || true
        fi

        # Forward-bet: emit [prediction] for testable wisdom lines only
        if [[ "$cat" == "wisdom" && -n "$id" ]]; then
            local _is_testable=false
            if [[ -n "$_SSL_VALENCE" && -n "$_SSL_AROUSAL" ]]; then
                python3 -c "import sys; v,a=float('$_SSL_VALENCE'),float('$_SSL_AROUSAL'); sys.exit(0 if v>0 and a>0.4 else 1)" 2>/dev/null && _is_testable=true
            fi
            if [[ "$_is_testable" != "true" ]]; then
                echo "$CURRENT_CONTENT" | grep -qiE '\b(will|should|enables|prevents|improves|reduces|guarantees|ensures)\b' && _is_testable=true
            fi

            if [[ "$_is_testable" == "true" ]]; then
                local _horizon
                _horizon=$(date -d "+30 days" +%Y-%m-%d 2>/dev/null || date -v+30d +%Y-%m-%d 2>/dev/null || echo "")
                if [[ -n "$_horizon" ]]; then
                    local _claim="${title:0:120}"
                    local _pred_content="[prediction] ${_claim} horizon:${_horizon} source_wisdom:${id} status:open"
                    local _pred_resp
                    _pred_resp=$("$CHITTA_BIN" observe \
                        --title "prediction:${title:0:80}" \
                        --content "$_pred_content" \
                        --category "signal" \
                        --tags "prediction,forward-bet,${cat}" \
                        --realm "$REALM" \
                        --json 2>/dev/null || echo "")
                    local _pred_id
                    _pred_id=$(echo "$_pred_resp" | grep -oP '"id"\s*:\s*"\K[^"]+' | head -1)
                    if [[ -n "$_pred_id" ]]; then
                        echo "[distill]     +prediction: ${_claim:0:50}... (horizon:${_horizon})"
                        "$CHITTA_BIN" connect --subject "$_pred_id" --predicate "predicts" --object "$id" 2>/dev/null || true
                    fi
                fi
            fi
        fi
    fi

    CURRENT_TYPE=""
    CURRENT_CONTENT=""
}

# Parse line by line
while IFS= read -r line || [[ -n "$line" ]]; do
    # Skip empty lines and markdown artifacts
    [[ -z "$line" ]] && continue
    [[ "$line" == '```'* ]] && continue
    [[ "$line" == '---' ]] && continue

    # Check for typed marker line
    if [[ "$line" =~ ^\[(SOLUTION|GOTCHA|DECISION|PATTERN|PREFERENCE|FAILURE)\][[:space:]] ]]; then
        # Store previous if any
        store_current
        CURRENT_TYPE="${BASH_REMATCH[1]}"
        CURRENT_CONTENT="${line#\[$CURRENT_TYPE\] }"

    # Check for epsilon (verbatim) line - append to current
    elif [[ "$line" == "[ε]"* && -n "$CURRENT_TYPE" ]]; then
        CURRENT_CONTENT+=$'\n'"$line"

    # Check for triplet relationship
    elif [[ "$line" =~ ^\[TRIPLET\][[:space:]] ]]; then
        store_current
        parts="${line#\[TRIPLET\] }"
        subj=$(echo "$parts" | awk '{print $1}')
        pred=$(echo "$parts" | awk '{print $2}')
        obj=$(echo "$parts" | awk '{$1=$2=""; print $0}' | xargs)

        if [[ -n "$subj" && -n "$pred" && -n "$obj" ]]; then
            if "$CHITTA_BIN" connect --subject "$subj" --predicate "$pred" --object "$obj" 2>/dev/null; then
                echo "[distill]   triplet: $subj→$pred→$obj"
                ((TRIPLETS++)) || true
            fi
        fi

    # Continuation line for current marker (no prefix)
    elif [[ -n "$CURRENT_TYPE" && ! "$line" =~ ^\[ ]]; then
        CURRENT_CONTENT+=$'\n'"$line"
    fi
done <<< "$RESULT"

# Store final marker if pending
store_current

echo "[distill] Session $SESSION_ID: Done (+$STORED learnings, $TRIPLETS triplets)"

exit 0
