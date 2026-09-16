#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
export CHITTA_BIN="$T/cli" CHITTA_DB_PATH="$T/mind" CHITTA_QUEUE="$T/queue"
mkdir -p "$CHITTA_DB_PATH" "$T/repo"
cat > "$CHITTA_BIN" <<'STUB'
#!/usr/bin/env bash
[[ "$1" != queue_write ]] || exit 1
[[ "$1" != realm_detect ]] || echo project:fixture
exit 0
STUB
chmod +x "$CHITTA_BIN"
for event in change change unlink; do
    jq -nc --arg path "$T/repo/a quoted \"file\".md" --arg event "$event" \
        '{file_path:$path,event:$event}' | bash "$ROOT/hooks/file-changed-hook.sh" >/dev/null
done
jq -s -e '
    [.[] | select(.tool == "learn_codebase")] as $rows |
    ([$rows[] | select(.args.incremental == true)] | length) == 3 and
    ([$rows[] | select(.args.incremental != true)] | length) == 1
' "$CHITTA_QUEUE" >/dev/null
echo 'ok: index refresh includes edits and deletion; symbol extraction stays throttled'
