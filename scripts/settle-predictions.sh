#!/usr/bin/env bash
# settle-predictions.sh — auto-confirm expired open predictions with no correction
# Usage: settle-predictions.sh [--realm REALM] [--dry-run]
set -euo pipefail

CHITTA_BIN="${CHITTA_BIN:-${HOME}/.claude/bin/chitta}"
REALM="${REALM:-}"
DRY_RUN=false
TODAY=$(date +%Y-%m-%d)

while [[ $# -gt 0 ]]; do
    case "$1" in
        --realm) REALM="$2"; shift 2 ;;
        --dry-run) DRY_RUN=true; shift ;;
        *) echo "Unknown arg: $1" >&2; exit 1 ;;
    esac
done

realm_args=()
[[ -n "$REALM" ]] && realm_args=(--realm "$REALM")

# Fetch all signal memories tagged prediction
raw=$("$CHITTA_BIN" list --category signal --tags prediction --json "${realm_args[@]}" 2>/dev/null || echo "[]")

settled=0
skipped=0

while IFS= read -r mem; do
    id=$(echo "$mem" | grep -oP '"id"\s*:\s*"\K[^"]+' | head -1)
    content=$(echo "$mem" | grep -oP '"content"\s*:\s*"\K[^"]+' | head -1)

    [[ -z "$id" || -z "$content" ]] && continue

    # Must be open
    echo "$content" | grep -q "status:open" || continue

    # Extract horizon date
    horizon=$(echo "$content" | grep -oP 'horizon:\K[0-9]{4}-[0-9]{2}-[0-9]{2}' | head -1)
    [[ -z "$horizon" ]] && continue

    # Only settle if horizon has passed
    [[ "$horizon" < "$TODAY" || "$horizon" == "$TODAY" ]] || continue

    # Check if any correction references this id
    corrections=$("$CHITTA_BIN" callers --object "$id" --predicate "corrects" --json "${realm_args[@]}" 2>/dev/null || echo "[]")
    if echo "$corrections" | grep -q '"id"'; then
        echo "[settle] skipping $id — has correction reference"
        ((skipped++)) || true
        continue
    fi

    new_content="${content/status:open/status:confirmed}"
    if [[ "$DRY_RUN" == "true" ]]; then
        echo "[settle] DRY-RUN: would confirm $id (horizon:$horizon)"
    else
        "$CHITTA_BIN" memory_edit --id "$id" --content "$new_content" "${realm_args[@]}" 2>/dev/null && \
            echo "[settle] confirmed $id (horizon:$horizon)" || \
            echo "[settle] WARN: failed to edit $id" >&2
    fi
    ((settled++)) || true

done < <(echo "$raw" | python3 -c "
import sys, json
data = json.load(sys.stdin)
if not isinstance(data, list):
    data = data.get('memories', [])
for m in data:
    print(json.dumps(m))
" 2>/dev/null)

echo "[settle] done: settled=${settled} skipped=${skipped}"
