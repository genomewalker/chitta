#!/bin/bash
# Shared library for chitta hooks (project renamed cc-soul -> chitta,
# 2026-09-02; see docs/RENAME.md).
#
# Common functions used across session-start, prompt, and stop hooks.

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
    REINDEX_RATE_LIMIT RETAG_INTERVAL RLM_MODE RLM_QUERY SADHANA_MAX
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
    for candidate in \
        "${CHITTA_PLUGIN_DIR:-${CC_SOUL_PLUGIN_DIR:-}}" \
        "$(dirname "$(dirname "$(realpath "${BASH_SOURCE[0]}" 2>/dev/null || echo "${BASH_SOURCE[0]}")")")"; do
        if [[ -n "$candidate" && -d "$candidate/chitta-mcp" ]]; then
            printf '%s\n' "$candidate"
            return 0
        fi
    done

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

# Shared session_registry.py invocation used by session-start, session-end,
# codex-session-start, prompt-core, and stop-core hooks. Resolves the plugin
# root the same way each of those call sites did, is a no-op (fail-open,
# non-fatal) when the registry script isn't present, and never propagates the
# invoked subprocess's own exit status — callers that need a fallback path
# when the registry itself is missing can branch on this function's return
# code instead (1 = registry not found, 0 = registry was invoked).
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

