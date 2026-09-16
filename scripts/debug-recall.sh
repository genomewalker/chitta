#!/bin/bash
# debug-recall.sh — expose the full recall pipeline for a query.
#
# Shows: per-realm candidate pool (over-fetch), stratification caps,
# what got included vs. dropped, and side-by-side realm breakdown.
#
# Usage:
#   debug-recall.sh "query text" [--limit 10] [--fetch 50]
#
# Output sections:
#   [OVER-FETCH]  raw candidates before stratification (fetch_k results)
#   [PER-REALM]   per-realm counts in over-fetch vs. final result
#   [RESULT]      final k results (same as chitta recall would return)
#   [DROPPED]     candidates present in over-fetch but absent from result

set -euo pipefail

CHITTA="${CHITTA_BIN:-$HOME/.claude/bin/chitta}"
QUERY="${1:-}"
LIMIT=10
FETCH=50

[[ -z "$QUERY" ]] && { echo "Usage: debug-recall.sh \"query\" [--limit N] [--fetch N]"; exit 1; }

shift || true
while [[ $# -gt 0 ]]; do
    case "$1" in
        --limit) LIMIT="$2"; shift 2 ;;
        --fetch) FETCH="$2"; shift 2 ;;
        *) shift ;;
    esac
done

BOLD=$'\033[1m'; RESET=$'\033[0m'; CYAN=$'\033[36m'; YELLOW=$'\033[33m'; RED=$'\033[31m'; GREEN=$'\033[32m'; DIM=$'\033[2m'

echo "${BOLD}=== debug-recall: ${QUERY} (limit=${LIMIT}, fetch=${FETCH}) ===${RESET}"
echo

# ── Over-fetch: get a large pool to see what the index returns ──
TMPDIR_DR=$(mktemp -d)
trap 'rm -rf "$TMPDIR_DR"' EXIT

SQZ_NO_DEDUP=1 "$CHITTA" recall --query "$QUERY" --limit "$FETCH" --json 2>/dev/null > "$TMPDIR_DR/over.json" || true
SQZ_NO_DEDUP=1 "$CHITTA" recall --query "$QUERY" --limit "$LIMIT" --json 2>/dev/null > "$TMPDIR_DR/result.json" || true

python3 - "$QUERY" "$LIMIT" "$FETCH" "$TMPDIR_DR" <<'PYEOF'
import sys, json
from collections import defaultdict, Counter

query, limit, fetch, tmpdir = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), sys.argv[4]

def load(name):
    try:
        d = json.loads(open(f"{tmpdir}/{name}").read())
        return d.get('results', []) if d else []
    except Exception:
        return []

over = load("over.json")
result = load("result.json")

BOLD='\033[1m'; RESET='\033[0m'; CYAN='\033[36m'; YELLOW='\033[33m'
RED='\033[31m'; GREEN='\033[32m'; DIM='\033[2m'


result_ids = {r['id'] for r in result}

# ── Section 1: Over-fetch pool ──
print(f"{BOLD}[OVER-FETCH] {len(over)} candidates (fetch_k={fetch}){RESET}")
realm_pool = defaultdict(list)
for r in over:
    realm_pool[r.get('realm','?')].append(r)

# Per-realm breakdown
realm_result_counts = Counter(r.get('realm','?') for r in result)
print(f"\n{'realm':<30} {'pool':>5} {'result':>7} {'cap':>5}  top candidate")
print('─'*85)
for realm, candidates in sorted(realm_pool.items(), key=lambda x: -len(x)):
    in_result = realm_result_counts.get(realm, 0)
    top = candidates[0]['text'][:50].replace('\n',' ') if candidates else ''
    # Infer cap: highest index in result from this realm
    cap_str = f"{in_result}/{len(candidates)}"
    color = GREEN if in_result > 0 else RED
    print(f"{color}{realm:<30}{RESET} {len(candidates):>5} {in_result:>7}   {cap_str:>5}  {DIM}{top}{RESET}")

# ── Section 2: Final result ──
print(f"\n{BOLD}[RESULT] top {limit} returned to caller{RESET}")
for i, r in enumerate(result, 1):
    rel = r.get('relevance', r.get('similarity', 0))
    text = r['text'][:100].replace('\n',' ')
    realm = r.get('realm','?')[:20]
    rtype = r.get('type','?')[:10]
    print(f"  {i:2}. {rel:.3f} [{rtype:10}] {CYAN}{realm:20}{RESET}  {text}")

# ── Section 3: Dropped ──
dropped = [r for r in over if r['id'] not in result_ids]
if dropped:
    print(f"\n{BOLD}[DROPPED] {len(dropped)} candidates not in result{RESET}")
    # Show top-10 by relevance that were dropped
    dropped_sorted = sorted(dropped, key=lambda r: r.get('relevance', r.get('similarity',0)), reverse=True)
    for r in dropped_sorted[:10]:
        rel = r.get('relevance', r.get('similarity', 0))
        text = r['text'][:100].replace('\n',' ')
        realm = r.get('realm','?')[:20]
        rtype = r.get('type','?')[:10]
        print(f"  {RED}DROP{RESET} {rel:.3f} [{rtype:10}] {CYAN}{realm:20}{RESET}  {text}")

# ── Section 4: Diagnosis ──
print(f"\n{BOLD}[DIAGNOSIS]{RESET}")
# Check if result is dominated by one realm
if result:
    top_realm = Counter(r.get('realm','?') for r in result).most_common(1)[0]
    if top_realm[1] / len(result) > 0.5:
        print(f"  {YELLOW}⚠ realm flood: '{top_realm[0]}' holds {top_realm[1]}/{len(result)} result slots{RESET}")
    else:
        print(f"  {GREEN}✓ realm diversity ok{RESET}")

# Check if dropped items had higher relevance than included items
if dropped and result:
    best_dropped = max(r.get('relevance', r.get('similarity',0)) for r in dropped)
    worst_result = min(r.get('relevance', r.get('similarity',0)) for r in result)
    if best_dropped > worst_result:
        print(f"  {YELLOW}⚠ stratification cut: best dropped ({best_dropped:.3f}) > worst kept ({worst_result:.3f}) — realm cap is active{RESET}")
    else:
        print(f"  {GREEN}✓ no high-relevance items cut by stratification{RESET}")

# Check for stub flood (very short memories dominating)
if result:
    short_count = sum(1 for r in result if len(r.get('text','')) < 60)
    if short_count > len(result) // 2:
        print(f"  {YELLOW}⚠ stub flood: {short_count}/{len(result)} results are short (<60 chars) — low-quality keyword matches dominating{RESET}")
    else:
        print(f"  {GREEN}✓ result quality ok (few stubs){RESET}")

PYEOF
