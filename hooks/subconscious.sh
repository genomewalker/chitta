#!/bin/bash
# Subconscious daemon management
#
# Usage: subconscious.sh <start|stop|status>

# Don't use set -e: we want hooks to succeed even if daemon is slow to start

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PLUGIN_DIR="$(dirname "$SCRIPT_DIR")"
source "${SCRIPT_DIR}/lib.sh"

# Binaries are installed to ~/.claude/bin/ by smart-install.sh
CHITTA_CLI="${HOME}/.claude/bin/chittad"
MIND_PATH="${CHITTA_DB_PATH:-${HOME}/.claude/mind}"

# The GPU embed server publishes its URL only once it answers, so a present file means live.
# Exported at file scope so every daemon start path inherits it, and the daemon re-points
# itself whenever the slurm job lands on a different node. Absent file => CPU fallback.
EMBED_URL_FILE="${CHITTA_BRIDGE_URL_DIR:-${HOME}/.chitta-bridge/endpoints}/embed-server.url"
if [[ -z "${CHITTA_EMBED_URL:-}" && -s "$EMBED_URL_FILE" ]]; then
    export CHITTA_EMBED_URL="$(<"$EMBED_URL_FILE")"
fi
MODEL_PATH="${HOME}/.claude/bin/model.onnx"
VOCAB_PATH="${HOME}/.claude/bin/vocab.txt"
LOG_FILE="${HOME}/.claude/mind/.subconscious.log"
# PID_FILE is defined after MIND_HASH calculation below
INTERVAL="${SUBCONSCIOUS_INTERVAL:-60}"
TIMEOUT_CMD=()
TIMEOUT_WARNED=false
MAX_WAIT="${CHITTA_MAX_WAIT:-${CC_SOUL_MAX_WAIT:-10}}"
# How long a daemon may take to boot before we treat silence as "stuck" rather than
# "still loading". Must exceed worst-case snapshot load + WAL replay, or hooks kill the
# daemon mid-boot on every tool call and it never comes up.
BOOT_GRACE="${CHITTA_BOOT_GRACE:-${CC_SOUL_BOOT_GRACE:-1800}}"
# Seconds to let a SIGTERMed daemon finish its shutdown snapshot before SIGKILL.
STOP_GRACE="${CHITTA_STOP_GRACE:-${CC_SOUL_STOP_GRACE:-120}}"

if [[ "$MAX_WAIT" != "0" ]] && command -v timeout >/dev/null 2>&1; then
    TIMEOUT_CMD=(timeout "$MAX_WAIT")
fi

