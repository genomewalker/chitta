#!/usr/bin/env bash
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
export S="$tmp/specs" W="$tmp/work" CLI="$tmp/chitta" ROOT
mkdir -p "$S" "$W" "$tmp/bin"
export PATH="$tmp/bin:$PATH"
cat > "$CLI" <<'STUB'
#!/usr/bin/env bash
if (($#)); then
    echo "$1" >> "$S/calls"
else
    payload=$(cat)
    op=$(jq -r .params.arguments.op <<< "$payload")
    echo "$op" >> "$S/calls"
    echo '{"result":{"isError":false,"structured":{"value":{"claimed":true,"released":true}}}}'
fi
STUB
cat > "$tmp/bin/claude" <<'STUB'
#!/usr/bin/env bash
printf '%s\n' "$$" > "$S/actual"
cat >/dev/null
[[ ${TEST_MODE:-} != missing ]] || exit 0
if [[ ${TEST_MODE:-} == signal ]]; then sleep 60; exit; fi
printf '[handoff] stream=%s gates=pass\n' "$name" > "$S/$name.handoff"
STUB
cat > "$tmp/patched-codex" <<'STUB'
#!/usr/bin/env bash
if [[ ${1:-} == --version ]]; then echo patched-fixture; exit; fi
[[ -z $(cat) ]] || exit 42
[[ ${!#} == prompt ]] || exit 43
printf '%s\n' "$$" > "$S/actual"
printf '[handoff] stream=%s gates=pass\n' "$name" > "$S/$name.handoff"
STUB
printf '#!/bin/bash\nexit 99\n' > "$tmp/bin/codex"
chmod +x "$CLI" "$tmp/bin/claude" "$tmp/bin/codex" "$tmp/patched-codex"
export CHITTA_CODEX_BIN="$tmp/patched-codex"
export name=fixture holder=holder lead=lead claim_args='{}'
for client in claude codex; do
    printf 'prompt' | bash "$ROOT/scripts/stream-lib.sh" --supervise "$client"
    [[ ! -e "$S/$name.pid" && -s "$S/$name.handoff" ]]
done
export TEST_MODE=missing
rm "$S/$name.handoff"
if printf 'prompt' | bash "$ROOT/scripts/stream-lib.sh" --supervise claude; then exit 1; fi
grep -q 'gates=fail' "$S/$name.handoff"
export TEST_MODE=signal
printf 'prompt' > "$tmp/prompt"
bash "$ROOT/scripts/stream-lib.sh" --supervise claude < "$tmp/prompt" &
supervisor=$!
for ((i=0;i<100;i++)); do
    [[ -s "$S/$name.pid" ]] && [[ $(<"$S/$name.pid") == "$(<"$S/actual")" ]] && break
    sleep .02
done
worker=$(<"$S/$name.pid")
[[ "$worker" == "$(<"$S/actual")" && "$worker" != "$supervisor" ]]
# A competing launch is rejected before claiming or constructing a prompt.
source "$ROOT/scripts/stream-lib.sh"
if stream_launch claude; then exit 1; fi
kill -TERM "$supervisor"
wait "$supervisor" && exit 1
! kill -0 "$worker" 2>/dev/null
[[ ! -e "$S/$name.pid" ]]
[[ $(grep -c '^stream_release$' "$S/calls") == 4 ]]
[[ $(grep -c '^msg_send$' "$S/calls") == 4 ]]
echo 'PASS: both clients, worker PID, duplicate rejection, missing handoff, signal cleanup, one message/release per exit'
