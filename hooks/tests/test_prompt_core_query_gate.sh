#!/usr/bin/env bash
# A user turn with no content token ("Do all", "Status") must not run the topic
# lanes: every one of them returns near-random rows for an empty query. Only
# the correction lanes may run. A one-token turn keeps the lanes.
set -u
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
T=$(mktemp -d "${TMPDIR:-/tmp}/qgate.XXXXXX")
trap 'rm -rf "$T"' EXIT
mkdir -p "$T/home/.claude/mind" "$T/bin"
cat > "$T/bin/chitta" <<'EOF'
#!/usr/bin/env bash
printf '%s\n' "$*" >> "${STUB_LOG}"
printf '{"results":[]}\n'
EOF
chmod +x "$T/bin/chitta"
export HOME="$T/home" XDG_RUNTIME_DIR="$T/run" CHITTA_DB_PATH="$T/home/.claude/mind" MIND_PATH="$T/home/.claude/mind"
export CHITTA_BIN="$T/bin/chitta" CHITTA_QUEUE="$T/queue.jsonl" CHITTA_REALM=project:test
unset CHITTA_HEADLESS CC_SOUL_HEADLESS
mkdir -p "$XDG_RUNTIME_DIR"
# daemon_available() only checks that the mind-path socket exists; bind one.
source "$here/../lib.sh"
SOCK="$(get_socket_path)"; mkdir -p "$(dirname "$SOCK")"
python3 -c "import socket,sys; socket.socket(socket.AF_UNIX).bind(sys.argv[1])" "$SOCK"
fail=0
check() { if [[ "$2" == *"$3"* ]]; then echo "ok: $1"; else echo "FAIL: $1 -> $2"; fail=1; fi; }

run_prompt() {
    export STUB_LOG="$T/$2.log"; : > "$STUB_LOG"
    printf '{"session_id":"s-%s","cwd":"%s","hook_event_name":"UserPromptSubmit","prompt":"%s"}' "$2" "$T" "$1" \
        | timeout 20 bash "$here/../prompt-core.sh" >/dev/null 2>&1 || true
}

run_prompt "Do all" zero
topic=$(sed -n 's/^prompt_context --state //p' "$T/zero.log" | sed 's/ --json$//' | jq '[.retrieval.lanes[] | select(. == "sem" or . == "kw" or . == "hyb")] | length')
check "zero-token turn launches no topic lane" "topic=$topic" "topic=0"

run_prompt "Why does the session registry use sqlite on the NFS home" many
topic=$(sed -n 's/^prompt_context --state //p' "$T/many.log" | sed 's/ --json$//' | jq '[.retrieval.lanes[] | select(. == "sem" or . == "kw" or . == "hyb")] | length')
check "multi-token turn launches topic lanes" "$([[ $topic -gt 0 ]] && echo launched || echo none)" "launched"

exit $fail
