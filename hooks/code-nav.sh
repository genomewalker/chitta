#!/usr/bin/env bash
# Bounded structural context; derived graph data never changes recall scoring.
set -uo pipefail
[[ "${CHITTA_CODE_NAV:-1}" != 0 ]] || exit 1
mode=${1:-read} path=${2:-} session=${3:-}
bin=${CHITTA_BIN:-$HOME/.claude/bin/chitta}
[[ -x "$bin" && -n "$path" ]] || exit 1
if [[ "$mode" == read ]]; then
    case "$path" in
        *.kt|*.kts|*.scala|*.sc|*.zig|*.tf|*.tfvars|*.hcl|*.php|*.phtml|*.sql|*.ddl|*.cmake|*/CMakeLists.txt|CMakeLists.txt|*.mk|*.mak|*/Makefile|*/makefile|*/GNUmakefile|*/Makefile.*|Makefile|makefile|GNUmakefile|*.pl|*.pm|*.t|*.perl|*.smk|*/Snakefile|*/snakefile|Snakefile|snakefile|*.nf|*.[fF]|*.[fF][oO][rR]|*.[fF]77|*.[fF]90|*.[fF]95|*.[fF]03|*.[fF]08|*.jl|*.R|*.r|*.Rprofile|*.sh|*.bash|*.c|*.h|*.cpp|*.hpp|*.cc|*.cxx|*.hxx|*.py|*.pyw|*.js|*.jsx|*.mjs|*.ts|*.tsx|*.go|*.rs|*.java|*.rb|*.cs|*.swift|*.lua|*.md|*.markdown|*.mdown) ;;
        *) exit 1 ;;
    esac
    root=$(git -C "$(dirname "$path")" rev-parse --show-toplevel 2>/dev/null) || exit 1
else
    root=$(git -C "$path" rev-parse --show-toplevel 2>/dev/null) || exit 1
fi
budget=${CHITTA_CODE_NAV_BUDGET_MS:-300}
[[ "$budget" =~ ^[1-9][0-9]*$ ]] || budget=300
(( budget > 1000 )) && budget=1000
printf -v deadline '%d.%03d' "$((budget / 1000))" "$((budget % 1000))"
state=${HOOK_STATE_DIR:-${XDG_RUNTIME_DIR:-/tmp}/chitta-code-nav-${UID}}
mkdir -p "$state" 2>/dev/null || exit 1
key=$(printf '%s\n%s' "$root" "$session" | sha256sum | cut -c1-24)
marker=$state/.code_nav_$key
repo_key=$(printf %s "$root" | sha256sum | cut -c1-24)
indexed_marker=$state/.code_nav_indexed_$repo_key
valid=0
if [[ "$mode" == session ]]; then
    [[ -n "$session" && ! -e "$marker" ]] || exit 0
    tool=codebase_overview
    args=(--path "$root")
else
    tool=code_query
    args=(--path "$path" --limit 20)
fi
reply=$(timeout --kill-after=0.05 "$deadline" "$bin" "$tool" "${args[@]}" --json 2>/dev/null </dev/null)
rc=$?
if [[ "$mode" == session && "${CHITTA_CODE_NAV_REFRESH:-1}" != 0 ]]; then
    # A full uncapped collection repairs missing coverage; unchanged files are
    # skipped by learn_codebase. flock drops duplicate background refreshes.
    ( flock -n 9 || exit 0; timeout --foreground 60 "$bin" learn_codebase --path "$root" >/dev/null 2>&1 </dev/null ) 9>"$state/.code_nav_refresh_$repo_key" >/dev/null 2>&1 &
fi
if (( rc != 0 )) || ! jq -e 'type == "object" and (.indexed | type == "boolean")' >/dev/null 2>&1 <<< "$reply"; then
    [[ -e "$indexed_marker" ]] || exit 1
    text="[code-nav] index status unavailable (query failed or exceeded ${budget} ms); run code_query before reading files."
else
    jq -e '.indexed' >/dev/null <<< "$reply" || exit 1
    valid=1
    : > "$indexed_marker"
    text=$(jq -r '.text // empty' <<< "$reply")
    [[ -n "$text" ]] || exit 1
    if jq -e '.truncated == true' >/dev/null <<< "$reply"; then
        text+=$'\n[code-nav] Symbol list truncated; use code_query with a question to narrow it.'
    fi
fi
# Bound output without cutting a read_symbol invocation or splitting UTF-8.
text=$(printf '%s\n' "$text" | LC_ALL=C awk 'length($0) + n < 12000 {print; n += length($0)+1; next} {cut=1} END {if(cut) print "[code-nav] Context truncated; narrow code_query."}')
if [[ "$mode" == session ]]; then
    if (( valid == 0 )) || ( set -o noclobber; : > "$marker" ) 2>/dev/null; then printf '%s\n' "$text"; fi
else
    jq -nc --arg context "$text" '{hookSpecificOutput:{hookEventName:"PreToolUse",additionalContext:$context}}'
fi
