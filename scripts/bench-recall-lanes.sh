#!/bin/bash
# Paired prompt-hook latency benchmark for the recall_lanes feature gate.
# Live-daemon access is restricted to read-class tools; all hook writes land in
# a temporary mind/queue and write RPCs are swallowed by the wrapper below.
set -u

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
N="${1:-20}"
if [[ ! "$N" =~ ^[1-9][0-9]*$ ]]; then
    echo "usage: $0 [positive repetitions per query]" >&2
    exit 2
fi

REAL_CHITTA="${CHITTA_BENCH_BIN:-$ROOT/bin/chitta}"
[[ -x "$REAL_CHITTA" ]] || { echo "not executable: $REAL_CHITTA" >&2; exit 1; }

source "$ROOT/hooks/lib.sh"
LIVE_MIND="${CHITTA_BENCH_MIND:-${CHITTA_DB_PATH:-$HOME/.claude/mind}}"
LIVE_SOCKET="${CHITTA_BENCH_SOCKET:-$(CHITTA_DB_PATH="$LIVE_MIND" get_socket_path)}"
[[ -S "$LIVE_SOCKET" ]] || { echo "live daemon socket not found: $LIVE_SOCKET" >&2; exit 1; }

T=$(mktemp -d "${TMPDIR:-/tmp}/chitta-bench-lanes.XXXXXX")
trap 'rm -rf "$T"' EXIT
mkdir -p "$T/runtime/chitta" "$T/mind" "$T/home"

# Make daemon_available() see the live socket while keeping every hook-created
# state file under $T. The actual CLI is given the live path explicitly.
export XDG_RUNTIME_DIR="$T/runtime"
export HOME="$T/home"
export CHITTA_DB_PATH="$T/mind"
BENCH_SOCKET=$(get_socket_path)
ln -s "$LIVE_SOCKET" "$BENCH_SOCKET"

WRAPPER="$T/chitta-read-only"
cat > "$WRAPPER" <<'WRAP'
#!/bin/bash
tool="${1:-}"
case "$tool" in
    recall|smart_recall|recall_lanes|correction_check|predicate_list|narrative_status|\
    anticipation_filter|anticipation_predict|habit_match|goal_list|curiosity_gaps|\
    msg_inbox|recall_failure_pattern)
        exec "$BENCH_REAL_CHITTA" --socket-path "$BENCH_LIVE_SOCKET" "$@"
        ;;
    *)
        # queue_write, log_event, gate/session/narrative writes, predicate_run,
        # acknowledgements, and optional enrichment are intentionally no-ops.
        exit 0
        ;;
esac
WRAP
chmod +x "$WRAPPER"

export BENCH_REAL_CHITTA="$REAL_CHITTA"
export BENCH_LIVE_SOCKET="$LIVE_SOCKET"
export CHITTA_BIN="$WRAPPER"
export CHITTA_QUEUE="$T/queue.jsonl"
export CHITTA_REALM="${CHITTA_BENCH_REALM:-brahman}"
export CHITTA_LEAN=1
export CHITTA_HOOK_BUDGET_MS="${CHITTA_BENCH_HOOK_BUDGET_MS:-6000}"

if ! timeout 10 "$REAL_CHITTA" --socket-path "$LIVE_SOCKET" status >/dev/null 2>&1; then
    echo "live daemon does not answer read-only status requests: $LIVE_SOCKET" >&2
    exit 1
fi
_probe=$(timeout 10 "$REAL_CHITTA" --socket-path "$LIVE_SOCKET" recall_lanes --json \
    --query "recall lanes benchmark capability probe" --lanes '["corrk"]' 2>/dev/null) || {
    echo "live CLI/daemon does not support recall_lanes; refusing to measure fallback" >&2
    exit 1
}
if ! printf '%s' "$_probe" | jq -e \
    '(.lanes.corrk.text | type == "string") and (.lanes.corrk.results | type == "array")' \
    >/dev/null 2>&1; then
    echo "live recall_lanes response has an unexpected shape" >&2
    exit 1
fi

QUERIES=(
    "how does chitta semantic recall use the field store"
    "what is the prompt hook admission policy"
    "how are durable corrections matched"
)
RESULTS="$T/results.tsv"
: > "$RESULTS"

echo "recall_lanes prompt-hook benchmark: N=$N per query, realm=$CHITTA_REALM" >&2
for ((i=1; i<=N; ++i)); do
    for qi in "${!QUERIES[@]}"; do
        for arm in off on; do
            flag=0; [[ "$arm" == "on" ]] && flag=1
            sid="bench-lanes-${arm}-${qi}-${i}-$$"
            input=$(jq -nc --arg sid "$sid" --arg prompt "${QUERIES[$qi]}" \
                '{session_id:$sid,prompt:$prompt,cwd:"/tmp"}')
            output=$(CHITTA_RECALL_LANES_RPC="$flag" \
                bash "$ROOT/hooks/prompt-core.sh" <<< "$input" 2>/dev/null || true)
            total=$(printf '%s' "$output" | grep -oE 'total=[0-9]+' | tail -1 | cut -d= -f2)
            if [[ -n "$total" ]]; then
                printf '%s\t%s\t%s\t0\n' "$arm" "$qi" "$total" >> "$RESULTS"
            else
                printf '%s\t%s\t\t1\n' "$arm" "$qi" >> "$RESULTS"
            fi
        done
    done
done

python3 - "$RESULTS" <<'PY'
import math
import statistics
import sys

rows = []
with open(sys.argv[1], encoding="utf-8") as handle:
    for line in handle:
        arm, query, total, empty = line.rstrip("\n").split("\t")
        rows.append((arm, int(query), int(total) if total else None, int(empty)))

def percentile(values, pct):
    values = sorted(values)
    if not values:
        return "n/a"
    return values[max(0, math.ceil(len(values) * pct / 100) - 1)]

print("arm\truns\tmedian_total_ms\tp95_total_ms\tempties")
for arm in ("off", "on"):
    selected = [row for row in rows if row[0] == arm]
    totals = [row[2] for row in selected if row[2] is not None]
    median = round(statistics.median(totals)) if totals else "n/a"
    print(f"{arm}\t{len(selected)}\t{median}\t{percentile(totals, 95)}\t{sum(r[3] for r in selected)}")
PY
