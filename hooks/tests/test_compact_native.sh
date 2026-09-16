#!/bin/bash
# Compaction policy retains operational truth while clients only move envelopes.
set -euo pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
export CHITTA_DB_PATH="$T/mind" CHITTA_QUEUE="$T/queue" CHITTA_BIN="$T/cli" HOOK_POLICY_FIXTURE="$T/responses"
export CHITTA_PLUGIN_DIR="$ROOT" CHITTA_HOOK_NOW=1789516800000
"${CXX:-g++}" -std=c++17 -O2 -pthread -I"$ROOT/chitta/include" "$ROOT/hooks/tests/event-response.cpp" -lcrypto -o "$CHITTA_BIN"
cat > "$T/transcript.jsonl" <<'JSON'
{"type":"user","message":{"content":"Analyze /data/reads.bam and retain the original data."}}
{"type":"assistant","message":{"content":[{"type":"text","text":"Completed validation.\nNext: plot the result.\nBlocked by missing labels.\n[DECISION] Preserve original data."},{"type":"tool_use","name":"Bash","input":{"command":"python analyze.py /data/reads.bam"}},{"type":"tool_use","name":"TaskUpdate","input":{"subject":"Plot results","status":"pending"}}]}}
JSON
printf '%s\n' '{"compact_context":{"structured":{"stats":{"before_tokens":100,"after_tokens":40,"dropped_pct":60,"embedding":true}}},"ledger_load":{"structured":{"snapshot":"Goal: Analyze the existing data","active_files":["/data/reads.bam"],"discoveries":["Completed validation"],"next_steps":["Plot results"]}}}' > "$HOOK_POLICY_FIXTURE"
jq -nc --arg path "$T/transcript.jsonl" '{session_id:"compact-native",transcript_path:$path}' > "$T/input"
bash "$ROOT/hooks/pre-compact-hook.sh" < "$T/input" > "$T/stdout" 2> "$T/stderr"
[[ ! -s "$T/stdout" ]]
python3 - "$T/queue" "$T/transcript.jsonl" <<'PY'
import json,sys
rows=[json.loads(line) for line in open(sys.argv[1])]
ledger=next(r['args'] for r in rows if r['tool']=='ledger_save')
assert '/data/reads.bam' in ledger['active_files']
assert 'Next: plot the result.' in ledger['next_steps']
assert 'Blocked by missing labels.' in ledger['blockers']
assert '[DECISION] Preserve original data.' in ledger['decisions']
assert any('ran: python analyze.py' in x for x in ledger['discoveries'])
assert ledger['todos']==[{'content':'Plot results','status':'pending'}]
registration=next(r['args'] for r in rows if r['tool']=='transcript_register')
assert registration['transcript_path']==sys.argv[2], 'do not append a newline to the real path'
assert any(r['tool']=='distill_trigger' for r in rows)
assert any('100→40' in r['args'].get('content','') for r in rows)
PY
bash "$ROOT/hooks/compact-restore-hook.sh" < "$T/input" > "$T/restore"
jq -e '.hookSpecificOutput.additionalContext | contains("[resume-directive]") and contains("[anti-confab]") and contains("Completed validation") and contains("Plot results")' "$T/restore" >/dev/null
export CHITTA_BIN=/bin/false
[[ $(bash "$ROOT/hooks/pre-compact-hook.sh" < "$T/input") == '[chitta] daemon unavailable; context not loaded.' ]]
[[ $(bash "$ROOT/hooks/compact-restore-hook.sh" < "$T/input") == '[chitta] daemon unavailable; context not loaded.' ]]
echo 'ok: native compact checkpoint, restoration and timeout envelopes'
