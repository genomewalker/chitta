#!/bin/bash
# Headless bridge participant (fusion room, codex_run): nobody is reading this.
# A one-shot model treats injected advice as instruction and goes exploring —
# with these live, sol ran 93 tool calls on a room prompt and never finished.
# Bash still passes safety checks before the headless enrichment bypass.
if [[ -n "${CHITTA_HEADLESS:-$CC_SOUL_HEADLESS}" && "${1:-}" != "Bash" ]]; then cat >/dev/null; printf '{}'; exit 0; fi

# PreToolUse hook: safety blocks, large-output guards, soul corrections.
#
# Stages:
# 1. Safety: block obviously destructive commands (rm -rf /, etc.).
# 2. Fallback: warn on unbounded find/grep/ls; suggest a safer form.
# 3. Soul: surface corrections/gotchas from chitta memory before execution.
#
# Output compression for Bash is handled upstream by sqz at the user-global
# hook layer — this hook no longer rewrites commands for token reduction.

# Don't use set -e: we want hooks to succeed even if some parts fail

MATCHER="${1:-}"
[[ -z "$MATCHER" ]] && exit 0

CHITTA_BIN="${CHITTA_BIN:-$HOME/.claude/bin/chitta}"
STDIN_DATA=$(cat)
exec </dev/null  # stdin consumed; children must not inherit the still-open hook pipe (chitta CLI blocks on it)

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${SCRIPT_DIR}/lib.sh" 2>/dev/null || true

json_escape() {
    echo -n "$1" | jq -Rs '.' | sed 's/^"//;s/"$//'
}

_strict_mode_enabled() {
    case "${CHITTA_STRICT_MODE:-${CC_SOUL_STRICT_MODE:-}}" in
        1) return 0 ;;
        0) return 1 ;;
    esac
    local mind="${CHITTA_DB_PATH:-${HOME}/.claude/mind}"
    [[ -f "${mind}/.strict_claude_style" ]]
}

# ─── Code-intel shadow/enforcement ────────────────────────────────────────────
# Phase 1 (initial): log what the hook WOULD do without changing behavior.
# Phase 2 (auto):    once shadow log has ≥100 entries AND is ≥3 days old, the
#                    hook flips to enforce mode automatically. No env var needed.
#                    Explicit CHITTA_HOOK_ENFORCE=0 disables; =1 forces on early
#                    (CC_SOUL_HOOK_ENFORCE still honored).
# Escape hatch:      CHITTA_ALLOW_READ=1 bypasses per env (CC_SOUL_ALLOW_READ too).
#                    Also honoured if ~/.claude/mind/.allow_read_<session_id> exists
#                    (use when env var can't persist across hook invocations).
_should_enforce() {
    # Explicit override wins
    case "${CHITTA_HOOK_ENFORCE:-${CC_SOUL_HOOK_ENFORCE:-}}" in
        1) return 0 ;;
        0) return 1 ;;
    esac
    # Cache result per-session to avoid re-computing on every Read/Edit.
    # Session_id comes from STDIN_DATA, cached in /tmp (per-node safe).
    local sid
    sid=$(echo "$STDIN_DATA" | jq -r '.session_id // empty' 2>/dev/null)
    local cache="/tmp/cc-soul-enforce-${sid:-unknown}-$USER"
    if [[ -f "$cache" ]]; then
        local cached
        cached=$(cat "$cache" 2>/dev/null)
        [[ "$cached" == "1" ]] && return 0
        [[ "$cached" == "0" ]] && return 1
    fi

    local mind="${CHITTA_DB_PATH:-${HOME}/.claude/mind}"
    local log="$mind/.hook_shadow.jsonl"
    local decision=0  # default: off
    if [[ -f "$log" && -r "$log" ]]; then
        local n first_ts first_epoch age
        n=$(wc -l < "$log" 2>/dev/null || echo 0)
        if [[ "${n:-0}" -ge 100 ]]; then
            first_ts=$(head -1 "$log" 2>/dev/null | jq -r '.ts // empty' 2>/dev/null)
            if [[ -n "$first_ts" ]]; then
                first_epoch=$(date -d "$first_ts" +%s 2>/dev/null || echo 0)
                if [[ "$first_epoch" -gt 0 ]]; then
                    age=$(( ( $(date +%s) - first_epoch ) / 86400 ))
                    [[ "$age" -ge 3 ]] && decision=1
                fi
            fi
        fi
    fi
    # Cache for rest of session (ignore write errors)
    [[ -n "$sid" ]] && echo "$decision" > "$cache" 2>/dev/null
    [[ "$decision" == "1" ]]
}
#
# Log format: one JSON line per Read/Edit decision →
#   $MIND_PATH/.hook_shadow.jsonl
# Fields: ts, tool, file, lines, indexed, decision, reason, enforced
_shadow_log() {
    local mind="${CHITTA_DB_PATH:-${HOME}/.claude/mind}"
    [[ -d "$mind" && -w "$mind" ]] || return 0
    local log="$mind/.hook_shadow.jsonl"
    # Cap log at 10MB — rotate to .1 if exceeded
    if [[ -f "$log" ]]; then
        local sz
        sz=$(stat -c %s "$log" 2>/dev/null || echo 0)
        if [[ "$sz" -gt 10485760 ]]; then
            mv "$log" "$log.1" 2>/dev/null || true
        fi
    fi
    local ts tool file lines indexed decision reason enforced
    ts=$(date -u +%Y-%m-%dT%H:%M:%SZ)
    tool="$1"; file="$2"; lines="$3"; indexed="$4"
    decision="$5"; reason="$6"; enforced="$7"
    printf '{"ts":"%s","tool":"%s","file":%s,"lines":%s,"indexed":%s,"decision":"%s","reason":%s,"enforced":%s}\n' \
        "$ts" "$tool" "$(echo -n "$file" | jq -Rs '.' 2>/dev/null || echo '""')" "${lines:-0}" "${indexed:-0}" \
        "$decision" "$(echo -n "$reason" | jq -Rs '.' 2>/dev/null || echo '""')" "${enforced:-0}" \
        >> "$log" 2>/dev/null || true
}

