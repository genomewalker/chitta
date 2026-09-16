#!/usr/bin/env bash
# Real hook entry point, bounded mock transport, and isolated session markers.
set -euo pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
unset CHITTA_HEADLESS CC_SOUL_HEADLESS
export HOME="$T/home" XDG_RUNTIME_DIR="$T/run" CHITTA_DB_PATH="$T/mind"
export CHITTA_QUEUE="$T/queue" CHITTA_SOCKET_PATH="$T/no-daemon.sock"
export HOOK_STATE_DIR="$T/state" CHITTA_BIN="$T/cli" CODE_NAV_FIXTURE="$T"
export CHITTA_CODE_NAV_REFRESH=0
mkdir -p "$HOME/.claude/mind" "$T/repo" "$T/state"
git -C "$T/repo" init -q
printf 'def alpha():\n    return beta()\n' > "$T/repo/a.py"
printf 'ordinary fixture\n' > "$T/repo/a.txt"
cat > "$CHITTA_BIN" <<'STUB'
#!/usr/bin/env bash
printf '%s\n' "$*" >> "$CODE_NAV_FIXTURE/calls"
[[ ! -f "$CODE_NAV_FIXTURE/slow" ]] || sleep 2
case "$1" in
    code_query) cat "$CODE_NAV_FIXTURE/query" ;;
    codebase_overview) cat "$CODE_NAV_FIXTURE/map" ;;
    learn_codebase) : ;;
esac
STUB
chmod +x "$CHITTA_BIN"
jq -nc --arg text '[code-nav] index 7 minutes old; STALE — refresh with learn_codebase
repo/a.py:1 alpha()
  in ["calls:caller[INFERRED]"] out ["calls:beta[EXTRACTED]"]
  read_symbol {"name":"alpha","path":"repo/a.py","line":1}' '{indexed:true,text:$text,stale:true}' > "$T/query"
jq -nc --arg text '[code-nav] 1 files; index 7 minutes old
Communities: repo
God nodes: alpha' '{indexed:true,text:$text}' > "$T/map"
jq -nc --arg path "$T/repo/a.py" '{session_id:"nav-test",tool_input:{file_path:$path}}' |
    bash "$ROOT/hooks/pre-tool-hook.sh" Read > "$T/read"
jq -se 'length == 1 and (.[0].hookSpecificOutput.additionalContext | contains("read_symbol") and contains("STALE") and contains("INFERRED") and (contains("For large files prefer") | not))' "$T/read" >/dev/null
bash "$ROOT/hooks/code-nav.sh" session "$T/repo" test > "$T/first"
bash "$ROOT/hooks/code-nav.sh" session "$T/repo" test > "$T/second"
grep -q 'Communities' "$T/first"
grep -q 'God nodes' "$T/first"
[[ ! -s "$T/second" ]]
n=$(wc -l < "$T/calls")
if bash "$ROOT/hooks/code-nav.sh" read "$T/repo/a.txt" test > "$T/noncode"; then exit 1; fi
[[ ! -s "$T/noncode" && $(wc -l < "$T/calls") == "$n" ]]
printf '{"indexed":false}\n' > "$T/query"
if bash "$ROOT/hooks/code-nav.sh" read "$T/repo/a.py" test > "$T/missing"; then exit 1; fi
[[ ! -s "$T/missing" ]]
touch "$T/slow"
CHITTA_CODE_NAV_BUDGET_MS=30 timeout 1 bash "$ROOT/hooks/code-nav.sh" read "$T/repo/a.py" test > "$T/timeout"
jq -e '.hookSpecificOutput.additionalContext | contains("unavailable")' "$T/timeout" >/dev/null
rm "$T/slow"
CHITTA_CODE_NAV_REFRESH=1 bash "$ROOT/hooks/code-nav.sh" session "$T/repo" refresh > /dev/null
for _ in {1..30}; do grep -q '^learn_codebase --path ' "$T/calls" && break; sleep .02; done
grep -q '^learn_codebase --path ' "$T/calls"
if grep -q -- '--max_files' "$T/calls"; then exit 1; fi
CHITTA_CODE_NAV=0 bash "$ROOT/hooks/code-nav.sh" read "$T/repo/a.py" test > "$T/off" && exit 1
[[ ! -s "$T/off" ]]
printf 'code-nav: indexed Read, stale warning, once/session map, noncode, timeout and uncapped refresh passed\n'
