#!/usr/bin/env bash
# Capsule mechanics only. These synthetic cases are not the real-thread exit panel.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
source "$ROOT/hooks/tests/ledger-fixture.inc"
mkdir -p "$T/repo with spaces"
git -c init.templateDir= init -q -b capsule-test "$T/repo with spaces"
# Source only the production function definitions; no hook side effects.
sed -n '/^_save_handoff_capsule() {/,/^}/p' "$ROOT/hooks/stop-core.sh" > "$T/functions"
# Native card entrypoint is now inside hook_session_start; exercise its pure production helper.
cat >> "$T/functions" <<'FUNCTION'
_load_handoff_capsule() {
    local branch
    branch=$(git -C "$PROJECT_DIR" symbolic-ref --quiet --short HEAD)
    "$CHITTA_BIN" ledger_op --op hook_handoff_context --args "$(jq -nc --arg project "$PROJECT_DIR" --arg branch "$branch" '{project_dir:$project,branch:$branch}')" | jq -r '.value.text|select(length>0)'
}
FUNCTION
# shellcheck source=/dev/null
source "$T/functions"
export STUB_HANDOFF_DIR="$T"
CHITTA_BIN="$T/cli"
cat > "$CHITTA_BIN" <<'STUB'
#!/usr/bin/env bash
op="$3"; args="$5"
[[ "${STUB_TIMEOUT:-0}" != 1 ]] || exit 124
case "$op" in
    hook_handoff_prepare)
        jq -nc --arg op "$op" --argjson args "$args" --slurpfile thread "$STUB_HANDOFF_DIR/thread.json" \
          '{op:$op,args:$args,session:{thread_id:"real-thread-id"},thread:$thread[0].value}' ;;
    hook_handoff_context)
        jq -nc --arg op "$op" --argjson args "$args" --slurpfile row "$STUB_HANDOFF_DIR/queued.json" \
          '{op:$op,args:$args,rows:[{metadata_json:($row[0].args.metadata|tojson)}]}' ;;
    *) exit 1 ;;
esac | "$LEDGER_TEST_BIN"

STUB
chmod +x "$CHITTA_BIN"
queue_write() { [[ "$1" == ledger_op ]]; printf '%s\n' "$2" > "$T/queued.json"; }
transcript_role_text() { printf '%s\n' "$VISIBLE"; }
PROJECT_DIR="$T/repo with spaces"
SESSION_ID=handoff-test
MAX_WAIT=2
INPUT=$(jq -nc --arg cwd "$PROJECT_DIR" '{cwd:$cwd}')
FILES_JSON='["src/a file.cpp","docs/guide.md","read-only-file"]'
mkdir -p "$PROJECT_DIR/src" "$PROJECT_DIR/docs"
touch "$PROJECT_DIR/src/a file.cpp" "$PROJECT_DIR/docs/guide.md"
printf '{"value":{"metadata_json":"{}"}}' > "$T/thread.json"
VISIBLE=$'Next: obsolete plan\n```sh\nNext: do not execute quoted code\n```\n> Next: quoted claim\nNext action: run the focused tests\nBlocker: waiting for fixture data\nclosing prose'
_save_handoff_capsule
jq -e '.args.metadata.handoff | .version == 2 and .state == "in_progress" and .next_action == "Next action: run the focused tests" and .branch == "capsule-test" and (.dirty_paths|length) == 2' "$T/queued.json" >/dev/null
_load_handoff_capsule > "$T/rendered"
[[ $(head -1 "$T/rendered") == '[handoff]' ]]
grep -q 'run the focused tests' "$T/rendered"
grep -q 'waiting for fixture data' "$T/rendered"
echo 'ok: last explicit visible plan round-trips with branch, artifacts and blocker'
# A later completed turn without a plan invalidates the old next action.
VISIBLE='Done.'
_save_handoff_capsule
[[ -z $(_load_handoff_capsule) ]]
echo 'ok: no invented action and no resurrection of a completed plan'
# A real ledger field is an eligible fallback; the title alone is not.
printf '%s\n' '{"value":{"title":"Do not treat a title as a plan","metadata_json":"{\"next_action\":\"inspect the failing assertion\"}"}}' > "$T/thread.json"
_save_handoff_capsule
jq -e '.args.metadata.handoff | .next_action == "inspect the failing assertion"' "$T/queued.json" >/dev/null
echo 'ok: explicit ledger next action retains its source thread'
# Another branch must not receive this continuation.
git -C "$PROJECT_DIR" symbolic-ref HEAD refs/heads/another-branch
[[ -z $(_load_handoff_capsule) ]]
echo 'ok: branch mismatch does not inject a continuation'

# Timeout must not reconstruct a capsule or issue legacy ledger reads.
cp "$T/queued.json" "$T/before.json"
if STUB_TIMEOUT=1 _save_handoff_capsule; then exit 1; fi
cmp "$T/before.json" "$T/queued.json"
echo 'ok: timeout leaves the acknowledged local queue unchanged'
