#!/bin/bash
# Shared library for chitta hooks (project renamed cc-soul -> chitta,
# 2026-09-02; see docs/RENAME.md).
#
# Common functions used across session-start, prompt, and stop hooks.

# Evaluation wall clock in Unix milliseconds. Explicit date parsing remains real;
# timeout(1) and prompt's budget clock remain unpinned. Invalid values are ignored.
if [[ "${CHITTA_HOOK_NOW:-}" =~ ^[1-9][0-9]{12}$ ]]; then
    date() {
        case " $* " in
            *' -d '*|*' --date'*|*' -r '*|*' --reference'*) command date "$@" ;;
            *) command date --date="@${CHITTA_HOOK_NOW:0:10}.${CHITTA_HOOK_NOW:10:3}" "$@" ;;
        esac
    }
fi

# --- CC_SOUL_* / CHITTA_* env var alias shim -------------------------------
# Every CC_SOUL_* knob keeps working under its CHITTA_* twin and vice versa.
# If a caller sets only one name, this exports the other so any process that
# sources lib.sh sees both; if both are set, neither is touched (new name
# wins at read sites, which read CHITTA_* first). Not a substitute for the
# early-exit reads in hook entrypoints that run before lib.sh is sourced --
# those read both names inline. Table: docs/RENAME.md.
_CHITTA_ALIAS_VARS=(
    ABLATE_LANES ADMIT_DEBUG AGENT_LIMIT AGENT_NO_FORCE AGENT_WARN
    ALLOW_EDIT ALLOW_GLOB_RM ALLOW_READ ANCHOR_ENFORCE AUTO_RECAP
    BOOT_GRACE C2_SMALL_REALM C2_SMALL_REALM_MAXN C2_SMALL_REALM_MINPCT
    CHECKPOINT_INTERVAL CTX_LANE DEEP_SEARCH DISCIPLINE_ENFORCE
    EDIT_REINDEX_RATE ENRICH_INTERVAL HEADLESS HOOK_BUDGET_MS HOOK_ENFORCE
    HOOK_STATE_DIR INDEX_INTERVAL LEAN LEGACY_MARKERS LOOP_LIMIT LOOP_WARN
    MAX_INDEX_FILES MAX_OUTPUT_CHARS MAX_WAIT MCP_DIR MODEL PLUGIN_DIR
    REINDEX_RATE_LIMIT RETAG_INTERVAL SADHANA_MAX
    SADHANA_TIMEOUT SNAPSHOT_TIMEOUT STOP_BOOTSTRAP_BYTES
    STOP_ENRICH_INTERVAL STOP_GRACE STOP_MAX_INCREMENT_BYTES STORE_INTERVAL
    STRICT_MODE STRICT_MODE_DEFAULT SUBAGENT_BASH_RECALL UNKNOWN_SILENCE
)
for _v in "${_CHITTA_ALIAS_VARS[@]}"; do
    _old="CC_SOUL_${_v}"
    _new="CHITTA_${_v}"
    if [[ -z "${!_new+x}" && -n "${!_old+x}" ]]; then
        export "$_new=${!_old}"
    elif [[ -z "${!_old+x}" && -n "${!_new+x}" ]]; then
        export "$_old=${!_new}"
    fi
done
unset _v _old _new

