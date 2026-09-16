#!/bin/bash
# bridge-holes.sh — G9: structural hole bridging
#
# Scans the memory graph for nodes with low connectivity (structural holes,
# degree < 2) and emits [bridge] wisdom memories that link isolated nodes
# from different realms via an LLM-suggested insight.
#
# Graph model: triplet entities (subject/object strings) are the nodes; an
# edge is any triplet in which the entity appears as subject or object.
# Degree = in-degree + out-degree. chitta has no global triplet dump, so the
# node set is harvested from memory content (`list_memories_brief`) and each
# candidate's degree is measured with `query_graph`.
#
# Usage: bridge-holes.sh [--dry-run] [--realm <r>] [--max <n>]

set -euo pipefail

CHITTA_BIN="${CHITTA_BIN:-$HOME/.claude/bin/chitta}"
DRY_RUN=0
REALM_FILTER=""
MAX_BRIDGES=5

while [[ $# -gt 0 ]]; do
    case "$1" in
        --dry-run) DRY_RUN=1; shift ;;
        --realm)   REALM_FILTER="$2"; shift 2 ;;
        --max)     MAX_BRIDGES="$2"; shift 2 ;;
        *) echo "Unknown flag: $1" >&2; exit 1 ;;
    esac
done

log() { echo "[$(date -Iseconds)] [bridge-holes] $*" >&2; }

# ---------------------------------------------------------------------------
# 1. Harvest candidate nodes (entity, realm, text) from memory content.
#    `list_memories_brief` emits one JSON object per line (JSONL); the --json
#    wrapper form is unreliable at high limits, so consume the JSONL stream.
#    Triplet-shaped content "[tag] A→B" yields two entities; other memories
#    contribute their content as a single entity.
# ---------------------------------------------------------------------------
BRIEF=$("$CHITTA_BIN" list_memories_brief --limit 500 ${REALM_FILTER:+--realm "$REALM_FILTER"} 2>/dev/null)

if [[ -z "$BRIEF" ]] || ! echo "$BRIEF" | jq -e . >/dev/null 2>&1; then
    log "no memories returned, skipping"
    exit 0
fi

