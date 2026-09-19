#!/usr/bin/env bash
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
fixture=$(mktemp -d "${TMPDIR:?}/p20-cleanup-XXXXXX")
export ROOT S="$fixture" W="$fixture" CLI="$fixture/cli" name=fixture holder=holder lead=''
cat > "$CLI" <<'STUB'
#!/usr/bin/env bash
payload=$(cat)
op=$(jq -r .params.arguments.op <<< "$payload")
printf '%s\n' "$op" >> "$S/calls"
[[ $op != stream_handoff ]] || exit 1
printf '{"result":{"isError":false,"structured":{"value":{"released":true}}}}\n'
STUB
chmod +x "$CLI"
for mode in exit success setup; do
    W="$fixture"
    case "$mode" in
        exit) worker=(bash -c 'exit 7');;
        success) worker=(bash -c 'printf "[handoff] stream=fixture gates=pass\n" > "$S/$name.handoff"');;
        setup) W="$fixture/missing"; worker=(true);;
    esac
    export W
    if bash "$ROOT/scripts/stream-lib.sh" --supervise "${worker[@]}" </dev/null; then
        echo "unexpected success for $mode"; exit 1
    fi
    [[ ! -e "$S/$name.pid" ]]
done
[[ $(grep -c '^stream_release$' "$S/calls") == 3 ]]
source "$ROOT/scripts/stream-lib.sh"
stream_rpc() { printf '{"released":false}\n'; }
if stream_release; then echo 'false release accepted'; exit 1; fi
printf 'PASS: release after worker failure, handoff failure, setup failure; false release rejected\n'
