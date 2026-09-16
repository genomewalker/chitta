#!/usr/bin/env bash
# Capsule mechanics only. These synthetic cases are not the real-thread exit panel.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
mkdir -p "$T/repo with spaces"
git -c init.templateDir= init -q -b capsule-test "$T/repo with spaces"
# Source only the production function definitions; no hook side effects.
sed -n '/^_save_handoff_capsule() {/,/^}/p' "$ROOT/hooks/stop-core.sh" > "$T/functions"
sed -n '/^_load_handoff_capsule() {/,/^}/p' "$ROOT/hooks/session-start-hook.sh" >> "$T/functions"
# shellcheck source=/dev/null
source "$T/functions"
export STUB_HANDOFF_DIR="$T"
CHITTA_BIN="$T/cli"
cat > "$CHITTA_BIN" <<'STUB'
#!/usr/bin/env bash
case "$3" in
    session_get) printf '{"value":{"thread_id":"real-thread-id"}}' ;;
    thread_get) cat "$STUB_HANDOFF_DIR/thread.json" ;;
    session_list) jq -n --slurpfile op "$STUB_HANDOFF_DIR/queued.json" \
        '{value:{rows:[{metadata_json:($op[0].args.metadata|tojson)}]}}' ;;
esac
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
jq -e '.args.metadata.handoff | .verified == true and .next_action == "Next action: run the focused tests" and .branch == "capsule-test" and (.artifact_paths|length) == 2 and .source.kind == "visible_plan"' "$T/queued.json" >/dev/null
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
jq -e '.args.metadata.handoff | .next_action == "inspect the failing assertion" and .source.kind == "ledger_thread" and .source.thread_id == "real-thread-id"' "$T/queued.json" >/dev/null
echo 'ok: explicit ledger next action retains its source thread'
# Another branch must not receive this continuation.
git -C "$PROJECT_DIR" symbolic-ref HEAD refs/heads/another-branch
[[ -z $(_load_handoff_capsule) ]]
echo 'ok: branch mismatch does not inject a continuation'