# ─── Safety blocks (destructive commands) ─────────────────────────────────────
safety_check() {
    local cmd="$1"
    # Broad MCP matches include the HTTP transport. Only a PID-selection pipeline
    # that explicitly filters --http out may kill other MCP workers.
    if [[ "${CHITTA_ALLOW_MCP_KILL:-0}" != "1" ]]; then
        local _kill_segment _mcp_cmd="${cmd//\[c\]/c}"
        while IFS= read -r _kill_segment; do
            [[ "$_kill_segment" =~ (^|[^[:alnum:]_.-])(pkill|kill|killall)([[:space:]]|$) ]] || continue
            [[ "$_kill_segment" == *chitta-mcp* ]] || continue
            if [[ ! "$_kill_segment" =~ (^|[^[:alnum:]_.-])(pkill|killall)[[:space:]] ]]; then
                local _exclude="grep[[:space:]]+(-[EF]*v[EF]*|--invert-match)[[:space:]]+(--[[:space:]]+)?['\"]?--http['\"]?([[:space:]|)]|$)"
                # The exclusion must filter PID input, not the kill command's output.
                if grep -qE "kill[[:space:]][^|]*\\$\\([^)]*pgrep[^)]*\\|[[:space:]]*$_exclude" <<<"$_kill_segment" ||
                   { [[ ! "${_kill_segment%%grep*}" =~ (^|[^[:alnum:]_.-])kill[[:space:]] ]] &&
                     grep -qE "pgrep[^|]*\\|[[:space:]]*$_exclude.*\\|[^;]*xargs[[:space:]][^;]*kill([[:space:]]|$)" <<<"$_kill_segment"; }; then
                    continue
                fi
            fi
            echo '{"hookSpecificOutput":{"hookEventName":"PreToolUse","permissionDecision":"deny","permissionDecisionReason":"Killing chitta-mcp can sever the HTTP transport. Use scripts/dev-install.sh for managed recovery; CHITTA_ALLOW_MCP_KILL=1 is the explicit environment bypass."}}'
            return 3
        done < <(tr ';&\n' '\n' <<<"$_mcp_cmd")
    fi
    if echo "$cmd" | grep -qE '^\s*rm\s+-rf\s+(/|~/?\s*$)'; then
        echo '{"hookSpecificOutput":{"hookEventName":"PreToolUse","permissionDecision":"block","additionalContext":"rm -rf on / or ~ is destructive"}}'
        return 2
    fi
    if echo "$cmd" | grep -qE '^\s*chmod\s+-R\s+777\s+/\s*$'; then
        echo '{"hookSpecificOutput":{"hookEventName":"PreToolUse","permissionDecision":"block","additionalContext":"chmod -R 777 / is destructive"}}'
        return 2
    fi
    if echo "$cmd" | grep -qE '^\s*dd\s+.*of=/dev/sd[a-z]'; then
        echo '{"hookSpecificOutput":{"hookEventName":"PreToolUse","permissionDecision":"block","additionalContext":"dd writing to raw disk device"}}'
        return 2
    fi
    # Glob/bulk-delete guard. Incident 2026-07-30: an agent ran
    # `rm -f chunk_*.sorted.fq` in the repo cwd, deleting 204 untracked files.
    # Blocks wildcard rm, find -delete/-exec rm, xargs rm, and git clean -f
    # UNLESS a scratch/temp path (/tmp|/dev/shm|/scratch) appears in the same
    # command segment. Explicit single-path deletes and git-clean dry-runs pass.
    # Command is split on ; & | so a scratch path in one segment can't unlock a
    # destructive delete in another. Bypass: prefix CHITTA_ALLOW_GLOB_RM=1
    # (CC_SOUL_ALLOW_GLOB_RM=1 still honored).
    if [[ "${CHITTA_ALLOW_GLOB_RM:-${CC_SOUL_ALLOW_GLOB_RM:-0}}" != "1" ]] && ! grep -qE '(^|[[:space:]])(CHITTA|CC_SOUL)_ALLOW_GLOB_RM=1([[:space:]]|$)' <<<"$cmd"; then
        local _seg _danger=0
        while IFS= read -r _seg; do
            [[ -z "$_seg" ]] && continue
            grep -qE '(^|[^[:alnum:]_.-])(rm(dir)?[[:space:]][^|;&]*[][*?]|find[[:space:]][^|;&]*(-delete([[:space:]]|$)|-exec(dir)?[[:space:]]+rm)|xargs\b[^|]*[[:space:]]rm([[:space:]]|$)|git[[:space:]]+clean[[:space:]][^|;&]*-[[:alnum:]]*f)' <<<"$_seg" || continue
            grep -qE '(/tmp/|/dev/shm/|/scratch/)' <<<"$_seg" && continue
            grep -qE 'git[[:space:]]+clean[^|;&]*(--dry-run|-[[:alnum:]]*n)' <<<"$_seg" && continue
            _danger=1; break
        done < <(tr ';&|\n' '\n' <<<"$cmd")
        if [[ $_danger -eq 1 ]]; then
            echo '{"hookSpecificOutput":{"hookEventName":"PreToolUse","permissionDecision":"deny","permissionDecisionReason":"Bulk/glob delete outside a scratch path blocked (incident 2026-07-30: rm -f chunk_*.sorted.fq deleted 204 untracked files). Delete explicit paths, scope to /tmp|/scratch|/dev/shm, or prefix the command with CHITTA_ALLOW_GLOB_RM=1 if intentional."}}'
            return 3
        fi
    fi
    return 0
}


