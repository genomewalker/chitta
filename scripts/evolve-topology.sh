#!/bin/bash
# evolve-topology.sh — G10: conductor room topology evolution
# Mutates conductor room visibility matrices based on QD archive fitness.
# Guards against instability (no fully-blind agents, requires n_evals >= 3).
#
# Usage: evolve-topology.sh [--dry-run] [--realm <realm>]

set -euo pipefail

CHITTA_BIN="${CHITTA_BIN:-$HOME/.claude/bin/chitta}"
DRY_RUN=0
REALM="${REALM:-brahman}"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --dry-run) DRY_RUN=1; shift ;;
        --realm)   REALM="$2"; shift 2 ;;
        *) echo "Unknown flag: $1" >&2; exit 1 ;;
    esac
done

log() { echo "[$(date -Iseconds)] [evolve-topology] $*"; }

# ---------------------------------------------------------------------------
# 1. GATE CHECK — abort if ledger is thin
# ---------------------------------------------------------------------------
COUNT=$("$CHITTA_BIN" recall --query "[run-ledger]" --type signal --limit 20 --json 2>/dev/null \
    | jq '[.results[] | select(.text | startswith("[run-ledger]"))] | length')

if [ "$COUNT" -lt 5 ]; then
    log "ledger too thin ($COUNT entries), skipping"
    exit 0
fi
log "ledger ok ($COUNT entries)"

# ---------------------------------------------------------------------------
# 2. Load best genome from QD archive
# ---------------------------------------------------------------------------
BEST_RAW=$("$CHITTA_BIN" recall --query "[genome] conductor topology" --type process --limit 5 --json 2>/dev/null)

# Pick the result with the highest n_evals that has a parseable visibility matrix
BEST_GENOME=$(echo "$BEST_RAW" | jq -r '
    .results
    | map(select(.text | test("\\[genome\\]")))
    | sort_by(
        (.text | capture("n_evals:(?<n>[0-9]+)") | .n // "0" | tonumber)
      )
    | reverse
    | .[0].text // ""
')

if [ -z "$BEST_GENOME" ]; then
    log "no genome found in QD archive, seeding default"
    # Default seed genome: 3 roles, full visibility
    BEST_GENOME='[genome] conductor topology {"kind":"process","version":1,"n_evals":0,"roles":["synthesizer","critic","integrator"],"visibility":{"synthesizer":["critic","integrator"],"critic":["synthesizer","integrator"],"integrator":["synthesizer","critic"]}}'
fi

# Extract JSON portion (everything after first '{')
GENOME_JSON=$(echo "$BEST_GENOME" | sed 's/^[^{]*//')

if ! echo "$GENOME_JSON" | jq . >/dev/null 2>&1; then
    log "genome JSON unparseable, seeding default"
    GENOME_JSON='{"kind":"process","version":1,"n_evals":0,"roles":["synthesizer","critic","integrator"],"visibility":{"synthesizer":["critic","integrator"],"critic":["synthesizer","integrator"],"integrator":["synthesizer","critic"]}}'
fi

N_EVALS=$(echo "$GENOME_JSON" | jq '.n_evals // 0')
log "best genome n_evals=$N_EVALS"

# ---------------------------------------------------------------------------
# 3. Stability guard — require n_evals >= 3
# ---------------------------------------------------------------------------
if [ "$N_EVALS" -lt 3 ]; then
    log "genome n_evals=$N_EVALS < 3, stability guard triggered, skipping mutation"
    exit 0
fi

# ---------------------------------------------------------------------------
# 4. Mutation operator — edge-flip
# ---------------------------------------------------------------------------
# The genome travels in the environment, not on stdin: `python3 -` already reads
# its program from stdin, so the heredoc wins and a pipe would be swallowed.
MUTATED_JSON=$(GENOME_JSON="$GENOME_JSON" python3 - <<'PYEOF'
import json, os, random, sys

data = json.loads(os.environ["GENOME_JSON"])
vis = data.get("visibility", {})
roles = data.get("roles", list(vis.keys()))

if not vis or len(roles) < 2:
    print(json.dumps(data))
    sys.exit(0)

# Collect all current edges (directed: A can see B)
edges = [(a, b) for a, bs in vis.items() for b in bs if b != a]

# Also collect absent edges (candidates for addition)
all_pairs = [(a, b) for a in roles for b in roles if a != b]
absent_edges = [p for p in all_pairs if p not in edges]

# Decide: flip a present edge (remove) or add an absent one
# Probability 0.3 → flip present edge; otherwise add absent edge
mutated = False
if edges and random.random() < 0.3:
    a, b = random.choice(edges)
    # Constraint: a must keep >= 1 outgoing edge
    outgoing = [x for x in edges if x[0] == a]
    if len(outgoing) > 1:
        vis[a] = [x for x in vis[a] if x != b]
        mutated = True
        print(f"MUTATION remove-edge {a}->{b}", file=sys.stderr)

if not mutated and absent_edges:
    a, b = random.choice(absent_edges)
    if a not in vis:
        vis[a] = []
    if b not in vis[a]:
        vis[a].append(b)
    mutated = True
    print(f"MUTATION add-edge {a}->{b}", file=sys.stderr)

if not mutated:
    print("MUTATION no-op (no valid move)", file=sys.stderr)

data["visibility"] = vis
data["n_evals"] = 0  # reset for new candidate
data["parent_n_evals"] = int(data.get("n_evals_snapshot", data.get("n_evals", 0)))
print(json.dumps(data))
PYEOF
)

log "mutation complete"

if [ "$DRY_RUN" -eq 1 ]; then
    log "[dry-run] would store mutant:"
    echo "$MUTATED_JSON" | jq .
    exit 0
fi

# ---------------------------------------------------------------------------
# 5. Store mutant as new process memory (archive competition selects winner)
# ---------------------------------------------------------------------------
CONTENT="[genome] conductor topology $MUTATED_JSON"

"$CHITTA_BIN" remember \
    --content "$CONTENT" \
    --type process \
    --tags "process,genome,candidate,topology-mutation" \
    --realm "$REALM" \
    --visibility 0

log "mutant stored (realm=$REALM)"