run_with_timeout() {
    if [[ "$MAX_WAIT" != "0" && ${#TIMEOUT_CMD[@]} -eq 0 && "$TIMEOUT_WARNED" != "true" ]]; then
        echo "[cc-soul] timeout not available; running without limit" >&2
        TIMEOUT_WARNED=true
    fi

    if [[ ${#TIMEOUT_CMD[@]} -gt 0 ]]; then
        "${TIMEOUT_CMD[@]}" "$@"
    else
        "$@"
    fi
}

MIND_HASH=$(djb2_hash "$MIND_PATH")

# Keep the manager's existing directory creation and fallback policy.
# lib.sh get_socket_dir also probes /run/user; lifecycle management does not.
subconscious_socket_dir() {
    if [[ -n "$XDG_RUNTIME_DIR" && -w "$XDG_RUNTIME_DIR" ]]; then
        local dir="$XDG_RUNTIME_DIR/chitta"
        mkdir -p "$dir" 2>/dev/null
        echo "$dir"
    elif [[ -n "$HOME" ]]; then
        local dir="$HOME/.cache/chitta"
        mkdir -p "$dir" 2>/dev/null
        echo "$dir"
    else
        echo "/tmp"
    fi
}

SOCKET_DIR="$(subconscious_socket_dir)"
LOCK_FILE="${SOCKET_DIR}/chitta-${MIND_HASH}.lock"
SOCKET_PATH="${SOCKET_DIR}/chitta-${MIND_HASH}.sock"
PID_FILE="${SOCKET_DIR}/chitta-${MIND_HASH}.pid"  # Daemon writes PID here

# Check if daemon is managed by systemd (service file exists + user session active)
uses_systemd() {
    [[ -f "${HOME}/.config/systemd/user/chittad.service" ]] && \
    command -v systemctl &>/dev/null && \
    systemctl --user status &>/dev/null 2>&1
}

is_running() {
    # First check PID file
    if [[ -f "$PID_FILE" ]]; then
        local pid
        pid=$(cat "$PID_FILE")
        if kill -0 "$pid" 2>/dev/null; then
            return 0
        fi
        # Stale PID file
        rm -f "$PID_FILE"
    fi

    # Also check for any running daemon process (covers MCP-spawned daemons)
    if pgrep -f "chittad daemon.*--path $MIND_PATH" >/dev/null 2>&1; then
        return 0
    fi

    return 1
}

# True if any listed pid is still doing work: parked in uninterruptible NFS I/O (D), or
# burning CPU across a 2s sample. Either way it is alive and must not be killed.
_daemon_progressing() {
    local pid line state c0 c1
    for pid in "$@"; do
        line=$(sed -e 's/^.*) //' "/proc/$pid/stat" 2>/dev/null) || continue
        [[ -z "$line" ]] && continue
        state=$(awk '{print $1}' <<<"$line")
        [[ "$state" == "D" ]] && return 0
        c0=$(awk '{print $12+$13}' <<<"$line")
        sleep 2
        line=$(sed -e 's/^.*) //' "/proc/$pid/stat" 2>/dev/null) || continue
        c1=$(awk '{print $12+$13}' <<<"$line")
        [[ -n "$c0" && -n "$c1" ]] && (( c1 > c0 )) && return 0
    done
    return 1
}

# Return PIDs of chittad daemons that are NOT bound to our MIND_PATH.
# Covers bare `chittad daemon` (no --path) and daemons with a different path
# that still share our socket dir — sources of split-brain storage.
find_stray_daemons() {
    local all_pids stray_pids=""
    all_pids=$(pgrep -x chittad 2>/dev/null || true)
    for pid in $all_pids; do
        [[ -r "/proc/$pid/cmdline" ]] || continue
        local cmdline
        cmdline=$(tr '\0' ' ' < "/proc/$pid/cmdline")
        # Only flag daemons with NO --path (would default to same mind path → conflict).
        # Daemons with a different explicit --path are isolated by design; leave them alone.
        if [[ "$cmdline" != *"--path "* ]]; then
            stray_pids+="$pid "
        fi
    done
    echo "${stray_pids% }"
}

# Kill any daemons whose --path != MIND_PATH. Quiet when nothing to do.
kill_stray_daemons() {
    local strays
    strays=$(find_stray_daemons)
    [[ -z "$strays" ]] && return 0
    echo "[subconscious] Killing stray daemon(s) with wrong/missing --path: $strays" >&2
    for pid in $strays; do
        kill -TERM "$pid" 2>/dev/null || true
    done
    sleep 1
    for pid in $strays; do
        kill -0 "$pid" 2>/dev/null && kill -KILL "$pid" 2>/dev/null || true
    done
}

# Cache for is_responsive — reset by kill_unresponsive after daemon restart
_responsive_cache=""

# Check if daemon actually responds to commands (not just running)
is_responsive() {
    if [[ "$_responsive_cache" == "yes" ]]; then return 0; fi
    if [[ "$_responsive_cache" == "no" ]];  then return 1; fi

    if [[ ! -S "$SOCKET_PATH" ]]; then
        _responsive_cache="no"
        return 1
    fi

    # Try health check with short timeout (CLI handles socket communication)
    local response
    response=$(timeout 3 "${HOME}/.claude/bin/chitta" --socket-path "$SOCKET_PATH" health_check 2>/dev/null || true)
    if [[ -n "$response" && "$response" == *"Status:"* ]]; then
        _responsive_cache="yes"
        return 0
    fi
    _responsive_cache="no"
    return 1
}

# Kill unresponsive daemon(s) and clean up
kill_unresponsive() {
    echo "[subconscious] Daemon unresponsive, killing..." >&2

    # Get all daemon PIDs
    local pids=""
    if [[ -f "$PID_FILE" ]]; then
        pids=$(cat "$PID_FILE" 2>/dev/null || true)
    fi
    local other_pids
    other_pids=$(pgrep -f "chittad daemon.*--path $MIND_PATH" 2>/dev/null || true)
    if [[ -n "$other_pids" ]]; then
        pids="$pids $other_pids"
    fi
    pids=$(echo "$pids" | tr ' ' '\n' | sort -u | grep -v '^$' | tr '\n' ' ')

    # Force kill all
    for pid in $pids; do
        kill -9 "$pid" 2>/dev/null || true
    done

    # Socket and PID only. Never the lock file: it is the single-writer mutex and it is keyed
    # by inode, so deleting it lets the next daemon lock a fresh inode and run alongside a
    # live one. fcntl releases the lock on death, so there is nothing here to clean up.
    rm -f "$SOCKET_PATH" "$PID_FILE" 2>/dev/null || true

    sleep 1
    echo "[subconscious] Cleaned up stale daemon" >&2
}

cmd_start() {
    # Sweep stray daemons (wrong/missing --path) before anything else to
    # prevent split-brain storage with the legitimate daemon.
    kill_stray_daemons

    # When systemd manages the daemon, delegate to it and just wait for readiness
    if uses_systemd; then
        systemctl --user start chittad 2>/dev/null || true
        # Wait for socket to become responsive
        local daemon_ready=false
        local wait_start
        wait_start=$(date +%s)
        while true; do
            if is_responsive; then
                daemon_ready=true
                break
            fi
            if [[ "$MAX_WAIT" != "0" ]]; then
                local now
                now=$(date +%s)
                if (( now - wait_start >= MAX_WAIT )); then
                    break
                fi
            fi
            sleep 0.1
        done
        if $daemon_ready; then
            echo "[subconscious] Started via systemd (healthy)"
        else
            echo "[subconscious] Daemon still initializing (will retry on next tool call)" >&2
        fi
        return 0
    fi

    # Non-systemd path: manage daemon directly
    # First: kill ALL existing daemon processes to ensure clean state
    # This prevents multiple daemons from accumulating
    local existing_pids
    existing_pids=$(pgrep -f "chittad daemon.*--path $MIND_PATH" 2>/dev/null || true)
    if [[ -n "$existing_pids" ]]; then
        # Check if any are responsive (use CLI instead of netcat)
        if [[ -S "$SOCKET_PATH" ]]; then
            local response
            response=$(timeout 2 "${HOME}/.claude/bin/chitta" --socket-path "$SOCKET_PATH" health_check 2>/dev/null || true)
            if [[ -n "$response" && "$response" == *"Status:"* ]]; then
                # Healthy daemon exists, nothing to do
                return 0
            fi
        fi
        # An absent socket does NOT mean dead: chittad binds it only after the store is
        # loaded, and a 650MB snapshot + WAL replay takes minutes. Hooks fire every few
        # seconds, so killing here restarts the boot forever — and a SIGKILL landing
        # mid-snapshot leaves a family with no .pld sidecar, which the loader then refuses
        # (field.rs:584). That livelock is what corrupted the store on 2026-07-14.
        local youngest=999999 age
        for pid in $existing_pids; do
            age=$(ps -o etimes= -p "$pid" 2>/dev/null | tr -d ' ')
            [[ -n "$age" ]] && (( age < youngest )) && youngest=$age
        done
        if (( youngest < BOOT_GRACE )); then
            echo "[subconscious] Daemon booting (${youngest}s of ${BOOT_GRACE}s grace) — leaving it alone" >&2
            return 0
        fi
        # Grace expired — but a slow answer is still not death, and conflating the two is
        # what killed this store. The health_check above gives the daemon 2 seconds; a
        # daemon that is merely BUSY blows that budget routinely (maint cycles, distill
        # passes, and reads on the hard-mounted mind volume that park the thread in
        # uninterruptible D state — simple_cli.cpp:336). SIGTERM is ignored in D state, so
        # the SIGKILL 30s later lands mid-snapshot and leaves a family with no .pld sidecar
        # that the loader then refuses (field.rs:584). That is how 2026-07-14 produced 54
        # daemon starts and 0 clean shutdowns. Liveness must be read from the process, not
        # from its latency.
        if _daemon_progressing $existing_pids; then
            echo "[subconscious] Daemon busy but progressing (D-state or burning CPU) — leaving it alone" >&2
            return 0
        fi
        # Neither answering, nor blocked on the filer, nor burning CPU: genuinely stuck.
        # SIGTERM first so an in-flight snapshot can finish; SIGKILL only if it refuses.
        echo "[subconscious] Cleaning up stuck daemon(s): $existing_pids" >&2
        kill -TERM $existing_pids 2>/dev/null || true
        for _ in {1..30}; do
            pgrep -f "chittad daemon.*--path $MIND_PATH" >/dev/null 2>&1 || break
            sleep 1
        done
        for pid in $existing_pids; do
            kill -0 "$pid" 2>/dev/null && kill -9 "$pid" 2>/dev/null || true
        done
        rm -f "$SOCKET_PATH" "$PID_FILE" 2>/dev/null || true
        sleep 1
    fi

    if [[ ! -x "$CHITTA_CLI" ]]; then
        echo "[cc-soul] Not installed. Run /cc-soul-setup or /cc-soul-update first." >&2
        return 0  # Don't block session, just warn
    fi

    # Atomic lock to prevent race conditions
    # Use mkdir which is atomic on POSIX systems
    local lock_dir="/tmp/chitta-${MIND_HASH}.startlock"
    if ! mkdir "$lock_dir" 2>/dev/null; then
        # Another process is starting - wait for it
        echo "[subconscious] Another process is starting daemon, waiting..." >&2
        for _ in {1..50}; do
            sleep 0.1
            if [[ -S "$SOCKET_PATH" ]]; then
                local response
                response=$(timeout 2 "${HOME}/.claude/bin/chitta" --socket-path "$SOCKET_PATH" health_check 2>/dev/null || true)
                if [[ -n "$response" && "$response" == *"Status:"* ]]; then
                    return 0
                fi
            fi
        done
        # Timed out waiting, clean stale lock if old
        if [[ -d "$lock_dir" ]]; then
            local lock_age lock_mtime
            # Use portable stat syntax (Linux: -c %Y, macOS: -f %m)
            if stat -c %Y "$lock_dir" >/dev/null 2>&1; then
                lock_mtime=$(stat -c %Y "$lock_dir" 2>/dev/null || echo 0)
            else
                lock_mtime=$(stat -f %m "$lock_dir" 2>/dev/null || echo 0)
            fi
            lock_age=$(( $(date +%s) - lock_mtime ))
            if (( lock_age > 30 )); then
                rmdir "$lock_dir" 2>/dev/null || true
            fi
        fi
        # Don't fail the hook - another process is starting the daemon
        return 0
    fi

    # We hold the lock - ensure cleanup on exit
    trap 'rmdir "$lock_dir" 2>/dev/null || true' EXIT

    # Detect supported daemon flags (avoid incompatible binaries)
    local daemon_help
    daemon_help=$(run_with_timeout "$CHITTA_CLI" daemon --help 2>&1 || true)
    if [[ -z "$daemon_help" ]]; then
        echo "[subconscious] Unable to read daemon help; proceeding cautiously" >&2
    fi

    local support_interval=false
    if echo "$daemon_help" | grep -q -- "--interval"; then
        support_interval=true
    fi

    local daemon_args=(daemon "--path" "$MIND_PATH" "--no-autonomous")
    if [[ "$support_interval" == "true" ]]; then
        daemon_args+=("--interval" "$INTERVAL")
    fi

    # Start daemon - it will fork and return immediately
    _responsive_cache=""  # invalidate cache — daemon was just (re)started
    "$CHITTA_CLI" "${daemon_args[@]}" 2>>"$LOG_FILE"

    # Wait for socket AND verify daemon responds
    # This ensures the daemon is fully ready for MCP clients
    local daemon_ready=false
    local wait_start
    wait_start=$(date +%s)
    while true; do
        if [[ -S "$SOCKET_PATH" ]]; then
            # Socket exists, now verify daemon responds with heartbeat (CLI)
            local response
            response=$(run_with_timeout "${HOME}/.claude/bin/chitta" --socket-path "$SOCKET_PATH" health_check 2>/dev/null || true)
            if [[ -n "$response" && "$response" == *"Status:"* ]]; then
                daemon_ready=true
                break
            fi
        fi

        if ! is_running; then
            echo "[subconscious] Failed to start (daemon exited). See $LOG_FILE" >&2
            break
        fi

        if [[ "$MAX_WAIT" != "0" ]]; then
            local now
            now=$(date +%s)
            if (( now - wait_start >= MAX_WAIT )); then
                echo "[subconscious] Startup timed out after ${MAX_WAIT}s (daemon may still be initializing)" >&2
                break
            fi
        fi

        sleep 0.1
    done

    # Release lock
    rmdir "$lock_dir" 2>/dev/null || true
    trap - EXIT

    if $daemon_ready && is_running; then
        local pid
        pid=$(cat "$PID_FILE" 2>/dev/null || pgrep -f "chittad daemon.*--path $MIND_PATH" | head -1)
        echo "[subconscious] Started (pid=$pid, socket=$SOCKET_PATH, heartbeat=ok)"
    else
        # Don't fail the hook - daemon may still be initializing
        # MCP server will spawn it on demand if needed
        echo "[subconscious] Daemon still initializing (will retry on next tool call)" >&2
    fi
}

cmd_stop() {
    # Clean up any stale lock directories first
    local lock_dir="/tmp/chitta-${MIND_HASH}.startlock"
    rmdir "$lock_dir" 2>/dev/null || true

    # When systemd manages the daemon, delegate to it
    if uses_systemd; then
        systemctl --user stop chittad 2>/dev/null || true
        echo "[subconscious] Stopped via systemd"
        return 0
    fi

    if ! is_running; then
        # Also clean up sockets/files even if not running
        rm -f "$SOCKET_PATH" "$PID_FILE" 2>/dev/null || true
        echo "[subconscious] Not running (cleaned up stale files)"
        return 0
    fi

    # Get PIDs from both PID file and process search
    local pids=""
    if [[ -f "$PID_FILE" ]]; then
        pids=$(cat "$PID_FILE")
    fi
    # Also find any MCP-spawned daemons
    local other_pids
    other_pids=$(pgrep -f "chittad daemon.*--path $MIND_PATH" 2>/dev/null || true)
    if [[ -n "$other_pids" ]]; then
        pids="$pids $other_pids"
    fi
    pids=$(echo "$pids" | tr ' ' '\n' | sort -u | tr '\n' ' ')

    echo "[subconscious] Stopping daemon(s): $pids"
    for pid in $pids; do
        kill "$pid" 2>/dev/null || true
    done

    # Wait for graceful shutdown. Chittad flushes its snapshot on SIGTERM and that takes
    # 15-20s on a 650MB store — the old 5s budget force-killed it every single time, losing
    # everything since the last periodic snapshot and leaving a .pld-less family behind.
    for _ in $(seq 1 "$STOP_GRACE"); do
        if ! is_running; then
            echo "[subconscious] Stopped"
            rm -f "$PID_FILE" "$SOCKET_PATH" 2>/dev/null || true
            return 0
        fi
        sleep 1
    done

    # Force kill any remaining
    for pid in $pids; do
        kill -9 "$pid" 2>/dev/null || true
    done
    rm -f "$PID_FILE" "$SOCKET_PATH" 2>/dev/null || true
    echo "[subconscious] Force stopped after ${STOP_GRACE}s (snapshot may be incomplete)" >&2
}

cmd_status() {
    if is_running; then
        local pid
        if [[ -f "$PID_FILE" ]]; then
            pid=$(cat "$PID_FILE")
            echo "[subconscious] Running (pid=$pid, managed)"
        else
            pid=$(pgrep -f "chittad daemon.*--path $MIND_PATH" | head -1)
            echo "[subconscious] Running (pid=$pid, MCP-spawned)"
        fi
        # Show socket info
        if [[ -S "$SOCKET_PATH" ]]; then
            echo "[subconscious] Socket: $SOCKET_PATH"
        else
            echo "[subconscious] Socket: not found"
        fi
        echo "[subconscious] PID file: $PID_FILE"
        return 0
    else
        echo "[subconscious] Not running"
        echo "[subconscious] Socket: $SOCKET_PATH"
        echo "[subconscious] PID file: $PID_FILE"
        return 1
    fi
}

cmd_health() {
    if ! is_running; then
        echo "[subconscious] UNHEALTHY: Not running"
        return 1
    fi

    if ! is_responsive; then
        echo "[subconscious] UNHEALTHY: Running but not responding"
        echo "[subconscious] Attempting recovery..."
        kill_unresponsive
        cmd_start
        if is_responsive; then
            echo "[subconscious] RECOVERED: Daemon restarted successfully"
            return 0
        else
            echo "[subconscious] FAILED: Could not recover daemon"
            return 1
        fi
    fi

    local pid
    pid=$(cat "$PID_FILE" 2>/dev/null || pgrep -f "chittad daemon.*--path $MIND_PATH" | head -1)
    echo "[subconscious] HEALTHY: pid=$pid, responsive"
    return 0
}

case "${1:-status}" in
    start)   cmd_start ;;
    stop)    cmd_stop ;;
    restart) cmd_stop; cmd_start ;;
    status)  cmd_status ;;
    health)  cmd_health ;;
    *)
        echo "Usage: subconscious.sh <start|stop|restart|status|health>"
        exit 1
        ;;
esac
