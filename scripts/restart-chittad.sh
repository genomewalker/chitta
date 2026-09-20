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
DRY_RUN=0
usage() {
    cat >&2 <<'USAGE'
usage: restart-chittad.sh [--wait SECONDS] [--refuse]
  --wait SECONDS  bound the wait for an in-flight save (default 600)
  --refuse        exit 3 instead of waiting when a save is in flight
  --probe SECONDS bound the post-restart probing (default 300)
  --dry-run       skip systemctl; run the probe loop against the daemon
                  that is already up and print every field it parses
USAGE
}
while [[ $# -gt 0 ]]; do
    case "$1" in
        --wait)   WAIT_S="${2:-}"; shift 2 || true ;;
        --probe)  PROBE_S="${2:-}"; shift 2 || true ;;
        --refuse) MODE="refuse"; shift ;;
        --dry-run) DRY_RUN=1; shift ;;
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
# `// empty` would drop a false boolean; only null and a missing key read as absent.
field() { jq -r "if (.result.structured.$1 | . == null) then empty else (.result.structured.$1 | tostring) end" 2>/dev/null; }

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
# an answer is attributed to it only on positive evidence: both pids present and
# equal. A warming daemon answers `loading` with no pid at all, and treating a
# missing pid as "still the old one" discards exactly the answers these clocks
# exist to catch.
if (( DRY_RUN )); then old_pid=""; else old_pid="$(printf '%s' "${health:-}" | field pid)"; fi
log_before=0
[[ -f "$LOG" ]] && log_before="$(wc -l < "$LOG")"
start=$(date +%s.%N)
restart_pid=""
if (( DRY_RUN )); then
    printf 'dry run: no systemctl; probing the daemon that is already up\n'
else
    systemctl --user restart chittad &
    restart_pid=$!
fi

since() { awk -v a="$start" -v b="$(date +%s.%N)" 'BEGIN{printf "%.2f", b-a}'; }
# `p=...` after the program text is an operand assignment, which awk applies
# only while reading input. A BEGIN-only program reads none, so p stayed unset,
# (b-a)>=0 was always true and the probe loop broke on its first pass -- which
# is why 2026-09-20 08:12Z reported none/TIMEOUT against a healthy start.
elapsed() { awk -v a="$start" -v b="$(date +%s.%N)" -v p="$PROBE_S" 'BEGIN{print (b-a)>=p}'; }
first_loading=""
first_health=""
first_recall=""
while :; do
    if [[ -S "$SOCKET" ]]; then
        answer="$(probe health_check '{}')"
        pid="$(printf '%s' "$answer" | field pid)"
        status="$(printf '%s' "$answer" | field status)"
        loading="$(printf '%s' "$answer" | field loading)"
        outgoing=0
        [[ -n "$old_pid" && -n "$pid" && "$pid" == "$old_pid" ]] && outgoing=1
        if (( DRY_RUN )); then
            printf '  probe t=%ss pid=%s old_pid=%s status=%s loading=%s outgoing=%s in_flight=%s commit_ms=%s\n' \
                "$(since)" "${pid:-none}" "${old_pid:-none}" "${status:-none}" \
                "${loading:-none}" "$outgoing" \
                "$(printf '%s' "$answer" | field snapshot_in_flight | grep . || echo absent)" \
                "$(printf '%s' "$answer" | field last_snapshot_commit_ms | grep . || echo absent)"
        fi
        if [[ -n "$answer" ]] && (( ! outgoing )); then
            # A warming answer is either `loading` or any status that is not ok.
            if [[ -z "$first_loading" && ( "$loading" == "true" || ( -n "$status" && "$status" != "ok" ) ) ]]; then
                first_loading="$(since)"
            fi
            [[ -z "$first_health" && "$status" == "ok" ]] && first_health="$(since)"
            if [[ -z "$first_recall" ]]; then
                # recall answers under `results`, not `hits`: the old key made
                # this test null and first_recall could never be stamped.
                reply="$(probe recall '{"query":"chitta storage core","limit":3}')"
                n="$(printf '%s' "$reply" | jq -r '.result.structured.results | length' 2>/dev/null)"
                rl="$(printf '%s' "$reply" | field loading)"
                if (( DRY_RUN )); then
                    printf '  recall t=%ss results=%s loading=%s\n' "$(since)" "${n:-none}" "${rl:-none}"
                fi
                [[ "$rl" != "true" && "${n:-0}" =~ ^[0-9]+$ && "${n:-0}" -gt 0 ]] && first_recall="$(since)"
            fi
        fi
    fi
    [[ -n "$first_health" && -n "$first_recall" ]] && break
    # Bounded: a daemon that comes up wedged must not hold this script open.
    [[ "$(elapsed)" == 1 ]] && break
    sleep 0.2
done
rc=0
[[ -n "$restart_pid" ]] && { wait "$restart_pid"; rc=$?; }

# ── 3. Report ─────────────────────────────────────────────────────────────────
if (( DRY_RUN )); then printf 'systemctl skipped (dry run)\n'; else printf 'systemctl rc %s\n' "$rc"; fi
printf 'first_loading_answer_s %s\n' "${first_loading:-none}"
printf 'first_recall_answer_s %s\n'  "${first_recall:-TIMEOUT}"
printf 'healthy_s %s\n'              "${first_health:-TIMEOUT}"
if [[ -f "$LOG" ]]; then
    tail -n "+$(( log_before + 1 ))" "$LOG" \
        | grep -E '\[shutdown\] .* duration_ms=|\[daemon\] ready ms=|failed validation' \
        | sed 's/^/  /'
fi
exit "$rc"
