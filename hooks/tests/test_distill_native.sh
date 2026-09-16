#!/bin/bash
set -euo pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
export CHITTA_BIN="$T/cli" DISTILL_ARGS="$T/args"
cat > "$CHITTA_BIN" <<'CLI'
#!/bin/bash
printf '%s\n' "$@" > "$DISTILL_ARGS"
printf 'Native distillation complete\n'
CLI
chmod +x "$CHITTA_BIN"
printf '%s\n' 'SESSION_ID=registered-session' 'REALM=project:test' 'MODEL=retired-shell-model' '---' '[assistant]' '[SOLUTION] Untrusted $(touch /never-execute)' > "$T/staging"
[[ $(bash "$ROOT/hooks/distill.sh" "$T/staging") == 'Native distillation complete' ]]
printf '%s\n' distill_now --session_id registered-session --realm project:test > "$T/expected"
cmp "$T/args" "$T/expected"
export CHITTA_BIN=/bin/false
[[ $(bash "$ROOT/hooks/distill.sh" "$T/staging") == '[chitta] daemon unavailable; context not loaded.' ]]
echo 'ok: registered-session native distillation only; timeout emits minimal output'
