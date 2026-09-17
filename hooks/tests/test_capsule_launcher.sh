#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
mkdir -p "$T/codex-wt-test"
git -C "$T/codex-wt-test" -c init.templateDir= init -q
git -C "$T/codex-wt-test" -c user.email=test@example.com -c user.name=test commit -qm init --allow-empty
cat > "$T/cli" <<'CLI'
#!/usr/bin/env bash
[[ "$1 $2 $3" == "ledger_op --op capsule_get" ]] || exit 2
jq -e '.stream_id == "test" and (.code_head|length == 40)' <<< "$5" >/dev/null || exit 2
printf '%s\n' "$REPLY"
CLI
chmod +x "$T/cli"
export CHITTA_CODEX_WORKTREES="$T" CHITTA_CODEX_SPECS="$T/specs" CHITTA_BIN="$T/cli"
for status in missing stale head_mismatch invalidated ok; do
    REPLY=$(jq -nc --arg status "$status" '{value:{status:$status,capsule:{revision:9},manifest:{streams:[],omitted:0}}}')
    export REPLY
    if bash "$ROOT/scripts/codex-stream.sh" --check test > "$T/output"; then
        [[ "$status" == ok ]] || exit 1
    else
        [[ "$status" != ok ]] || exit 1
    fi
done
REPLY='{}'; export REPLY
if bash "$ROOT/scripts/codex-stream.sh" --check test > "$T/output" 2>&1; then exit 1; fi
echo 'launcher exact retrieval + missing/stale/HEAD/invalidation/transport rejection: PASS'