# Resolve the chitta root whether a hook runs from the source tree, a Claude
# plugin cache, or a user-level hook symlink/copy. Callers should still verify
# the particular file they need exists.
resolve_cc_soul_root() {
    local candidate real_root
    candidate=${CHITTA_PLUGIN_DIR:-${CC_SOUL_PLUGIN_DIR:-}}
    if [[ -n "$candidate" && -d "$candidate/chitta-mcp" ]]; then
        printf '%s\n' "$candidate"
        return 0
    fi
    real_root=$(realpath "${BASH_SOURCE[0]}" 2>/dev/null || echo "${BASH_SOURCE[0]}")
    candidate=${real_root%/hooks/*}
    if [[ -d "$candidate/chitta-mcp" ]]; then
        printf '%s\n' "$candidate"
        return 0
    fi

    # Marketplace installs are versioned. `sort -V` makes the newest installed
    # source the compatibility fallback for copied ~/.claude/hooks scripts.
    # Prefer the renamed marketplace/plugin dir, fall back to the pre-rename
    # one for installs that haven't re-added the marketplace yet.
    real_root=$(for candidate in \
        "$HOME"/.claude/plugins/cache/genomewalker-chitta/chitta/* \
        "$HOME"/.claude/plugins/cache/genomewalker-cc-soul/cc-soul/*; do
        [[ -d "$candidate/chitta-mcp" ]] && printf '%s\n' "$candidate"
    done 2>/dev/null | sort -V | tail -1)
    if [[ -n "$real_root" ]]; then
        printf '%s\n' "$real_root"
        return 0
    fi
    return 1
}

# Shared, bounded session-registry transport. Nonzero means the adapter did
# not acknowledge the call; hooks emit the minimal unavailable envelope.
# Usage: printf '%s' "$INPUT" | registry_call <timeout_s> <subcmd> [args...]
registry_call() {
    local t="$1" subcmd="$2"
    shift 2
    local plugin_dir registry
    plugin_dir="$(resolve_cc_soul_root 2>/dev/null)"
    if [[ -z "$plugin_dir" ]]; then
        plugin_dir="$(dirname "$(dirname "$(realpath "${BASH_SOURCE[1]}" 2>/dev/null || echo "${BASH_SOURCE[1]}")")")"
    fi
    registry="$plugin_dir/chitta-mcp/session_registry.py"
    [[ -f "$registry" ]] || return 1
    timeout "$t" python3 "$registry" "$subcmd" "$@" >/dev/null 2>&1
}

# Bounded ledger-tail transport and local notice acknowledgement; policy is native.
saddle_check() {
    local root
    root=$(resolve_cc_soul_root) || return 1
    printf '%s' "$1" | python3 -S "$root/chitta-mcp/hook_client.py" saddle-check \
        "$2" "$3" "${4:-7}" "${5:-3}" "${6:-0.8}"
}

# Direct liveness RPC also renews the daemon-owned thread lease. Preserve the
# durable --queued fallback when the socket is missing or the RPC fails.
session_heartbeat() {
    local session_id="$1" input="$2" metadata args
    metadata=$(jq -c '{thread_id: ((.thread_id // "") | tostring),
                          client: ((.client // "") | tostring)}' <<< "$input") || return 1
    if daemon_available && [[ -x "${CHITTA_BIN:-}" ]] &&
        timeout 1 "$CHITTA_BIN" session_heartbeat --session_id "$session_id" \
            --metadata "$metadata" </dev/null >/dev/null 2>&1; then
        return 0
    fi
    args=$(jq -nc --arg sid "$session_id" --argjson metadata "$metadata" \
        '{session_id:$sid, metadata:$metadata}') || return 1
    queue_write session_heartbeat "$args"
}

# DJB2 hash function (matches C++ implementation in socket_server.hpp)
djb2_hash() {
    local str="$1"
    local h=5381
    local i c
    for ((i=0; i<${#str}; i++)); do
        # Keep this builtin in-process: a subshell per path character adds
        # hundreds of milliseconds before the recall lanes even launch.
        printf -v c '%d' "'${str:$i:1}"
        h=$(( ((h << 5) + h + c) & 0xFFFFFFFF ))
    done
    echo "$h"
}

# Get socket directory — matches C++ get_socket_dir() priority:
# $XDG_RUNTIME_DIR/chitta > ~/.cache/chitta > /tmp
get_socket_dir() {
    if [[ -n "${XDG_RUNTIME_DIR:-}" && -w "$XDG_RUNTIME_DIR" ]]; then
        echo "${XDG_RUNTIME_DIR}/chitta"
    elif [[ -w "/run/user/$(id -u)" ]]; then
        echo "/run/user/$(id -u)/chitta"
    elif [[ -n "${HOME:-}" ]]; then
        echo "${HOME}/.cache/chitta"
    else
        echo "/tmp"
    fi
}

# Compute socket path from mind path — matches C++ socket_path_for_mind()
get_socket_path() {
    # Explicit override first (the CLI honours the same variable), so hooks
    # driven at a replica (benchmarks/smriti, eval-replica.sh) talk to it too.
    if [[ -n "${CHITTA_SOCKET_PATH:-}" ]]; then echo "$CHITTA_SOCKET_PATH"; return; fi
    local mind_path="${CHITTA_DB_PATH:-${CHITTA_MIND:-$HOME/.claude/mind}}"
    local hash=$(djb2_hash "$mind_path")
    echo "$(get_socket_dir)/chitta-${hash}.sock"
}

# Fast O(1) check: is the daemon socket present?
# Returns 0 (true) if socket file exists, 1 (false) otherwise.
# Use this before any blocking chitta CLI calls to skip them instantly when daemon is down.
daemon_available() {
    [[ -S "$(get_socket_path)" ]]
}

# Get next turn index atomically (flock-protected increment)
get_next_turn() {
    local session_id="${1:-}"
    [[ -z "$session_id" ]] && echo 0 && return

    local state_dir
    state_dir=$(runtime_state_dir "${CHITTA_DB_PATH:-$HOME/.claude/mind}") || return 1
    mkdir -p "$state_dir" || return 1
    local turn_file="$state_dir/.turn_index_$session_id"
    local turn
    {
        flock -x 200
        turn=$(cat "$turn_file" 2>/dev/null || echo 0)
        echo $((turn + 1)) > "$turn_file"
    } 200>"$turn_file.lock"
    echo "$turn"
}

# One resolver for queue, markers and ledger tail. Match queue_path.hpp exactly.
runtime_state_dir() {
    local mind="${1:-${MIND_PATH:-${CHITTA_DB_PATH:-$HOME/.claude/mind}}}"
    if [[ "${CHITTA_RUNTIME_LOCAL:-0}" != 1 ]]; then
        printf '%s\n' "$mind"
        return
    fi
    local canonical hash=5381 byte i
    local LC_ALL=C
    canonical=$(realpath -m -- "$mind") || return 1
    for ((i = 0; i < ${#canonical}; i++)); do
        printf -v byte '%d' "'${canonical:i:1}"
        hash=$((hash * 33 + byte))
    done
    printf '%s/chitta/%016x\n' "${XDG_RUNTIME_DIR:-/tmp}" "$hash"
}

# Default queue file location (must match daemon's queue_path in simple_cli.cpp)
get_queue_file() {
    local mind_path="${MIND_PATH:-${CHITTA_DB_PATH:-$HOME/.claude/mind}}"
    local runtime_dir
    runtime_dir=$(runtime_state_dir "$mind_path") || return 1
    echo "${CHITTA_QUEUE:-${CHITTA_QUEUE_PATH:-${runtime_dir%/}/queue.jsonl}}"
}

# Generate UUID for queue acknowledgments
# Uses uuidgen, /proc/sys/kernel/random/uuid, or fallback
generate_ack_id() {
    if command -v uuidgen >/dev/null 2>&1; then
        uuidgen
    elif [[ -f /proc/sys/kernel/random/uuid ]]; then
        cat /proc/sys/kernel/random/uuid
    else
        # Fallback: timestamp + random hex
        printf '%08x-%04x-%04x-%04x-%012x' \
            "$(date +%s)" \
            "$((RANDOM % 65536))" \
            "$((RANDOM % 65536))" \
            "$((RANDOM % 65536))" \
            "$((RANDOM % 281474976710656))"
    fi
}

# Queue write with acknowledgment ID
# Usage: queue_write <tool> <args_json>
# Writes: {"ack_id":"uuid","tool":"...","args":{...},"ts":...}
queue_write() {
    local tool="$1"
    local args="$2"
    local queue_file
    queue_file=$(get_queue_file)

    # Native enqueue: parses/compacts `args` JSON and writes the JSONL line
    # with a single atomic write() syscall. Avoids bash quoting bugs and the
    # "multi-line jq output truncates the JSONL entry" class of parse errors.
    # CHITTA_QUEUE_PATH lets the binary honor non-default queue locations
    # (test fixtures, isolated checkouts).
    [[ -d "${queue_file%/*}" ]] || mkdir -p "${queue_file%/*}"
    local chitta_bin="${CHITTA_BIN:-$HOME/.claude/bin/chitta}"
    if [[ -x "$chitta_bin" ]]; then
        CHITTA_QUEUE_PATH="$queue_file" "$chitta_bin" queue_write "$tool" "$args" >/dev/null 2>&1 && return
    fi

    # Fallback: bootstrap path when the native binary isn't installed yet.
    # Validate with jq, but retain raw argument number tokens: jq 1.6 would
    # otherwise round u64 IDs. Literal CR/LF only occur outside strings in valid
    # JSON; removing them preserves JSONL framing and escaped text newlines.
    local ack_id
    ack_id=$(generate_ack_id)
    local line
    if line=$(jq -nr --arg ack_id "$ack_id" --arg tool "$tool" \
        --arg args "$args" --argjson ts "$(date +%s)" '
        ($args | fromjson) as $validated |
        {ack_id:$ack_id,tool:$tool,ts:$ts} | tojson |
        .[:-1] + ",\"args\":" + ($args | gsub("[\r\n]"; "")) + "}"' 2>/dev/null); then
        # Command substitution strips trailing newlines from jq's output.
        # Restore the JSONL record boundary explicitly so consecutive fallback
        # writes cannot be concatenated into one invalid record.
        printf '%s\n' "$line" >> "$queue_file"
    else
        echo "[queue_write] dropped: no chitta binary and args not valid JSON" >&2
        return 1
    fi
}

# safe_queue_write — queue_write with one retry.
# Absorbs the transient enqueue failure seen when the daemon rotates the queue
# file between our open() and write().
safe_queue_write() {
    local tool="$1"
    local args="$2"
    queue_write "$tool" "$args" && return 0
    sleep 0.05
    queue_write "$tool" "$args"
}

# record_ingest_metric — count turns seen vs turns actually ingested.
# Reads METRICS_FILE and ALERT_FILE from the calling hook, which set them after
# sourcing this library; both are resolved at call time.
# Usage: record_ingest_metric true|false
record_ingest_metric() {
    local success="$1" # true|false
    [[ -f "$METRICS_FILE" ]] || printf '%s\n' '{"turns_total":0,"turns_ingested":0}' > "$METRICS_FILE"
    if [[ "$success" == "true" ]]; then
        jq '.turns_total += 1 | .turns_ingested += 1' "$METRICS_FILE" > "${METRICS_FILE}.tmp" 2>/dev/null || true
    else
        jq '.turns_total += 1' "$METRICS_FILE" > "${METRICS_FILE}.tmp" 2>/dev/null || true
    fi
    if [[ -s "${METRICS_FILE}.tmp" ]]; then
        mv "${METRICS_FILE}.tmp" "$METRICS_FILE"
    else
        rm -f "${METRICS_FILE}.tmp"
    fi

    local total ingested pct
    total=$(jq -r '.turns_total // 0' "$METRICS_FILE" 2>/dev/null || echo 0)
    ingested=$(jq -r '.turns_ingested // 0' "$METRICS_FILE" 2>/dev/null || echo 0)
    if [[ "$total" -gt 0 ]]; then
        pct=$(( ingested * 100 / total ))
        if [[ "$total" -ge 20 && "$pct" -lt 95 ]]; then
            printf '%s ingest_rate=%s%% turns=%s ingested=%s\n' "$(date -Is)" "$pct" "$total" "$ingested" >> "$ALERT_FILE"
        fi
    fi
}

# emit_event — structured soul event with provenance.
# Usage: emit_event <dedup_file> <category> <source> <content> <confidence> <evidence> <realm> [valence] [arousal] [flags] [refs]
#   category:   solution|gotcha|preference|decision|failure|pattern|correction|curiosity_gap
#   source:     hook_regex|hook_compliance|distillation|mcp_tool
#   content:    raw learning text (SSL-formatted by caller or this function)
#   confidence: 0.5 (provisional/hook) or 0.85 (distillation) or 1.0 (explicit)
#   evidence:   what triggered this (e.g. "regex match on [SOLUTION]")
#   valence:    affect valence -1.0..+1.0 (optional)
#   arousal:    affect arousal 0.0..1.0 (optional)
#   flags:      comma-separated semantic flags: ORIGIN,CORE,PIVOT,GENESIS,TURNING (optional)
#   refs:       comma-separated cross-references: tag names or memory IDs (optional)
emit_event() {
    local dedup_file="$1"
    local category="$2"
    local source="$3"
    local content="$4"
    local confidence="${5:-0.7}"
    local evidence="${6:-}"
    local realm="${7:-brahman}"
    local valence="${8:-}"
    local arousal="${9:-}"
    local flags="${10:-}"
    local refs="${11:-}"

    # Quality gate: minimum length
    if [[ ${#content} -lt 30 ]]; then return; fi

    # Quality gate: dedup
    local content_hash
    content_hash=$(echo -n "$content" | md5sum | cut -d' ' -f1)
    if [[ -n "$dedup_file" ]] && grep -q "^${content_hash}$" "$dedup_file" 2>/dev/null; then
        return
    fi
    [[ -n "$dedup_file" ]] && echo "$content_hash" >> "$dedup_file"

    local title
    title=$(echo "$content" | head -c 100)

    # Build JSON args with optional affect/flags/refs
    local args
    args="{\"category\":\"$category\",\"title\":$(echo "$title" | jq -Rs .),\"content\":$(echo "$content" | jq -Rs .),\"confidence\":$confidence,\"source\":$(echo "$source" | jq -Rs .),\"evidence\":$(echo "$evidence" | jq -Rs .),\"realm\":$(echo "$realm" | jq -Rs .)}"

    # Append optional SSL v0.3 fields
    if [[ -n "$valence" ]]; then
        args=$(echo "$args" | jq --arg v "$valence" '. + {valence: ($v | tonumber)}')
    fi
    if [[ -n "$arousal" ]]; then
        args=$(echo "$args" | jq --arg a "$arousal" '. + {arousal: ($a | tonumber)}')
    fi
    if [[ -n "$flags" ]]; then
        args=$(echo "$args" | jq --arg f "$flags" '. + {flags: $f}')
    fi
    if [[ -n "$refs" ]]; then
        args=$(echo "$args" | jq --arg r "$refs" '. + {refs: $r}')
    fi

    queue_write "observe" "$args"
}

# parse_ssl_annotations — extract A:v,a F:FLAG G:N <=@refs src:loc →@ref from an SSL line
# Sets global variables: _SSL_VALENCE, _SSL_AROUSAL, _SSL_FLAGS, _SSL_REFS, _SSL_CLEAN
#                        _SSL_GRANULARITY, _SSL_DERIVATION, _SSL_SOURCE
# Usage: parse_ssl_annotations "line of SSL"
parse_ssl_annotations() {
    local line="$1"
    _SSL_VALENCE=""
    _SSL_AROUSAL=""
    _SSL_FLAGS=""
    _SSL_REFS=""
    _SSL_GRANULARITY=""
    _SSL_DERIVATION=""
    _SSL_SOURCE=""
    _SSL_CLEAN="$line"

    # Extract A:valence,arousal
    if [[ "$line" =~ A:([+-]?[0-9]*\.?[0-9]+),([0-9]*\.?[0-9]+) ]]; then
        _SSL_VALENCE="${BASH_REMATCH[1]}"
        _SSL_AROUSAL="${BASH_REMATCH[2]}"
        _SSL_CLEAN="${_SSL_CLEAN//${BASH_REMATCH[0]}/}"
    fi

    # Extract F:FLAG (comma-separated flags possible: F:PIVOT,ORIGIN)
    if [[ "$line" =~ F:([A-Z_,]+) ]]; then
        _SSL_FLAGS="${BASH_REMATCH[1]}"
        _SSL_CLEAN="${_SSL_CLEAN//${BASH_REMATCH[0]}/}"
    fi

    # Extract G:N — granularity tier 0-4 (v0.4)
    if [[ "$_SSL_CLEAN" =~ G:([0-4]) ]]; then
        _SSL_GRANULARITY="${BASH_REMATCH[1]}"
        _SSL_CLEAN="${_SSL_CLEAN//${BASH_REMATCH[0]}/}"
    fi

    # Extract <=@refs — derivation provenance, comma-separated IDs (v0.4)
    if [[ "$_SSL_CLEAN" =~ \<=@([a-zA-Z0-9_,-]+) ]]; then
        _SSL_DERIVATION="${BASH_REMATCH[1]}"
        _SSL_CLEAN="${_SSL_CLEAN//${BASH_REMATCH[0]}/}"
    fi

    # Extract src:loc — external source grounding (v0.4)
    if [[ "$_SSL_CLEAN" =~ src:([a-zA-Z0-9_./#:-]+) ]]; then
        _SSL_SOURCE="${BASH_REMATCH[1]}"
        _SSL_CLEAN="${_SSL_CLEAN//${BASH_REMATCH[0]}/}"
    fi

    # Extract →@ref (multiple possible)
    local refs=""
    while [[ "$_SSL_CLEAN" =~ →@([a-zA-Z0-9_-]+) ]]; do
        [[ -n "$refs" ]] && refs="$refs,"
        refs="${refs}${BASH_REMATCH[1]}"
        _SSL_CLEAN="${_SSL_CLEAN//${BASH_REMATCH[0]}/}"
    done
    _SSL_REFS="$refs"

    # Trim trailing whitespace from cleaned line
    _SSL_CLEAN="${_SSL_CLEAN%"${_SSL_CLEAN##*[![:space:]]}"}"
}

# Guard: fail if CHITTA_SANDBOX=1 and running in main worktree
worktree_guard() {
    if [[ "${CHITTA_SANDBOX:-0}" != "1" ]]; then
        return 0
    fi
    local main_wt
    main_wt="$(git rev-parse --show-toplevel 2>/dev/null)" || return 0
    local cwd="$PWD"
    # Allow if inside a named worktree directory
    if [[ "$cwd" != "$main_wt"* ]] || [[ "$cwd" == */worktrees/* ]]; then
        return 0
    fi
    echo "ERROR: CHITTA_SANDBOX=1 but operating in main worktree. Use EnterWorktree first." >&2
    return 1
}