# Per-Bash saddle check. Python remains the offline/Stop reference. The caller
# bounds tail parsing and comparison with a 300 ms jq timeout.
# Usage: saddle_check HOOK_JSON LEDGER MIND_DIR [MINUTES FAILS SIMILARITY]
saddle_check() (
    local input="$1" ledger="$2" mind="$3" marker
    local result identity notice previous marker_fd
    [[ -s "$ledger" ]] || return 1
    result=$(tail -c 1048577 -- "$ledger" | tail -n 4097 | timeout -s KILL 0.3s jq -Rrs \
        --argjson input "$input" \
        --argjson minutes "${4:-7}" --argjson min_fails "${5:-3}" \
        --argjson threshold "${6:-0.8}" '
        def ws: "[\\s\\x{001c}-\\x{001f}\\x{0085}]";
        def strip: sub("^" + ws + "+"; "") | sub(ws + "+$"; "");
        # Unicode 13 lowercase ranges (start,end,stride,delta), generated from
        # Python 3.9 str.lower; jq ascii_downcase alone changes command identity.
        # ASCII commands bypass the table. U+0130 expands; sigma is contextual.
        def lower:
            ascii_downcase | if test("[^\\x{00}-\\x{7f}]") then
            gsub("(?<pre>\\p{Cased}\\p{Case_Ignorable}*)Σ(?!\\p{Case_Ignorable}*\\p{Cased})";
                 .pre + "ς") |
            ("[
            [192,214,1,32],[216,222,1,32],[256,302,2,1],[306,310,2,1],[313,327,2,1],
            [330,374,2,1],[376,376,1,-121],[377,381,2,1],[385,385,1,210],[386,388,2,1],
            [390,390,1,206],[391,391,1,1],[393,394,1,205],[395,395,1,1],[398,398,1,79],
            [399,399,1,202],[400,400,1,203],[401,401,1,1],[403,403,1,205],[404,404,1,207],
            [406,406,1,211],[407,407,1,209],[408,408,1,1],[412,412,1,211],[413,413,1,213],
            [415,415,1,214],[416,420,2,1],[422,422,1,218],[423,423,1,1],[425,425,1,218],
            [428,428,1,1],[430,430,1,218],[431,431,1,1],[433,434,1,217],[435,437,2,1],
            [439,439,1,219],[440,440,1,1],[444,444,1,1],[452,452,1,2],[453,453,1,1],
            [455,455,1,2],[456,456,1,1],[458,458,1,2],[459,475,2,1],[478,494,2,1],
            [497,497,1,2],[498,500,2,1],[502,502,1,-97],[503,503,1,-56],[504,542,2,1],
            [544,544,1,-130],[546,562,2,1],[570,570,1,10795],[571,571,1,1],[573,573,1,-163],
            [574,574,1,10792],[577,577,1,1],[579,579,1,-195],[580,580,1,69],[581,581,1,71],
            [582,590,2,1],[880,882,2,1],[886,886,1,1],[895,895,1,116],[902,902,1,38],
            [904,906,1,37],[908,908,1,64],[910,911,1,63],[913,929,1,32],[931,939,1,32],
            [975,975,1,8],[984,1006,2,1],[1012,1012,1,-60],[1015,1015,1,1],[1017,1017,1,-7],
            [1018,1018,1,1],[1021,1023,1,-130],[1024,1039,1,80],[1040,1071,1,32],[1120,1152,2,1],
            [1162,1214,2,1],[1216,1216,1,15],[1217,1229,2,1],[1232,1326,2,1],[1329,1366,1,48],
            [4256,4293,1,7264],[4295,4295,1,7264],[4301,4301,1,7264],[5024,5103,1,38864],[5104,5109,1,8],
            [7312,7354,1,-3008],[7357,7359,1,-3008],[7680,7828,2,1],[7838,7838,1,-7615],[7840,7934,2,1],
            [7944,7951,1,-8],[7960,7965,1,-8],[7976,7983,1,-8],[7992,7999,1,-8],[8008,8013,1,-8],
            [8025,8031,2,-8],[8040,8047,1,-8],[8072,8079,1,-8],[8088,8095,1,-8],[8104,8111,1,-8],
            [8120,8121,1,-8],[8122,8123,1,-74],[8124,8124,1,-9],[8136,8139,1,-86],[8140,8140,1,-9],
            [8152,8153,1,-8],[8154,8155,1,-100],[8168,8169,1,-8],[8170,8171,1,-112],[8172,8172,1,-7],
            [8184,8185,1,-128],[8186,8187,1,-126],[8188,8188,1,-9],[8486,8486,1,-7517],[8490,8490,1,-8383],
            [8491,8491,1,-8262],[8498,8498,1,28],[8544,8559,1,16],[8579,8579,1,1],[9398,9423,1,26],
            [11264,11310,1,48],[11360,11360,1,1],[11362,11362,1,-10743],[11363,11363,1,-3814],[11364,11364,1,-10727],
            [11367,11371,2,1],[11373,11373,1,-10780],[11374,11374,1,-10749],[11375,11375,1,-10783],[11376,11376,1,-10782],
            [11378,11378,1,1],[11381,11381,1,1],[11390,11391,1,-10815],[11392,11490,2,1],[11499,11501,2,1],
            [11506,11506,1,1],[42560,42604,2,1],[42624,42650,2,1],[42786,42798,2,1],[42802,42862,2,1],
            [42873,42875,2,1],[42877,42877,1,-35332],[42878,42886,2,1],[42891,42891,1,1],[42893,42893,1,-42280],
            [42896,42898,2,1],[42902,42920,2,1],[42922,42922,1,-42308],[42923,42923,1,-42319],[42924,42924,1,-42315],
            [42925,42925,1,-42305],[42926,42926,1,-42308],[42928,42928,1,-42258],[42929,42929,1,-42282],[42930,42930,1,-42261],
            [42931,42931,1,928],[42932,42942,2,1],[42946,42946,1,1],[42948,42948,1,-48],[42949,42949,1,-42307],
            [42950,42950,1,-35384],[42951,42953,2,1],[42997,42997,1,1],[65313,65338,1,32],[66560,66599,1,40],
            [66736,66771,1,40],[68736,68786,1,64],[71840,71871,1,32],[93760,93791,1,32],[125184,125217,1,34]
            ]" | fromjson) as $ranges |
            explode | map(. as $c | if . == 304 then [105,775]
                else (first($ranges[] | select($c >= .[0] and $c <= .[1]
                      and ($c - .[0]) % .[2] == 0)) // [0,0,1,0]) as $r |
                     [$c + $r[3]] end) | add | implode
            else . end;
        def normalize:
            strip | lower | gsub("\\p{Nd}+"; "#")
            | gsub(ws + "+"; " ") | .[:60];
        # SequenceMatcher(None,a,b): longest contiguous block, earliest a/b
        # tie, then recurse left/right. With <=60 characters autojunk is off.
        def block($a; $b):
            (reduce range(0; $b|length) as $j ({}; .[$b[$j]|tostring] += [$j])) as $positions |
            reduce range(0; $a|length) as $i
                ({best:[0,0,0], prev:{}};
                 .prev as $prev | .next = {} |
                 reduce ($positions[$a[$i]|tostring] // [])[] as $j (. ;
                     if $a[$i] == $b[$j] then
                         (($prev[($j-1)|tostring] // 0) + 1) as $n |
                         .next[$j|tostring] = $n |
                         if $n > .best[2] then .best = [$i+1-$n,$j+1-$n,$n] else . end
                     else . end) | .prev = .next) | .best;
        def matched($a; $b):
            block($a; $b) as [$i,$j,$n] |
            if $n == 0 then 0 else
                $n + (if $i > 0 and $j > 0 then matched($a[:$i];$b[:$j]) else 0 end)
                   + (if $i+$n < ($a|length) and $j+$n < ($b|length)
                      then matched($a[$i+$n:];$b[$j+$n:]) else 0 end)
            end;
        def similar($a; $b):
            if $a == $b then true
            elif 2 * ([($a|length),($b|length)]|min) / (($a|length)+($b|length)) < $threshold
            then false
            else 2 * matched($a|explode; $b|explode) / (($a|length)+($b|length)) >= $threshold end;
        def truth: . != null and . != false and . != 0 and . != "" and . != [] and . != {};
        def failed:
            if .exit_code == null then .likely_fail | truth
            else .exit_code | if type == "boolean" then .
                elif type == "number" then (if . < 0 then ceil else floor end) != 0
                elif type == "string" then
                    try (strip | select(test("^[+-]?[0-9]+(_[0-9]+)*$"))
                         | gsub("_"; "") | tonumber != 0) catch false
                else false end // false end;
        # Python json.dumps defaults to ASCII escaping, including surrogate pairs.
        def hex4:
            . as $n | [4096,256,16,1 | . as $d |
                "0123456789abcdef"[(($n / $d | floor) % 16):][0:1]] | join("");
        def pyjson:
            tojson | explode | map(if . < 128 then [.] | implode
                elif . <= 65535 then "\\u" + hex4
                else . - 65536 | "\\u" + ((55296 + (. / 1024 | floor)) | hex4)
                    + "\\u" + ((56320 + (. % 1024)) | hex4) end) | join("");
        ($input.session_id // "" | tostring) as $sid |
        select($sid|test("\\A[a-zA-Z0-9_-]+\\z")) |
        ($input.tool_input.command // "") as $cmd |
        (now * 1000) as $now |
        (utf8bytelength > 1048576) as $partial |
        # One extra byte detects truncation without stat. If that byte is a
        # newline, Python starts at the next record and discards that too.
        split("\n")[:-1] |
        if $partial then (if .[0] == "" then .[2:] else .[1:] end) else . end |
        .[-4096:] |
        map(select(contains("bash_outcome") and contains($sid)) |
            fromjson? | select(type == "object") |
            select(.session_id == $sid and .event == "bash_outcome") |
            select(.ts|type == "number") |
            select(.ts >= $now - $minutes * 60000 and .ts <= $now) |
            select(.cmd_head|type == "string") | select(.cmd_head|strip|length > 0)) |
        sort_by(.ts) |
        reduce .[] as $row ([];
            if $row|failed then
                ($row.cmd_head|normalize) as $shape |
                ([to_entries[] | select(similar(.value.shape;$shape)) | .key][0]) as $idx |
                if $idx == null then . + [{shape:$shape,fails:[$row]}]
                else .[$idx].fails += [$row] end
            elif $row.exit_code == 0 or $row.exit_code == "0" or $row.exit_code == false
            then if length == 0 then . else
                ($row.cmd_head|normalize) as $shape |
                map(select(similar(.shape;$shape)|not)) end else . end) |
        map(select((.fails|length) >= $min_fails and similar(.shape; $cmd|normalize))) |
        # max() in Python keeps the first group on timestamp ties.
        sort_by(-.fails[-1].ts) | .[0] | select(. != null) |
        . as $g | .fails[-1].stderr_head as $err |
        (if $err|truth then $err|tostring else "error unavailable" end
         | strip | gsub(ws + "+"; " ") | .[:160]) as $excerpt |
        ($sid + ":" + ($g.fails[0].ts|tostring) + ":" + $g.shape),
        ("[saddle] this command shape failed \($g.fails|length)× in \($minutes) min "
         + "(last: \($excerpt)). Change approach or read the error before retrying." | pyjson)
    ') || return 1
    [[ -n "$result" ]] || return 1
    identity=${result%%$'\n'*}
    notice=${result#*$'\n'}
    # Same nonblocking advisory lock and line-based marker as Python check().
    marker="$(runtime_state_dir "$mind")/.saddle_${identity%%:*}"
    mkdir -p "${marker%/*}" || return 1
    exec {marker_fd}<>"$marker" || return 1
    flock -n "$marker_fd" || return 1
    while IFS= read -r previous || [[ -n "$previous" ]]; do
        [[ "$previous" != "$identity" ]] || return 1
    done <&"$marker_fd"
    printf '%s\n' "$identity" >>"$marker" || return 1
    printf '{"hookSpecificOutput": {"hookEventName": "PreToolUse", "additionalContext": %s}}\n' "$notice"
)

# Preserve Python's left-to-right, non-greedy markup stripping. Bound each sed
# substitution at the FIRST matching close, including nested reminders.
clean_query() {
    local text rest tag prefix matched closer cleaned="" old_nocasematch=0
    local opener='<(task-notification|system-reminder|command-name|command-message|local-command-[[:alnum:]_]+)[^>]*>'
    text=$(cat)
    shopt -q nocasematch && old_nocasematch=1
    shopt -s nocasematch
    while [[ "$text" =~ $opener ]]; do
        matched=${BASH_REMATCH[0]} tag=${BASH_REMATCH[1]}
        prefix=${text%%"$matched"*}
        rest=${text#*"$matched"}
        closer="</$tag>"
        # \w+ can backtrack when the opening local-command name is longer
        # than its close; [^>]* then consumes the remaining name characters.
        while [[ ! "$rest" =~ $closer && "$tag" == local-command-* && ${#tag} -gt 15 ]]; do
            tag=${tag%?}
            closer="</$tag>"
        done
        if [[ "$rest" =~ $closer ]]; then
            closer=${BASH_REMATCH[0]}
            cleaned+=$(printf '%s' "$prefix$matched${rest%%"$closer"*}$closer" |
                sed -Ez 's@<(task-notification|system-reminder|command-name|command-message|local-command-[[:alnum:]_]+)[^>]*>.*</\1>@@I'; printf '.')
            cleaned=${cleaned%.}
            text=${rest#*"$closer"}
        else
            cleaned+="$prefix$matched"
            text=$rest
        fi
    done
    (( old_nocasematch )) || shopt -u nocasematch
    # Python str.strip also includes C0 separators, NEL and nonbreaking spaces.
    local space=$'\t\n\v\f\r\034\035\036\037 \u0085\u00a0\u1680\u2000\u2001\u2002\u2003\u2004\u2005\u2006\u2007\u2008\u2009\u200a\u2028\u2029\u202f\u205f\u3000'
    space=${space//$'\n'/\\n}
    printf '%s' "$cleaned$text" | sed -Ez "s/^[$space]+//; s/[$space]+$//"
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

# Shared Pre/PostToolUse JSON string escaping and lifecycle path decoding.
json_escape() {
    echo -n "$1" | jq -Rs '.' | sed 's/^"//;s/"$//'
}

decode_project_path() {
    local encoded="${1:1}"  # Skip leading dash
    local path_so_far=""
    local part
    local -a PARTS
    IFS='-' read -ra PARTS <<< "$encoded"
    for part in "${PARTS[@]}"; do
        local test_path="$path_so_far/$part"
        if [[ -d "$test_path" ]]; then
            path_so_far="$test_path"
        else
            local alt_path="$path_so_far-$part"
            if [[ -d "$alt_path" ]]; then
                path_so_far="$alt_path"
            else
                path_so_far="$test_path"
            fi
        fi
    done
    echo "$path_so_far"
}

# Lifecycle lookup; prompt-core's cached local realm detection is distinct.
# Uses caller CHITTA_BIN/MAX_WAIT and preserves output on CLI failure.
detect_project_realm() {
    local project_dir="$1"
    if [[ -n "$project_dir" && -d "$project_dir" ]]; then
        (cd "$project_dir" && timeout "$MAX_WAIT" "$CHITTA_BIN" realm_detect 2>/dev/null || echo "brahman")
    else
        timeout "$MAX_WAIT" "$CHITTA_BIN" realm_detect 2>/dev/null || echo "brahman"
    fi
}

# Get current session ID from environment or registry
get_session_id() {
    # First check environment
    if [[ -n "${CLAUDE_SESSION_ID:-}" ]]; then
        echo "$CLAUDE_SESSION_ID"
        return
    fi

    # Use CLI sql_query to lookup session by PID (no netcat)
    local claude_pid=${PPID:-$$}
    if [[ -n "$claude_pid" && "$claude_pid" != "0" ]]; then
        local result
        result=$(chitta sql_query --query "SELECT session_id FROM session_registry WHERE pid = $claude_pid AND status = 'active' LIMIT 1" --text-only 2>/dev/null)
        # Extract UUID from result (handles table format output)
        local session_id
        session_id=$(echo "$result" | grep -oE '[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}' | head -1)
        if [[ -n "$session_id" ]]; then
            echo "$session_id"
            return
        fi
    fi

    # Fallback to empty (caller should handle default)
    echo ""
}

# Get next turn index atomically (flock-protected increment)
get_next_turn() {
    local session_id="${1:-$(get_session_id)}"
    [[ -z "$session_id" ]] && echo 0 && return

    local turn_file="$HOME/.claude/mind/.turn_index_$session_id"
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
    mkdir -p "$(dirname "$queue_file")"
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
