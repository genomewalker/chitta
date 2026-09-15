#!/usr/bin/env bash
# Manage a read-mostly chitta daemon from a manifest-committed snapshot.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
EVAL_MIND="${CHITTA_EVAL_MIND:-/projects/caeg/scratch/kbd606/tmp/chitta-eval-mind}"
LIVE_MIND="${CHITTA_LIVE_MIND:-${HOME}/.claude/mind}"
LIVE_FIELD="$LIVE_MIND/chitta-field"
EVAL_FIELD="$EVAL_MIND/chitta-field"
PID_FILE="$EVAL_MIND/replica.pid"
ENV_FILE="$EVAL_MIND/replica.env"
LOG_FILE="$EVAL_MIND/replica.log"
CHITTAD_BIN="${CHITTAD_BIN:-${HOME}/.claude/bin/chittad}"
CHITTA_BIN="${CHITTA_BIN:-${HOME}/.claude/bin/chitta}"
EMBED_MODEL="${CHITTA_EVAL_EMBED_MODEL:-/maps/projects/caeg/people/kbd606/models/nomic-embed-text-v1.5.gguf}"
EVAL_PORT="${CHITTA_EVAL_PORT:-7433}"
START_TIMEOUT="${CHITTA_EVAL_START_TIMEOUT:-600}"
ACTIVE_TMP_MAX_AGE="${CHITTA_EVAL_TMP_MAX_AGE:-600}"

die() { printf 'eval-replica: ERROR: %s\n' "$*" >&2; exit 1; }
usage() { printf 'usage: %s start|stop|status|snapshot-id\n' "${0##*/}" >&2; exit 2; }
require_file() { [[ -f "$1" ]] || die "required file not found: $1"; }

