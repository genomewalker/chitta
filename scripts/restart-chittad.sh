#!/usr/bin/env bash
# Restart chittad without abandoning a snapshot family that is mid-write.
#
# SIGTERM inside the full-snapshot save abandons the family being written. The
# next start logs `manifest family … failed validation`, falls back to the
# previous family and replays every WAL record since it. On 2026-09-20 that cost
# a 66-minute tail and a 51 s start where the same store starts in 12-13 s.
#
# The store now publishes `snapshot_in_flight` through health_check, so this
# waits on the writer's own state. Reading the log tail cannot do that: the save
# can begin in the second between the grep and the restart, which is exactly
# what happened on 2026-09-20.
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# shellcheck source=/dev/null
source "$ROOT/hooks/lib.sh"

WAIT_S=600
PROBE_S=300
MODE="wait"
usage() {
    cat >&2 <<'USAGE'
usage: restart-chittad.sh [--wait SECONDS] [--refuse]
  --wait SECONDS  bound the wait for an in-flight save (default 600)
  --refuse        exit 3 instead of waiting when a save is in flight
  --probe SECONDS bound the post-restart probing (default 300)
USAGE
}
while [[ $# -gt 0 ]]; do
    case "$1" in
        --wait)   WAIT_S="${2:-}"; shift 2 || true ;;
        --probe)  PROBE_S="${2:-}"; shift 2 || true ;;
        --refuse) MODE="refuse"; shift ;;
        -h|--help) usage; exit 0 ;;
        *) printf 'unknown argument: %s\n' "$1" >&2; usage; exit 2 ;;
    esac
done
[[ "$WAIT_S"  =~ ^[0-9]+$ ]] || { printf -- '--wait needs whole seconds\n' >&2; exit 2; }
[[ "$PROBE_S" =~ ^[0-9]+$ ]] || { printf -- '--probe needs whole seconds\n' >&2; exit 2; }

MIND="${CHITTA_DB_PATH:-${CHITTA_MIND:-$HOME/.claude/mind}}"
SOCKET="$(CHITTA_DB_PATH="$MIND" get_socket_path)"
LOG="$MIND/chittad.log"
CHITTA="${CHITTA_BIN:-$HOME/.claude/bin/chitta}"
[[ -x "$CHITTA" ]] || { printf 'not executable: %s\n' "$CHITTA" >&2; exit 2; }
command -v jq >/dev/null || { printf 'jq is required\n' >&2; exit 2; }

# One JSON-RPC round trip over the daemon socket. Empty output means the daemon
# did not answer inside the timeout, which is a distinct state from "answered".
probe() {
    local name="$1" args="$2"
    printf '{"jsonrpc":"2.0","method":"tools/call","params":{"name":"%s","arguments":%s},"id":1}\n' \
        "$name" "$args" \
        | timeout 10 "$CHITTA" --socket-path "$SOCKET" 2>/dev/null \
        | grep -v '^\[chitta' || true
}
field() { jq -r ".result.structured.$1 // empty" 2>/dev/null; }

# ── 1. Do not interrupt a save ────────────────────────────────────────────────
if [[ -S "$SOCKET" ]]; then
    waited=0
    while :; do
        health="$(probe health_check '{}')"
        if [[ -z "$health" ]]; then
            # A daemon that cannot answer cannot be asked to finish its save.
            # Restarting is then the lesser evil, but say so in the record.
            printf 'warning: health_check did not answer; proceeding without an in-flight check\n' >&2
            break
        fi
        [[ "$(printf '%s' "$health" | field snapshot_in_flight)" == "true" ]] || break
        if [[ "$MODE" == refuse ]]; then
            printf 'refusing: a snapshot save is in flight\n' >&2
            exit 3
        fi
        if (( waited >= WAIT_S )); then
            printf 'timed out after %ss with a save still in flight\n' "$WAIT_S" >&2
            exit 4
        fi
        (( waited == 0 )) && printf 'waiting for the in-flight snapshot save (up to %ss)\n' "$WAIT_S"
        sleep 2
        waited=$(( waited + 2 ))
    done
    last_commit="$(printf '%s' "${health:-}" | field last_snapshot_commit_ms)"
    printf 'last_snapshot_commit_ms %s\n' "${last_commit:-unknown}"
fi

# ── 2. Restart, clocking the answers from one start instant ───────────────
# The outgoing daemon keeps answering on the same socket path until it exits, so
# every clock below ignores answers carrying the old pid. Without that, healthy
# is stamped from the process being replaced.
old_pid="$(printf '%s' "${health:-}" | field pid)"
log_before=0
[[ -f "$LOG" ]] && log_before="$(wc -l < "$LOG")"
start=$(date +%s.%N)
systemctl --user restart chittad &
restart_pid=$!

since() { awk -v a="$start" -v b="$(date +%s.%N)" 'BEGIN{printf "%.2f", b-a}'; }
elapsed() { awk -v a="$start" -v b="$(date +%s.%N)" 'BEGIN{print (b-a)>=p}' p="$PROBE_S"; }
first_loading=""
first_health=""
first_recall=""
while :; do
    if [[ -S "$SOCKET" ]]; then
        answer="$(probe health_check '{}')"
        pid="$(printf '%s' "$answer" | field pid)"
        if [[ -n "$answer" && "$pid" != "$old_pid" ]]; then
            status="$(printf '%s' "$answer" | field status)"
            [[ -z "$first_loading" && "$status" != "ok" ]] && first_loading="$(since)"
            [[ -z "$first_health"  && "$status" == "ok" ]] && first_health="$(since)"
            if [[ -z "$first_recall" ]]; then
                hits="$(probe recall '{"query":"chitta storage core","limit":3}')"
                if [[ -n "$hits" ]] && ! printf '%s' "$hits" | grep -qi '"loading"'; then
                    [[ "$(printf '%s' "$hits" | jq -r '.result.structured.hits | length' 2>/dev/null)" -gt 0 ]] \
                        && first_recall="$(since)"
                fi
            fi
        fi
    fi
    [[ -n "$first_health" && -n "$first_recall" ]] && break
    # Bounded: a daemon that comes up wedged must not hold this script open.
    [[ "$(elapsed)" == 1 ]] && break
    sleep 0.2
done
wait "$restart_pid"; rc=$?

# ── 3. Report ─────────────────────────────────────────────────────────────────
printf 'systemctl rc %s\n' "$rc"
printf 'first_loading_answer_s %s\n' "${first_loading:-none}"
printf 'first_recall_answer_s %s\n'  "${first_recall:-TIMEOUT}"
printf 'healthy_s %s\n'              "${first_health:-TIMEOUT}"
if [[ -f "$LOG" ]]; then
    tail -n "+$(( log_before + 1 ))" "$LOG" \
        | grep -E '\[shutdown\] .* duration_ms=|\[daemon\] ready ms=|failed validation' \
        | sed 's/^/  /'
fi
exit "$rc"