# Emit TSV: entity \t realm \t text  (one row per candidate node).
# Normalize both shapes the daemon emits: a single JSON array (high limits)
# or one object per line (low limits).
NODES_TSV=$(echo "$BRIEF" | jq -r '
    (if type=="array" then .[] else . end)
    | . as $m
    | (.content // "") as $c
    # Split arrow-shaped triplet content into endpoint entities; otherwise
    # treat the whole content as one entity.
    | if ($c | test("→|->")) then
        ($c | gsub("^\\[[^\\]]*\\]\\s*"; "") | split("→|->"; "x"))
        | map(gsub("^\\s+|\\s+$"; "")) | map(select(length > 0))
        | .[]
      else
        $c
      end
    | gsub("\\n.*$"; "")        # first line only
    | gsub("^\\s+|\\s+$"; "")
    | select(length > 2)
    | [ ., $m.realm, ($c | gsub("\n"; " ")) ]
    | @tsv
' | awk -F'\t' '!seen[$1]++')   # dedup by entity

if [[ -z "$NODES_TSV" ]]; then
    log "no candidate nodes harvested, skipping"
    exit 0
fi

# ---------------------------------------------------------------------------
# 2. Measure degree per node and collect structural holes (degree < 2).
#    degree = #triplets(subject=entity) + #triplets(object=entity)
# ---------------------------------------------------------------------------
degree_of() {
    local entity="$1" d=0 n
    n=$("$CHITTA_BIN" query_graph --subject "$entity" --json 2>/dev/null \
        | jq '(.triplets // []) | length' 2>/dev/null || echo 0)
    d=$((d + ${n:-0}))
    n=$("$CHITTA_BIN" query_graph --object "$entity" --json 2>/dev/null \
        | jq '(.triplets // []) | length' 2>/dev/null || echo 0)
    d=$((d + ${n:-0}))
    echo "$d"
}

# Each node costs 2 RPCs; cap the scan so the hook stays fast. Stop early once
# we have a comfortable margin of holes (gate=10) plus room for pairing.
MAX_SCAN="${MAX_SCAN:-150}"
HOLE_TARGET=$(( 10 + MAX_BRIDGES * 2 ))

# HOLES_TSV rows: entity \t realm \t text  (only isolated nodes)
HOLES_TSV=""
scanned=0
found=0
while IFS=$'\t' read -r entity realm text; do
    [[ -z "$entity" ]] && continue
    [[ "$scanned" -ge "$MAX_SCAN" ]] && break
    [[ "$found" -ge "$HOLE_TARGET" ]] && break
    scanned=$((scanned + 1))
    deg=$(degree_of "$entity")
    if [[ "$deg" -lt 2 ]]; then
        HOLES_TSV+="${entity}	${realm}	${text}"$'\n'
        found=$((found + 1))
    fi
done <<< "$NODES_TSV"
log "scanned $scanned nodes (cap $MAX_SCAN)"

HOLES_TSV=$(printf '%s' "$HOLES_TSV" | sed '/^$/d')
N_HOLES=$(printf '%s\n' "$HOLES_TSV" | sed '/^$/d' | wc -l | tr -d ' ')
log "found $N_HOLES structural holes (degree < 2)"

# ---------------------------------------------------------------------------
# 3. Gate: require at least 10 isolated nodes.
# ---------------------------------------------------------------------------
if [[ "$N_HOLES" -lt 10 ]]; then
    log "fewer than 10 holes ($N_HOLES), gate not met, skipping"
    exit 0
fi

# ---------------------------------------------------------------------------
# 4. Discover LLM endpoint (cached GPU URL files → localhost ollama).
# ---------------------------------------------------------------------------
ENDPOINT=""
for f in /tmp/ollama-server-*.url; do
    [[ -f "$f" ]] || continue
    url=$(tr -d '\n' < "$f" 2>/dev/null)
    if curl -sL --max-time 3 "$url/v1/models" 2>/dev/null | grep -q "data"; then
        ENDPOINT="$url"; break
    fi
done
[[ -z "$ENDPOINT" ]] && curl -sL --max-time 3 "http://localhost:11434/v1/models" 2>/dev/null | grep -q "data" && ENDPOINT="http://localhost:11434"

if [[ -z "$ENDPOINT" ]]; then
    log "no LLM endpoint available, skipping"
    exit 0
fi
MODEL="${MODEL:-$(curl -sL --max-time 3 "$ENDPOINT/v1/models" 2>/dev/null | jq -r '.data[0].id // empty' 2>/dev/null)}"
[[ -z "$MODEL" ]] && { log "no model available at $ENDPOINT, skipping"; exit 0; }
log "endpoint=$ENDPOINT model=$MODEL"

BRIDGE_SYS_PROMPT='You connect ideas across domains. Given two isolated memories, name a one-or-two-word THEME they share and a single concise INSIGHT (one sentence) that bridges them. Reply on a single line in exactly this form: THEME|||INSIGHT'

bridge_insight() {
    # $1=text A  $2=text B  → prints "THEME|||INSIGHT" or empty on failure.
    # Build the request via jq (no temp file), and extract the THEME|||INSIGHT
    # line from content OR reasoning (reasoning models leave content empty).
    local ta="$1" tb="$2" req resp
    req=$(jq -nc \
        --arg model "$MODEL" --arg sys "$BRIDGE_SYS_PROMPT" \
        --arg a "$ta" --arg b "$tb" \
        '{model:$model,
          messages:[{role:"system",content:$sys},
                    {role:"user",content:("Memory A:\n"+$a+"\n\nMemory B:\n"+$b)}],
          temperature:0.4, max_tokens:512}') || return 1

    resp=$(curl -sL --max-time 90 -H "Content-Type: application/json" \
        --data-binary "$req" "$ENDPOINT/v1/chat/completions" 2>/dev/null || echo "")
    [[ -z "$resp" ]] && return 1

    echo "$resp" | jq -r '
        (.choices[0].message.content // "") as $c
        | (.choices[0].message.reasoning // "") as $r
        | (if ($c | test("\\|\\|\\|")) then $c else $r end)
        | split("\n")[]
        | select(test("\\|\\|\\|"))
    ' 2>/dev/null | head -1 | tr -d '\r'
}

# ---------------------------------------------------------------------------
# 5. Pair isolated nodes from DIFFERENT realms; emit up to MAX_BRIDGES bridges.
# ---------------------------------------------------------------------------
mapfile -t HOLES < <(printf '%s\n' "$HOLES_TSV")

EMITTED=0
declare -A USED   # entity → 1 once bridged, so each hole is used at most once

for ((i=0; i<${#HOLES[@]}; i++)); do
    [[ "$EMITTED" -ge "$MAX_BRIDGES" ]] && break
    IFS=$'\t' read -r ea ra txa <<< "${HOLES[$i]}"
    [[ -z "$ea" || -n "${USED[$ea]:-}" ]] && continue

    for ((j=i+1; j<${#HOLES[@]}; j++)); do
        [[ "$EMITTED" -ge "$MAX_BRIDGES" ]] && break
        IFS=$'\t' read -r eb rb txb <<< "${HOLES[$j]}"
        [[ -z "$eb" || -n "${USED[$eb]:-}" ]] && continue
        [[ "$ra" == "$rb" ]] && continue   # different realms only

        result=$(bridge_insight "$txa" "$txb" || echo "")
        [[ -z "$result" ]] && { log "no insight for ($ea | $eb), skipping pair"; continue; }

        THEME="${result%%|||*}"
        INSIGHT="${result#*|||}"
        # Strip leading markdown bullets/quotes/whitespace the model may prepend,
        # then collapse THEME spaces to hyphens so the [bridge] line stays parseable.
        THEME=$(echo "$THEME" | sed 's/^[[:space:][:punct:]]*//;s/[[:space:]"'\''*]*$//' | tr -s ' ' '-')
        INSIGHT=$(echo "$INSIGHT" | sed 's/^[[:space:]"]*//;s/[[:space:]"]*$//')
        [[ -z "$THEME" || -z "$INSIGHT" ]] && continue

        CONTENT="[bridge] source:${ea} target:${eb} theme:${THEME} insight:${INSIGHT}"

        if [[ "$DRY_RUN" -eq 1 ]]; then
            echo "$CONTENT"
        else
            "$CHITTA_BIN" remember \
                --content "$CONTENT" \
                --type wisdom \
                --tags "bridge,structural-hole" \
                --realm brahman \
                --visibility 1 >/dev/null 2>&1
            log "bridged ${ra}:${ea} ↔ ${rb}:${eb} theme=$THEME"
        fi

        USED[$ea]=1; USED[$eb]=1
        EMITTED=$((EMITTED + 1))
        break   # move to next source node
    done
done

log "emitted $EMITTED bridge(s) (max=$MAX_BRIDGES, dry_run=$DRY_RUN)"