# ─── find: memory-first search strategy ──────────────────────────────────────
# 1. chitta recall  → if known, deny + show path
# 2. folder in mem  → rewrite find root to mem dir, maxdepth 3
# 3. cwd            → rewrite to find . -maxdepth 3
# 4. expand         → allow if CHITTA_DEEP_SEARCH=1
find_strategy() {
    local cmd="$1"
    # Only when find is invoked as a command (start of line or after ; | && &).
    # A bare \bfind\b substring match hijacked python heredocs containing
    # .find( / "find" — the entire command got replaced by the fallback.
    echo "$cmd" | grep -qE '(^|[;&|][[:space:]]*)find[[:space:]]' || return 1
    echo "$cmd" | grep -qE '\-maxdepth\s+[0-3]\b' && return 1
    [[ "${CHITTA_DEEP_SEARCH:-${CC_SOUL_DEEP_SEARCH:-0}}" == "1" ]] && return 1
    # Inline env var: CHITTA_DEEP_SEARCH=1 find ... — not exported to hook env.
    echo "$cmd" | grep -qE '(^|\s)(CHITTA|CC_SOUL)_DEEP_SEARCH=1(\s|$)' && return 1
    # Explicit non-root absolute path: find /maps/... or find /home/... — let through.
    local _target
    _target=$(echo "$cmd" | sed -nE 's/.*\bfind\s+([^[:space:]]+).*/\1/p' | head -1)
    [[ "$_target" == /* && "$_target" != "/" ]] && return 1

    # In strict mode, hard-deny explicit root scans.
    if [[ "${CHITTA_STRICT_MODE:-${CC_SOUL_STRICT_MODE:-0}}" == "1" ]] && echo "$cmd" | grep -qE '^[[:space:]]*find[[:space:]]+/([[:space:]]|$)'; then
        local deny_msg
        deny_msg=$(jq -Rn '"[strict] Root-wide find is blocked. Scope to project/cwd (e.g., find . -maxdepth 3 ...) or set CHITTA_DEEP_SEARCH=1 when intentional."')
        printf '{"hookSpecificOutput":{"hookEventName":"PreToolUse","permissionDecision":"deny","permissionDecisionReason":%s}}\n' "$deny_msg"
        return 0
    fi

    # Rewriter for the first find path token; keeps the rest of the expression.
    # This prevents unsafe broad scans like `find / ...` from passing through.
    _rewrite_find_root() {
        local src="$1" dst="$2"
        echo "$src" | sed -E "s|^([[:space:]]*)find[[:space:]]+([^[:space:]]+)([[:space:]]+.*)?$|\1find ${dst} -maxdepth 3\3|"
    }

    local term
    term=$(echo "$cmd" | sed -nE 's/.*-[i]?name[[:space:]]+([^[:space:]]+).*/\1/p' | tr -d '"'"'"'*?[]' || true)

    if [[ -n "$term" && -x "$CHITTA_BIN" ]] && daemon_available 2>/dev/null; then
        local sym_json sym_file sym_dir
        sym_json=$(timeout 2 "$CHITTA_BIN" find_symbol --name "$term" --json 2>/dev/null || true)
        if [[ -n "$sym_json" ]]; then
            sym_file=$(echo "$sym_json" | jq -r '.symbols[0].file // empty' 2>/dev/null || true)
            if [[ -n "$sym_file" && -e "$sym_file" ]]; then
                local msg
                msg=$(jq -Rn --arg t "$term" --arg p "$sym_file" \
                    '"[memory-first] chitta knows \($t): \($p) — use read_symbol/smart_context instead of find."')
                printf '{"hookSpecificOutput":{"hookEventName":"PreToolUse","permissionDecision":"deny","permissionDecisionReason":%s}}\n' "$msg"
                return 0
            fi
            sym_dir=$(echo "$sym_json" | jq -r 'if .symbols[0].file then (.symbols[0].file | split("/")[:-1] | join("/")) else empty end' 2>/dev/null || true)
            if [[ -n "$sym_dir" && -d "$sym_dir" ]]; then
                local new_cmd ctx new_cmd_json
                new_cmd=$(_rewrite_find_root "$cmd" "$sym_dir")
                ctx=$(jq -Rn --arg d "$sym_dir" '"[memory-first] No exact match but memory hints at \($d). Scoped find there (maxdepth 3). CHITTA_DEEP_SEARCH=1 to expand."')
                new_cmd_json=$(jq -Rn --arg c "$new_cmd" '$c')
                printf '{"hookSpecificOutput":{"hookEventName":"PreToolUse","additionalContext":%s,"updatedInput":{"command":%s}}}\n' "$ctx" "$new_cmd_json"
                return 0
            fi
        fi
    fi

    local new_cmd ctx new_cmd_json term_label
    new_cmd=$(_rewrite_find_root "$cmd" ".")
    # Rewriter is anchored to commands starting with find; if it didn't match,
    # pass through unchanged — NEVER substitute a synthetic find for the command.
    [[ "$new_cmd" == "$cmd" ]] && return 1
    term_label="${term:-(pattern)}"
    ctx=$(jq -Rn --arg t "$term_label" '"[search-strategy] No memory hit for \($t). Scoped to cwd -maxdepth 3. CHITTA_DEEP_SEARCH=1 to expand."')
    new_cmd_json=$(jq -Rn --arg c "$new_cmd" '$c')
    printf '{"hookSpecificOutput":{"hookEventName":"PreToolUse","additionalContext":%s,"updatedInput":{"command":%s}}}\n' "$ctx" "$new_cmd_json"
    return 0
}

case "$MATCHER" in
    Bash)
        command=$(echo "$STDIN_DATA" | jq -r '.tool_input.command // empty')
        [[ -z "$command" ]] && exit 0

        _bash_advisory=""
        # Advisory: nudge away from temp patch scripts toward file_patch.
        if echo "$command" | grep -qE '(python3?|bash)\s+(/tmp/|/maps/[^[:space:]]*/scratch/)[^[:space:]]+\.(py|sh)'; then
            _bash_advisory="[code-intel] Temp patch script detected. Use the Edit tool directly — no script needed."
        elif echo "$command" | grep -qE "python3?\s+-c\s+['\"]" && \
             echo "$command" | grep -qE "(open\([^)]*['\"][wa]['\"]|\.write_text\(|Path\([^)]*\)\.write\()"; then
            _bash_advisory="[code-intel] Inline Python file-write detected. Use the Edit tool directly."
        fi

        # Stage 1a: Safety blocks
        safety_result=$(safety_check "$command")
        safety_rc=$?
        if [[ $safety_rc -eq 2 ]]; then
            echo "$safety_result"
            exit 2
        elif [[ $safety_rc -eq 3 ]]; then
            # Structured deny (permissionDecision:"deny") — echo JSON, exit 0 so the
            # reason is surfaced (same idiom as find_strategy's strict-mode deny).
            echo "$safety_result"
            exit 0
        fi

        if [[ -n "${CHITTA_HEADLESS:-$CC_SOUL_HEADLESS}" ]]; then printf '{}'; exit 0; fi

        # Stage 1b: find — memory-first search strategy
        if find_strategy "$command"; then
            exit 0
        fi

        # ── Task ledger: pre-stage analysis/long-running commands ──────────────
        _PLUGIN_DIR="${CHITTA_PLUGIN_DIR:-${CC_SOUL_PLUGIN_DIR:-$(dirname "$(dirname "$(realpath "${BASH_SOURCE[0]}")")")}}"
        _MCP_DIR="$_PLUGIN_DIR/chitta-mcp"
        _MIND_PATH="${CHITTA_DB_PATH:-${HOME}/.claude/mind}"
        _is_trackable=0
        if echo "$command" | grep -qE             '(^|\s)(sbatch|srun|bsub|qsub|nohup|screen|tmux\s+new|snakemake|nextflow)\s'             || echo "$command" | grep -qE             '(^|\s)(python3?|Rscript|julia|perl)\s+\S+\.(py|R|jl|pl)'             || echo "$command" | grep -qE             '(^|\s)bash\s+\S+\.sh(\s|$)'             || echo "$command" | grep -qE             '(^|\s)\./\S+\.(sh|py|R)(\s|$)'; then
            _is_trackable=1
        fi
        if [[ "$_is_trackable" == "1" ]]; then
            _task_id=$(python3 -c "import uuid; print(str(uuid.uuid4()))" 2>/dev/null || cat /proc/sys/kernel/random/uuid 2>/dev/null || date +%s%N)
            mkdir -p "$_MIND_PATH" 2>/dev/null
            echo "$_task_id" > "$_MIND_PATH/.pending_task_id" 2>/dev/null || true
            _cwd_pre=$(pwd 2>/dev/null || echo "")
            timeout 3 python3 "$_MCP_DIR/provenance.py" snapshot                 --cwd "$_cwd_pre" --out "$_MIND_PATH/.fs_snapshot_${_task_id}" >/dev/null 2>&1 || true
        fi
        # ── End task ledger pre-stage ─────────────────────────────────────────

        # Advisory only: include interpreter startup in the 300 ms budget.
        # The helper emits JSON only after a successful, deduplicated check.
        _saddle_mind="${CHITTA_DB_PATH:-${HOME}/.claude/mind}"
        _saddle_root="${CHITTA_PLUGIN_DIR:-${CC_SOUL_PLUGIN_DIR:-$SCRIPT_DIR/..}}"
        [[ -f "$_saddle_root/chitta-mcp/saddle_detector.py" ]] || _saddle_root=$(resolve_cc_soul_root 2>/dev/null)
        if [[ -s "$_saddle_mind/outcome_ledger.jsonl" ]]; then
            _saddle_sid=$(printf '%s' "$STDIN_DATA" | jq -r '.session_id // empty' 2>/dev/null)
            if [[ "$_saddle_sid" =~ ^[a-zA-Z0-9_-]+$ ]]; then
                _saddle_notice=$(timeout -s KILL 0.3s python3 -S "$_saddle_root/chitta-mcp/saddle_detector.py" \
                    check --session "$_saddle_sid" --cmd "$command" \
                    --ledger "$_saddle_mind/outcome_ledger.jsonl" \
                    --notice-file "$_saddle_mind/.saddle_${_saddle_sid}" 2>/dev/null) && {
                    if [[ -n "$_bash_advisory" ]]; then
                        printf '%s' "$_saddle_notice" | jq --arg advisory "$_bash_advisory" '
                            .hookSpecificOutput.additionalContext += ("\n" + $advisory)'
                    else
                        printf '%s\n' "$_saddle_notice"
                    fi
                    exit 0
                }
            fi
        fi

        if [[ -n "$_bash_advisory" ]]; then
            jq -nc --arg context "$_bash_advisory" \
                '{hookSpecificOutput:{hookEventName:"PreToolUse",additionalContext:$context}}'
        fi

        # Stage 2: Soul memory — surface corrections/gotchas
        # Skip for subagent calls by default (saves 2s timeout per tool call).
        # Set CHITTA_SUBAGENT_BASH_RECALL=1 to enable Bash recall for subagents too.
        agent_id=$(echo "$STDIN_DATA" | jq -r '.agent_id // empty')
        if [[ -n "$agent_id" && "${CHITTA_SUBAGENT_BASH_RECALL:-${CC_SOUL_SUBAGENT_BASH_RECALL:-0}}" != "1" ]]; then
            exit 0
        fi

        [[ ! -x "$CHITTA_BIN" ]] && exit 0
        daemon_available || exit 0

        # Per-turn dedup: only inject soul recall once per assistant turn.
        # Turn index is written by stop-hook; all tool calls within a turn share the same index.
        MIND_PATH="${HOME}/.claude/mind"
        _session_id=$(echo "$STDIN_DATA" | jq -r '.session_id // empty')
        if [[ -n "$_session_id" ]]; then
            _turn=$(cat "$MIND_PATH/.turn_index_${_session_id}" 2>/dev/null || echo 0)
            _soul_sentinel="$MIND_PATH/.soul_injected_${_session_id}_${_turn}"
            [[ -f "$_soul_sentinel" ]] && exit 0
            touch "$_soul_sentinel" 2>/dev/null || true
        fi

        query=""
        tags=""

        if echo "$command" | grep -qiE '(Rscript|R --vanilla|R -e|module.*load.*R)'; then
            query="R environment conda activation"; tags="correction"
        elif echo "$command" | grep -qiE '(python3?[[:space:]]|pip[[:space:]]|conda activate)'; then
            query="python conda environment"; tags="correction"
        elif echo "$command" | grep -qiE '(chittad|daemon)'; then
            query="chittad daemon startup"; tags="correction,gotcha"
        elif echo "$command" | grep -qiE '(git push|git commit|git rebase)'; then
            query="git push commit"; tags="gotcha"
        elif echo "$command" | grep -qiE '(nohup|&$)'; then
            query="background daemon nohup"; tags="correction"
        elif echo "$command" | grep -qiE '(release\.sh|version)'; then
            query="release version bump"; tags="gotcha"
        elif echo "$command" | grep -qiE '(cmake|make|build)'; then
            query="build cmake"; tags="solution"
        fi

        if [[ -n "$query" ]]; then
            escaped_query=$(json_escape "$query")
            # Scope recall to current project realm to prevent cross-session memory bleed.
            _recall_realm=$(timeout 1 "$CHITTA_BIN" realm_detect 2>/dev/null || echo "")
            memories=""
            # If realm detection failed (daemon warming/slow → realm_detect times out and
            # returns empty), SKIP injection entirely. Never fall back to an unscoped recall:
            # that bleeds other projects' memories (e.g. bioinformatics corrections) into an
            # unrelated session — the actual cause of the "hooks misfiring" noise. Honours the
            # "No fallback unfiltered recall" intent below, which the old else-branch violated.
            if [[ -n "$_recall_realm" ]]; then
                for tag in ${tags//,/ }; do
                    # A tag filter with a canned query returns the nearest tagged row no
                    # matter how far it is; the tag fallback even reports similarity 0.
                    # Inject only rows the semantic leg actually matched.
                    result=$(timeout 2 "$CHITTA_BIN" recall --json --query "$escaped_query" --tag "$tag" --realm "$_recall_realm" --limit 1 2>/dev/null \
                        | jq -r --argjson min "${CHITTA_PRETOOL_MIN_SIM:-0.6}" '.results[]? | select((.similarity // 0) >= $min) | .text' 2>/dev/null | head -c 400)
                    [[ -n "$result" ]] && memories="$memories$result\n"
                done
            fi
            # No fallback unfiltered recall — avoids cross-domain memory bleed
            if [[ -n "$memories" ]]; then
                escaped_mem=$(json_escape "$memories")
                echo "{\"hookSpecificOutput\":{\"hookEventName\":\"PreToolUse\",\"additionalContext\":\"⚠️ BEFORE RUNNING: $escaped_mem\"}}"
            fi
        fi
        ;;
    Read)
        file_path=$(echo "$STDIN_DATA" | jq -r '.tool_input.file_path // empty')
        [[ -z "$file_path" || ! -f "$file_path" ]] && exit 0
        # Assigned up-front: used by the .allow_read_<id> escape hatch below (was
        # previously referenced before assignment → flag-file path lost its suffix).
        _session_id=$(echo "$STDIN_DATA" | jq -r '.session_id // empty' 2>/dev/null || true)

        # Code intelligence advisory: if chitta has this file's directory indexed, suggest smart_context/read_symbol
        advisory=""
        if [[ -x "$CHITTA_BIN" ]] && daemon_available; then
            dir_path=$(dirname "$file_path")
            dir_syms=$(timeout 1 "$CHITTA_BIN" code_context --path "$dir_path" --json 2>/dev/null | jq -r '.dir_symbols // 0' 2>/dev/null || echo 0)
            if [[ "$dir_syms" -gt 0 ]]; then
                advisory="[code-intel] File is indexed in chitta ($dir_syms symbols in dir). For large files prefer smart_context(task) → read_symbol(file,symbol) over full Read. Whole-symbol rewrites: symbol_patch(file,symbol,body)."
            fi
        fi

        # Only compress large files (≤200 lines pass through untouched)
        line_count=$(wc -l < "$file_path" 2>/dev/null || echo 0)
        is_indexed=0
        [[ "$dir_syms" -gt 0 ]] 2>/dev/null && is_indexed=1

        # Resolve CHITTA_ALLOW_READ from env or persistent flag file.
        # Flag file survives across hook invocations (each is a separate shell).
        _allow_read="${CHITTA_ALLOW_READ:-${CC_SOUL_ALLOW_READ:-0}}"
        _allow_read_flag="${CHITTA_DB_PATH:-${HOME}/.claude/mind}/.allow_read_${_session_id}"
        [[ "$_allow_read" != "1" && -f "$_allow_read_flag" ]] && _allow_read=1

        # Strict mode: enforce symbol-level flow for indexed files.
        # Safety valve: never block reads for system/external paths where no chitta
        # alternative exists — blocking those forces dangerous workarounds
        # (e.g. patching site-packages via raw Python instead of Read+Edit).
        _is_system_path=0
        case "$file_path" in
            */site-packages/*|*/dist-packages/*|*/conda/envs/*/lib/*|/usr/lib/*|/opt/*/lib/*)
                _is_system_path=1 ;;
        esac
        if _strict_mode_enabled && [[ "$is_indexed" == "1" && "$_allow_read" != "1" && "$_is_system_path" == "0" ]]; then
            _shadow_log "Read" "$file_path" "$line_count" "$is_indexed" "advisory" "strict-indexed-read" 0
            printf '{"hookSpecificOutput":{"hookEventName":"PreToolUse","additionalContext":"[code-intel] Indexed file. Prefer mcp__chitta-bridge__read_symbol/smart_context over full Read for large files."}}\n'
        fi

        # ─── Read dedup: route re-reads of unchanged files to sqz ────────────────
        if [[ "$line_count" -gt 200 && -n "$_session_id" && "$_allow_read" != "1" && "$_is_system_path" == "0" ]]; then
            _file_mtime=$(stat -c %Y "$file_path" 2>/dev/null || echo 0)
            _file_hash=$(printf '%s:%s' "$file_path" "$_file_mtime" | md5sum | cut -c1-16)
            _read_cache="${CHITTA_DB_PATH:-${HOME}/.claude/mind}/.read_cache_${_session_id}"
            if grep -qF "$_file_hash" "$_read_cache" 2>/dev/null; then
                _dd_offset=$(echo "$STDIN_DATA" | jq -r '.tool_input.offset // 0')
                if _should_enforce; then
                    # Enforce (safe): a re-read of an UNCHANGED file this session is almost
                    # always redundant (content already in context / sqz cache). TRUNCATE to
                    # 40 lines rather than hard-deny — a Read still occurs so Edit's
                    # read-precondition stays satisfied, and any genuinely-needed bytes are
                    # recoverable via offset or sqz_read_file. This matters because silent
                    # context compaction is frequent: a post-compaction re-read is legitimate,
                    # so it must never be blocked — only trimmed. Escape hatch: CHITTA_ALLOW_READ=1
                    # or the .allow_read_<id> flag (resolved above) bypasses this whole block.
                    _shadow_log "Read" "$file_path" "$line_count" "$is_indexed" "truncate" "read-dedup" 1
                    _dd_path=$(echo -n "$file_path" | jq -Rs '.')
                    printf '{"hookSpecificOutput":{"hookEventName":"PreToolUse","additionalContext":"[read-dedup] %s already read this session (%d lines) — returning 40. Use sqz_read_file for the full cached content (13-token §ref§), or Read with offset for a specific range.","updatedInput":{"file_path":%s,"limit":40,"offset":%s}}}' \
                        "$(basename "$file_path")" "$line_count" "$_dd_path" "$_dd_offset"
                    printf '%s\n' "$_file_hash" >> "$_read_cache" 2>/dev/null || true
                    exit 0
                else
                    _shadow_log "Read" "$file_path" "$line_count" "$is_indexed" "advisory" "read-dedup" 0
                    printf '{"hookSpecificOutput":{"hookEventName":"PreToolUse","additionalContext":"[read-dedup] %s already read this session (%d lines). sqz_read_file returns a 13-token §ref§ for cached content."}}\n' \
                        "$(basename "$file_path")" "$line_count"
                fi
            fi
            printf '%s\n' "$_file_hash" >> "$_read_cache" 2>/dev/null || true
        fi
        # ─────────────────────────────────────────────────────────────────────────

        if [[ "$line_count" -le 200 ]]; then
            # Small file — pass through, but still advise code-intel if available
            _shadow_log "Read" "$file_path" "$line_count" "$is_indexed" "pass" "small-file" 0
            if [[ -n "$advisory" ]]; then
                printf '{"hookSpecificOutput":{"hookEventName":"PreToolUse","additionalContext":"%s"}}' "$advisory"
            fi
            exit 0
        fi

        # Use updatedInput to limit Read to 150 lines + advisory context.
        offset=$(echo "$STDIN_DATA" | jq -r '.tool_input.offset // 0')
        # Advisory: large indexed file — nudge toward read_symbol/smart_context.
        if [[ "$is_indexed" == "1" && "$offset" == "0" ]]; then
            _shadow_log "Read" "$file_path" "$line_count" "$is_indexed" "advisory" "indexed-large-offset0" 0
            advisory="[code-intel] Large indexed file ($line_count lines). Prefer read_symbol or smart_context for targeted extraction."
        fi
        _shadow_log "Read" "$file_path" "$line_count" "$is_indexed" "truncate" "large-file" 0
        escaped_path=$(echo -n "$file_path" | jq -Rs '.')
        if [[ -n "$advisory" ]]; then
            printf '{"hookSpecificOutput":{"hookEventName":"PreToolUse","additionalContext":"[hook] Large file (%d lines). Reading first 150. Use offset for later sections. %s","updatedInput":{"file_path":%s,"limit":150,"offset":%s}}}' \
                "$line_count" "$advisory" "$escaped_path" "$offset"
        else
            printf '{"hookSpecificOutput":{"hookEventName":"PreToolUse","additionalContext":"[hook] Large file (%d lines). Reading first 150. Use offset for later sections.","updatedInput":{"file_path":%s,"limit":150,"offset":%s}}}' \
                "$line_count" "$escaped_path" "$offset"
        fi
        ;;

    Grep)
        output_mode=$(echo "$STDIN_DATA" | jq -r '.tool_input.output_mode // "files_with_matches"')
        [[ "$output_mode" != "content" ]] && exit 0

        # Inject head_limit:50 into tool input — no pre-run, native tool handles once.
        updated=$(echo "$STDIN_DATA" | jq '.tool_input + {"head_limit": 50}')
        printf '{"hookSpecificOutput":{"hookEventName":"PreToolUse","additionalContext":"[hook] Grep content mode — capped at 50 matches.","updatedInput":%s}}' "$updated"
        ;;

    Glob)
        pattern=$(echo "$STDIN_DATA" | jq -r '.tool_input.pattern // empty')
        [[ -z "$pattern" ]] && exit 0

        # Inject head_limit:100 — Glob returns sorted by mtime, 100 is usually enough.
        updated=$(echo "$STDIN_DATA" | jq '.tool_input + {"head_limit": 100}')
        printf '{"hookSpecificOutput":{"hookEventName":"PreToolUse","updatedInput":%s}}' "$updated"
        ;;

    Write)
        FP_BIN="${HOME}/.claude/bin/fp"
        # Block Python/shell patch scripts written to temp/scratch locations.
        # Check path first (cheap); only extract content if path matches — avoids
        # jq + grep on large payloads which hangs the hook for sizeable writes.
        _wp_path=$(echo "$STDIN_DATA" | jq -r '.tool_input.file_path // empty' 2>/dev/null)
        if [[ "$_wp_path" =~ \.(py|sh)$ ]]; then
            _is_temp=0
            case "$_wp_path" in
                /tmp/*|*/scratch/*|*/tmp/*) _is_temp=1 ;;
                *patch*|*fix_*|*edit_*|*_fix.*|*_edit.*) _is_temp=1 ;;
            esac
            if [[ "$_is_temp" == "1" ]]; then
                _wp_content=$(echo "$STDIN_DATA" | jq -r '.tool_input.content // empty' 2>/dev/null)
                if echo "$_wp_content" | grep -qE \
                    "(open\([^)]*['\"][wa]['\"]|\.write_text\(|Path\([^)]*\)\.write\(|\.write\()"; then
                    printf '{"hookSpecificOutput":{"hookEventName":"PreToolUse","additionalContext":"[code-intel] Temp patch script detected. Use the Edit tool directly — no script needed."}}\n'
                fi
            fi
        fi
        [[ ! -x "$FP_BIN" ]] && exit 0
        # Block Write on existing files ≥50 lines — suggest file_patch instead.
        echo "$STDIN_DATA" | "$FP_BIN" --write-hook
        exit $?
        ;;

    Agent)
        # ─── Subagent budget + model routing ─────────────────────────────────────
        MIND_PATH="${CHITTA_DB_PATH:-${HOME}/.claude/mind}"
        _session_id=$(echo "$STDIN_DATA" | jq -r '.session_id // empty')
        _subtype=$(echo "$STDIN_DATA" | jq -r '.tool_input.subagent_type // empty' 2>/dev/null)
        _desc=$(echo "$STDIN_DATA" | jq -r '.tool_input.description // empty' 2>/dev/null)
        agent_model=$(echo "$STDIN_DATA" | jq -r '.tool_input.model // empty' 2>/dev/null)

        # Route search/lookup/research agents to haiku + inject ≤200 word limit.
        # Uses updatedInput (no deny+retry round trip). Bypass: CHITTA_AGENT_NO_FORCE=1.
        #
        # Two hard exemptions:
        #   1. An explicit .model is never rewritten. Any value the Agent tool accepts
        #      (sonnet|opus|haiku|fable) passes through untouched — asking for fable
        #      on a research agent is a deliberate choice, not a mistake to correct.
        #   2. subagent_type "fork" is never rewritten. A fork inherits the parent
        #      model by definition (the Agent tool ignores `model` for forks), so
        #      stamping haiku on it would silently contradict the tool contract.
        if [[ -z "$agent_model" && "$_subtype" != "fork" \
              && "${CHITTA_AGENT_NO_FORCE:-${CC_SOUL_AGENT_NO_FORCE:-0}}" != "1" ]]; then
            if echo "${_subtype} ${_desc}" | grep -qiE '(explore|search|find|research|grep|glob|read|locate|list|lookup|where|enumerate|check if)'; then
                _prompt=$(echo "$STDIN_DATA" | jq -r '.tool_input.prompt // empty')
                _updated=$(echo "$STDIN_DATA" | jq --arg p "Report in ≤200 words.\n\n${_prompt}" \
                    '.tool_input | .model = "haiku" | .prompt = $p')
                printf '{"hookSpecificOutput":{"hookEventName":"PreToolUse","additionalContext":"[token-route] lookup→haiku (4.5) + ≤200 word limit. Pass model=sonnet when the agent must reason, model=opus for architecture, model=fable to match the orchestrator; subagent_type=fork always inherits fable.","updatedInput":%s}}\n' "$_updated"
                exit 0
            fi
        fi

        if [[ -n "$_session_id" ]]; then
            AGENT_COUNT_FILE="$MIND_PATH/.subagent_count_${_session_id}"
            AGENT_COUNT=$(cat "$AGENT_COUNT_FILE" 2>/dev/null || echo 0)
            AGENT_COUNT=$((AGENT_COUNT + 1))
            echo "$AGENT_COUNT" > "$AGENT_COUNT_FILE"

            AGENT_WARN_THRESHOLD="${CHITTA_AGENT_WARN:-${CC_SOUL_AGENT_WARN:-20}}"
            AGENT_HARD_LIMIT="${CHITTA_AGENT_LIMIT:-${CC_SOUL_AGENT_LIMIT:-50}}"

            if [[ $AGENT_COUNT -gt $AGENT_HARD_LIMIT ]]; then
                printf '{"hookSpecificOutput":{"hookEventName":"PreToolUse","additionalContext":"[agent-budget] %d/%d subagents. Each cold-starts a cache (~$5-50). Batch work or /recap for fresh session."}}\n' \
                    "$AGENT_COUNT" "$AGENT_HARD_LIMIT"
            elif [[ $AGENT_COUNT -gt $AGENT_WARN_THRESHOLD ]]; then
                printf '{"hookSpecificOutput":{"hookEventName":"PreToolUse","additionalContext":"[agent-budget] %d subagents this session. Batch independent queries where possible."}}\n' \
                    "$AGENT_COUNT"
            fi
        fi
        ;;

    ScheduleWakeup)
        # ─── Loop-budget guard ────────────────────────────────────────────────────
        # Warn at CHITTA_LOOP_WARN (default 10), block at CHITTA_LOOP_LIMIT (default 20)
        # for autonomous-loop-dynamic re-fires. Legitimate poll wakeups get a softer total advisory.
        MIND_PATH="${CHITTA_DB_PATH:-${HOME}/.claude/mind}"
        _session_id=$(echo "$STDIN_DATA" | jq -r '.session_id // empty' 2>/dev/null)
        _sw_prompt=$(echo "$STDIN_DATA" | jq -r '.tool_input.prompt // empty' 2>/dev/null)

        if [[ -n "$_session_id" ]]; then
            WAKEUP_FILE="$MIND_PATH/.wakeup_count_${_session_id}"
            WAKEUP_COUNT=$(cat "$WAKEUP_FILE" 2>/dev/null || echo 0)
            WAKEUP_COUNT=$((WAKEUP_COUNT + 1))
            echo "$WAKEUP_COUNT" > "$WAKEUP_FILE"

            if [[ "$_sw_prompt" == "<<autonomous-loop-dynamic>>" ]]; then
                LOOP_FILE="$MIND_PATH/.loop_count_${_session_id}"
                LOOP_COUNT=$(cat "$LOOP_FILE" 2>/dev/null || echo 0)
                LOOP_COUNT=$((LOOP_COUNT + 1))
                echo "$LOOP_COUNT" > "$LOOP_FILE"

                LOOP_WARN="${CHITTA_LOOP_WARN:-${CC_SOUL_LOOP_WARN:-10}}"
                LOOP_LIMIT="${CHITTA_LOOP_LIMIT:-${CC_SOUL_LOOP_LIMIT:-20}}"

                if [[ $LOOP_COUNT -gt $LOOP_LIMIT ]]; then
                    printf '{"hookSpecificOutput":{"hookEventName":"PreToolUse","permissionDecision":"block","additionalContext":"[loop-budget] %d autonomous loop iterations this session (limit %d). Use /compact then restart the loop, or set CHITTA_LOOP_LIMIT=N to raise."}}\n' \
                        "$LOOP_COUNT" "$LOOP_LIMIT"
                    exit 2
                elif [[ $LOOP_COUNT -gt $LOOP_WARN ]]; then
                    printf '{"hookSpecificOutput":{"hookEventName":"PreToolUse","additionalContext":"[loop-budget] %d autonomous loop iterations this session (warn at %d, limit %d). Consider /compact to reset context."}}\n' \
                        "$LOOP_COUNT" "$LOOP_WARN" "$LOOP_LIMIT"
                fi
            elif [[ $WAKEUP_COUNT -gt 30 ]]; then
                printf '{"hookSpecificOutput":{"hookEventName":"PreToolUse","additionalContext":"[loop-budget] %d total wakeups this session. Verify the loop has a termination condition."}}\n' \
                    "$WAKEUP_COUNT"
            fi
        fi
        ;;

    *)
        exit 0
        ;;
esac

exit 0
