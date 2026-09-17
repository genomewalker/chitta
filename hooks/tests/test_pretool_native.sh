#!/bin/bash
set -euo pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
export HOME="$T/home" CHITTA_DB_PATH="$T/mind" CHITTA_QUEUE="$T/queue" CHITTA_BIN="$T/cli" CHITTA_PLUGIN_DIR="$ROOT" HOOK_POLICY_FIXTURE="$T/responses"
mkdir -p "$CHITTA_DB_PATH"
"${CXX:-g++}" -std=c++17 -O2 -pthread -I"$ROOT/chitta/include" "$ROOT/hooks/tests/event-response.cpp" -lcrypto -o "$CHITTA_BIN"
printf '%s\n' '{"find_symbol":{"structured":{"symbols":[]}},"code_context":{"structured":{"dir_symbols":10}}}' > "$HOOK_POLICY_FIXTURE"
python3 - "$ROOT" "$T" <<'PY'
import json,os,subprocess,sys
from pathlib import Path
root=Path(sys.argv[1]);base=Path(sys.argv[2])
def call(tool,fields,**options):
 env=dict(os.environ,**options)
 p=subprocess.run(['bash',str(root/'hooks/pre-tool-hook.sh'),tool],input=json.dumps({'session_id':'native-pretool','tool_input':fields}),text=True,capture_output=True,env=env,timeout=3)
 return p,json.loads(p.stdout) if p.stdout.startswith('{') else {}
p,data=call('Bash',{'command':'find / -name needle'},CHITTA_STRICT_MODE='1')
assert p.returncode==0 and data['hookSpecificOutput']['permissionDecision']=='deny'
p,data=call('Bash',{'command':'find . -name needle'})
assert data['hookSpecificOutput']['updatedInput']['command']=='(set -o pipefail; ( find . -maxdepth 3 -name needle\n) 2>&1 | chitta output_cap)'
p,data=call('Agent',{'subagent_type':'research','description':'find a symbol','model':'fable'})
assert 'updatedInput' not in data.get('hookSpecificOutput',{})
p,data=call('Agent',{'subagent_type':'fork','description':'find a symbol'})
assert 'updatedInput' not in data.get('hookSpecificOutput',{})
p,data=call('Agent',{'subagent_type':'research','description':'find a symbol','prompt':'lookup'})
assert data['hookSpecificOutput']['updatedInput']['model']=='haiku'
source=base/'large.py';source.write_text('pass\n'*250)
p,data=call('Read',{'file_path':str(source)},CHITTA_HOOK_ENFORCE='1')
assert data['hookSpecificOutput']['updatedInput']['limit']==150
p,data=call('Read',{'file_path':str(source)},CHITTA_HOOK_ENFORCE='1')
assert data['hookSpecificOutput']['updatedInput']['limit']==40
p,data=call('Read',{'file_path':str(source)},CHITTA_HOOK_ENFORCE='1',CHITTA_ALLOW_READ='1')
assert data['hookSpecificOutput']['updatedInput']['limit']==150
assert 'read-dedup' not in p.stdout
p,data=call('ScheduleWakeup',{'prompt':'<<autonomous-loop-dynamic>>'},CHITTA_LOOP_LIMIT='0')
assert p.returncode==2 and data['hookSpecificOutput']['permissionDecision']=='block'
p,data=call('Bash',{'command':'rm -rf /'},CHITTA_BIN='/bin/false',CHITTA_HEADLESS='1')
assert p.returncode==2 and data['hookSpecificOutput']['permissionDecision']=='block'
print('ok: native search routing, model exemptions, read cache, loop budget; shell safety without daemon')
PY