validate_paths() {
    local eval_real live_real home_real
    eval_real="$(python3 -c 'import os,sys; print(os.path.realpath(sys.argv[1]))' "$EVAL_MIND")"
    live_real="$(python3 -c 'import os,sys; print(os.path.realpath(sys.argv[1]))' "$LIVE_MIND")"
    home_real="$(python3 -c 'import os,sys; print(os.path.realpath(sys.argv[1]))' "$HOME")"
    [[ "$EVAL_MIND" == /* ]] || die "CHITTA_EVAL_MIND must be an absolute path"
    [[ "$eval_real" != "/" && "$eval_real" != "$home_real" ]] || die "unsafe CHITTA_EVAL_MIND: $EVAL_MIND"
    [[ "$eval_real" != "$live_real" ]] || die "eval mind must not be the live mind: $EVAL_MIND"
}

manifest_fingerprint() {
    python3 - "$LIVE_FIELD" <<'PY'
import hashlib
import pathlib
import sys

root = pathlib.Path(sys.argv[1])
digest = hashlib.sha256()
found = False
for path in (root / "MANIFEST.1", root / "MANIFEST.2"):
    if path.is_file():
        found = True
        digest.update(path.name.encode())
        digest.update(path.read_bytes())
if not found:
    raise SystemExit("no MANIFEST.1 or MANIFEST.2 found")
print(digest.hexdigest())
PY
}

# Emit the family selected by Manifest::load() + validated_snapshot_path().
# Modern snapshots (v11+) additionally require a valid .pld: save strips their
# payload content from the snapshot body, and open refuses to serve without it.
select_family() {
    python3 "$SCRIPT_DIR/eval-replica-select.py" "${1:-$LIVE_FIELD}"
}

assert_not_mid_save() {
    local snapshot_id="$1"
    python3 - "$LIVE_FIELD" "$snapshot_id" "$ACTIVE_TMP_MAX_AGE" <<'PY'
import pathlib
import re
import sys
import time

root = pathlib.Path(sys.argv[1])
snapshot_id = sys.argv[2]
max_age = int(sys.argv[3])
now = time.time()
active = []
patterns = (
    re.compile(r"MANIFEST\.[12]\.tmp$"),
    re.compile(r"chitta\..+\.(?:snapshot|emb|hdc|bin|mu|hnsw|delta\.hnsw|realm_hnsw|pld|rsf|sup\.json|shdr|lsh|organs|turbo|turbo\.meta)\.tmp$"),
    re.compile(r"cortex\..+\.tmp$"),
)
for path in root.iterdir():
    if not path.is_file() or not any(pattern.fullmatch(path.name) for pattern in patterns):
        continue
    selected = path.name.startswith(f"chitta.{snapshot_id}.") or path.name == f"cortex.{snapshot_id}.tmp"
    manifest_tmp = path.name.startswith("MANIFEST.")
    if (selected or manifest_tmp) and now - path.stat().st_mtime <= max_age:
        active.append(path.name)
if active:
    raise SystemExit("snapshot save appears active (temporary files present): " + ", ".join(sorted(active)))
PY
}

socket_hash() {
    python3 - "$EVAL_MIND" <<'PY'
import sys

value = 5381
for byte in sys.argv[1].encode():
    value = ((value << 5) + value + byte) & 0xFFFFFFFF
print(value)
PY
}

replica_socket() { printf '%s/run/chitta/chitta-%s.sock\n' "$EVAL_MIND" "$(socket_hash)"; }

pid_is_ours() {
    local pid="$1" arg next_is_path=0 saw_daemon=0 saw_path=0 first=1
    [[ "$pid" =~ ^[0-9]+$ && -r "/proc/$pid/cmdline" ]] || return 1
    while IFS= read -r -d '' arg; do
        if (( first )); then
            [[ "${arg##*/}" == "chittad" ]] || return 1
            first=0
        fi
        if (( next_is_path )); then
            [[ "$arg" == "$EVAL_MIND" ]] && saw_path=1
            next_is_path=0
        elif [[ "$arg" == "--path" ]]; then
            next_is_path=1
        elif [[ "$arg" == "daemon" ]]; then
            saw_daemon=1
        fi
    done < "/proc/$pid/cmdline"
    (( saw_daemon && saw_path ))
}

running_pid() {
    [[ -f "$PID_FILE" ]] || return 1
    local pid
    read -r pid < "$PID_FILE" || return 1
    kill -0 "$pid" 2>/dev/null || return 1
    pid_is_ours "$pid" || return 2
    printf '%s\n' "$pid"
}

load_replica_env() {
    [[ -f "$ENV_FILE" ]] || die "replica metadata not found: $ENV_FILE"
    # shellcheck disable=SC1090
    source "$ENV_FILE"
}

write_replica_env() {
    local pid="$1" snapshot_id="$2" snapshot_seqno="$3" generation="$4" socket="$5" tmp="$ENV_FILE.tmp"
    {
        printf 'CHITTA_EVAL_MIND=%q\n' "$EVAL_MIND"
        printf 'CHITTA_EVAL_SOCKET=%q\n' "$socket"
        printf 'CHITTA_EVAL_PORT=%q\n' "$EVAL_PORT"
        printf 'CHITTA_EVAL_SNAPSHOT_ID=%q\n' "$snapshot_id"
        printf 'CHITTA_EVAL_SNAPSHOT_SEQNO=%q\n' "$snapshot_seqno"
        printf 'CHITTA_EVAL_MANIFEST_GENERATION=%q\n' "$generation"
        printf 'CHITTA_EVAL_PID=%q\n' "$pid"
    } > "$tmp"
    mv "$tmp" "$ENV_FILE"
}

status_probe() {
    local socket="$1"
    "$CHITTA_BIN" --socket-path "$socket" recall --query ping --limit 1 --no-learn
}

snapshot_id_command() {
    if [[ -f "$ENV_FILE" ]]; then
        load_replica_env
        [[ -n "${CHITTA_EVAL_SNAPSHOT_ID:-}" ]] || die "replica.env has no snapshot id"
        printf '%s\n' "$CHITTA_EVAL_SNAPSHOT_ID"
        return
    fi
    [[ -d "$LIVE_FIELD" ]] || die "live store directory not found: $LIVE_FIELD"
    local selection snapshot_id=""
    selection="$(select_family)" || die "unable to select a snapshot family"
    while IFS=$'\t' read -r kind value _; do
        [[ "$kind" == "ID" ]] && snapshot_id="$value"
    done <<< "$selection"
    [[ -n "$snapshot_id" ]] || die "snapshot selection returned no id"
    printf '%s\n' "$snapshot_id"
}

start_replica() {
    local pid rc
    if pid="$(running_pid)"; then
        load_replica_env
        printf 'eval replica already running (pid=%s, snapshot=%s, manifest_generation=%s)\n' \
            "$pid" "$CHITTA_EVAL_SNAPSHOT_ID" "${CHITTA_EVAL_MANIFEST_GENERATION:-unknown}"
        status_probe "$CHITTA_EVAL_SOCKET"
        return
    else
        rc=$?
        (( rc != 2 )) || die "pidfile $PID_FILE points to a process that is not this replica"
    fi
    [[ ! -f "$PID_FILE" ]] || rm -f "$PID_FILE"
    require_file "$CHITTAD_BIN"
    require_file "$CHITTA_BIN"
    require_file "$EMBED_MODEL"
    require_file /bin/true
    command -v setsid >/dev/null || die "required command not found: setsid"
    [[ "$EVAL_PORT" =~ ^[0-9]+$ ]] && (( EVAL_PORT > 0 && EVAL_PORT < 65536 )) || die "invalid CHITTA_EVAL_PORT: $EVAL_PORT"
    [[ "$START_TIMEOUT" =~ ^[0-9]+$ ]] || die "invalid CHITTA_EVAL_START_TIMEOUT: $START_TIMEOUT"
    [[ -d "$LIVE_FIELD" ]] || die "live store directory not found: $LIVE_FIELD"

    local before selection after_selection staged_selection snapshot_id="" snapshot_seqno="" generation=""
    local staged_id="" staged_generation="" kind name size stage stage_field after socket runtime_dir old_field i
    local -a family_files=() family_sizes=() manifest_files=() manifest_sizes=() wal_files=() wal_sizes=()
    before="$(manifest_fingerprint)" || die "could not fingerprint live manifests"
    selection="$(select_family)" || die "unable to select a snapshot family"
    while IFS=$'\t' read -r kind name size; do
        case "$kind" in
            ID) snapshot_id="$name" ;;
            SEQNO) snapshot_seqno="$name" ;;
            GENERATION) generation="$name" ;;
            MANIFEST) manifest_files+=("$name"); manifest_sizes+=("$size") ;;
            FILE) family_files+=("$name"); family_sizes+=("$size") ;;
            WAL) wal_files+=("$name"); wal_sizes+=("$size") ;;
        esac
    done <<< "$selection"
    [[ -n "$snapshot_id" && -n "$snapshot_seqno" && -n "$generation" && ${#manifest_files[@]} -gt 0 ]] \
        || die "incomplete snapshot selection"
    assert_not_mid_save "$snapshot_id" || die "refusing to copy while the live store may be saving"

    mkdir -p "${EVAL_MIND%/*}"
    stage="$(mktemp -d "${EVAL_MIND}.stage.XXXXXX")"
    stage_field="$stage/chitta-field"
    mkdir -p "$stage_field"
    (( ${#wal_files[@]} == 0 )) || mkdir -p "$stage_field/segments"
    trap 'rm -rf "${stage:-}"' EXIT
    printf 'copying snapshot %s (seqno=%s, manifest_generation=%s, files=%s)\n' \
        "$snapshot_id" "$snapshot_seqno" "$generation" "$(( ${#family_files[@]} + ${#manifest_files[@]} + ${#wal_files[@]} ))"
    for ((i = 0; i < ${#family_files[@]}; i++)); do
        name="${family_files[$i]}"
        size="${family_sizes[$i]}"
        cp --reflink=auto --preserve=mode,timestamps "$LIVE_FIELD/$name" "$stage_field/$name"
        [[ "$(stat -c %s "$stage_field/$name")" == "$size" ]] || die "copied size mismatch for $name"
        [[ "$(stat -c %s "$LIVE_FIELD/$name")" == "$size" ]] || die "source changed while copying $name"
    done
    for ((i = 0; i < ${#manifest_files[@]}; i++)); do
        name="${manifest_files[$i]}"
        size="${manifest_sizes[$i]}"
        cp --preserve=mode,timestamps "$LIVE_FIELD/$name" "$stage_field/$name"
        [[ "$(stat -c %s "$stage_field/$name")" == "$size" ]] || die "copied size mismatch for $name"
    done
    for ((i = 0; i < ${#wal_files[@]}; i++)); do
        name="${wal_files[$i]}"
        size="${wal_sizes[$i]}"
        cp --reflink=auto --preserve=mode,timestamps "$LIVE_FIELD/segments/$name" "$stage_field/segments/$name"
        [[ "$(stat -c %s "$stage_field/segments/$name")" == "$size" ]] || die "copied size mismatch for segments/$name"
        [[ "$(stat -c %s "$LIVE_FIELD/segments/$name")" == "$size" ]] || die "source changed while copying segments/$name"
    done
    # Loader state markers (field.rs load): without the *.migrated flags the
    # replica re-marks every memory for re-embedding and its semantic index
    # is degraded for hours (golden nDCG 0.31 vs 0.49, 2026-09-13).
    for name in embed_1536_v1.migrated ssl_gloss_v1.migrated lite_encoder.bin; do
        [[ -f "$LIVE_FIELD/$name" ]] || continue
        cp --preserve=mode,timestamps "$LIVE_FIELD/$name" "$stage_field/$name"
    done
    after="$(manifest_fingerprint)" || die "could not re-fingerprint live manifests"
    [[ "$before" == "$after" ]] || die "live manifest changed during the copy; snapshot discarded"
    after_selection="$(select_family)" || die "live family became inconsistent during the copy; snapshot discarded"
    [[ "$selection" == "$after_selection" ]] || die "live family files changed during the copy; snapshot discarded"
    assert_not_mid_save "$snapshot_id" || die "a live snapshot save began during the copy; snapshot discarded"

    # There is no read-only cf_open/CLI validator: cf_open creates a WAL and may
    # prune caches. Re-run the manifest/file/.pld checks above against staging,
    # before the first daemon process is launched.
    staged_selection="$(select_family "$stage_field")" || die "copied store failed pre-start consistency verification"
    while IFS=$'\t' read -r kind name _; do
        case "$kind" in
            ID) staged_id="$name" ;;
            GENERATION) staged_generation="$name" ;;
        esac
    done <<< "$staged_selection"
    [[ "$staged_id" == "$snapshot_id" && "$staged_generation" == "$generation" ]] \
        || die "copied store selects snapshot=$staged_id generation=$staged_generation; expected snapshot=$snapshot_id generation=$generation"
    printf 'verified snapshot=%s manifest_generation=%s before daemon start\n' "$snapshot_id" "$generation"

    mkdir -p "$EVAL_MIND"
    old_field="$EVAL_MIND/chitta-field.previous.$$"
    [[ ! -e "$old_field" ]] || rm -rf "$old_field"
    [[ ! -d "$EVAL_FIELD" ]] || mv "$EVAL_FIELD" "$old_field"
    mv "$stage_field" "$EVAL_FIELD"
    rmdir "$stage"
    stage=""
    [[ ! -d "$old_field" ]] || rm -rf "$old_field"
    trap - EXIT

    runtime_dir="$EVAL_MIND/run"
    mkdir -p "$runtime_dir/chitta"
    socket="$(replica_socket)"
    rm -f "$socket"
    # The daemon's quiesce gate suppresses periodic theme/belief/learning
    # passes that have no individual CLI switches. CHITTA_NO_QUEUE prevents
    # this scratch daemon from consuming the live hooks' shared queue.
    touch "$EVAL_MIND/.quiesce"
    : > "$LOG_FILE"
    # The daemon records its own pid: when setsid finds itself a process-group
    # leader (job control on, CI runners) it forks, and $! would name the
    # already-exited parent.
    rm -f "$PID_FILE"
    XDG_RUNTIME_DIR="$runtime_dir" CHITTA_RPC_PORT="$EVAL_PORT" \
        CHITTA_NO_QUEUE=1 CHITTA_HINT_ENRICHER=/bin/true \
        nohup setsid bash -c 'printf "%s\n" "$$" > "$1"; shift; exec "$@"' _ "$PID_FILE" \
            "$CHITTAD_BIN" daemon --path "$EVAL_MIND" --foreground \
            --no-autonomous --no-distill --distill-interval 60 --no-enrich \
            --no-hygiene --no-embed-interval --embed-model "$EMBED_MODEL" \
            --rpc-port "$EVAL_PORT" >> "$LOG_FILE" 2>&1 &
    pid=""
    for _ in $(seq 1 50); do
        [[ -s "$PID_FILE" ]] && read -r pid < "$PID_FILE" && [[ -n "$pid" ]] && break
        sleep 0.1
    done
    [[ -n "$pid" ]] || die "replica daemon did not record its pid"
    write_replica_env "$pid" "$snapshot_id" "$snapshot_seqno" "$generation" "$socket"
    printf 'started pid=%s socket=%s rpc_port=%s manifest_generation=%s\n' "$pid" "$socket" "$EVAL_PORT" "$generation"

    local deadline=$((SECONDS + START_TIMEOUT))
    while (( SECONDS < deadline )); do
        if ! kill -0 "$pid" 2>/dev/null; then
            printf '%s\n' '--- replica.log ---' >&2
            tail -80 "$LOG_FILE" >&2 || true
            rm -f "$PID_FILE"
            die "replica exited before becoming ready"
        fi
        if status_probe "$socket" >/dev/null 2>&1; then
            printf 'ready snapshot=%s seqno=%s manifest_generation=%s\n' "$snapshot_id" "$snapshot_seqno" "$generation"
            return
        fi
        sleep 2
    done
    kill "$pid" 2>/dev/null || true
    rm -f "$PID_FILE"
    printf '%s\n' '--- replica.log ---' >&2
    tail -80 "$LOG_FILE" >&2 || true
    die "replica did not become ready within ${START_TIMEOUT}s"
}

stop_replica() {
    local pid rc
    if pid="$(running_pid)"; then
        :
    else
        rc=$?
        (( rc != 2 )) || die "pidfile $PID_FILE points to a process that is not this replica"
        [[ ! -f "$PID_FILE" ]] || rm -f "$PID_FILE"
        printf '%s\n' 'eval replica is not running'
        return
    fi
    printf 'stopping pid=%s\n' "$pid"
    kill "$pid"
    local deadline=$((SECONDS + 60))
    while kill -0 "$pid" 2>/dev/null && (( SECONDS < deadline )); do sleep 1; done
    kill -0 "$pid" 2>/dev/null && die "pid $pid did not stop after SIGTERM"
    rm -f "$PID_FILE"
    printf '%s\n' 'stopped'
}

status_replica() {
    local pid
    pid="$(running_pid)" || die "eval replica is not running"
    load_replica_env
    printf 'pid=%s snapshot=%s manifest_generation=%s socket=%s rpc_port=%s\n' \
        "$pid" "$CHITTA_EVAL_SNAPSHOT_ID" "${CHITTA_EVAL_MANIFEST_GENERATION:-unknown}" \
        "$CHITTA_EVAL_SOCKET" "$CHITTA_EVAL_PORT"
    status_probe "$CHITTA_EVAL_SOCKET"
}

validate_paths

case "${1:-}" in
    start)
        # The live daemon rewrites MANIFEST.* every few seconds; a copy that
        # races a save is discarded and retried rather than failing the run.
        for _attempt in 1 2 3; do
            if _out="$(start_replica 2>&1)"; then printf '%s\n' "$_out"; exit 0; fi
            printf '%s\n' "$_out" >&2
            grep -qE 'size mismatch|source changed|manifest changed|mid-save|active' <<< "$_out" || exit 1
            sleep 5
        done
        exit 1 ;;
    stop) stop_replica ;;
    status) status_replica ;;
    snapshot-id) snapshot_id_command ;;
    *) usage ;;
esac
