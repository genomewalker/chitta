#!/bin/bash
set -euo pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
export HOME="$T/home" CHITTA_DB_PATH="$T/mind" CHITTA_RUNTIME_LOCAL=0 CHITTA_BIN="$T/chitta" CHITTA_PLUGIN_DIR="$ROOT"
export PATH="$T:$PATH"
mkdir -p "$CHITTA_DB_PATH"
"${CXX:-g++}" -std=c++17 -O2 -pthread -I"$ROOT/chitta/include" "$ROOT/hooks/tests/event-response.cpp" -lcrypto -o "$CHITTA_BIN"
python3 - "$ROOT" <<'PY'
import hashlib,json,os,subprocess,sys,time
from pathlib import Path
root=Path(sys.argv[1]);cli=os.environ['CHITTA_BIN']
def run(*args,data=b'',**env):
 return subprocess.run([cli,*args],input=data,capture_output=True,env=dict(os.environ,**env))
def rewrite(command,background=False,**env):
 p=subprocess.run(['bash',str(root/'hooks/pre-tool-hook.sh'),'Bash'],input=json.dumps({'session_id':'cap-test','tool_input':{'command':command,'timeout':123,'run_in_background':background}}),text=True,capture_output=True,env=dict(os.environ,**env),check=True)
 out=json.loads(p.stdout or '{}').get('hookSpecificOutput',{})
 updated=out.get('updatedInput',{})
 if updated: assert updated['timeout']==123
 return updated.get('command',command)
for command in ['echo one\necho two','sleep 1 &','srun hostname','nohup sleep 1','setsid sleep 1','codex exec hi','cat <<EOF','echo hi | sqz compress','echo hi | chitta output_cap','read answer','bash -i']:
 assert rewrite(command)==command,command
assert rewrite('sleep 30',background=True)=='sleep 30'
assert rewrite('echo hi',CHITTA_OUTPUT_CAP='0')=='echo hi'
for command,status in [('printf hello',0),('printf error >&2; exit 7',7),('false | true',1)]:
 rewritten=rewrite(command);assert ' | chitta output_cap' in rewritten,(command,rewritten)
 p=subprocess.run(['bash','-c',rewritten],capture_output=True)
 assert p.returncode==status,(command,p.returncode,p.stderr)
 assert p.stdout==({'printf hello':b'hello','printf error >&2; exit 7':b'error'}.get(command,b''))
for raw in [b'',b'abc\x00\xff\n',b'x'*6000,('é'*6000).encode()]:
 assert run('output_cap',data=raw).stdout==raw
for raw in [(('row é'*30+'\n')*100).encode(),b'z'*30000]:
 p=run('output_cap',data=raw);assert p.returncode==0,p.stderr
 ref=hashlib.sha256(raw).hexdigest()[:12]
 assert p.stdout.startswith(('§ref:'+ref+'§').encode())
 assert len(p.stdout.decode())<=6000
 assert run('output_ref','--hash',ref).stdout==raw
 assert run('output_ref','--hash',ref,'--lines','2-4').stdout==b''.join(raw.splitlines(keepends=True)[1:4])
 assert run('output_ref','--hash',ref,'--lines','0-2').returncode==2
 path=Path(os.environ['CHITTA_DB_PATH'])/'outputs'/ref
 os.utime(path,(time.time()-49*3600,)*2)
 assert run('output_ref','--hash',ref).returncode==1
assert run('output_ref','--hash','../../secret').returncode==2
# A storage failure must preserve every byte.
blocked=Path(os.environ['CHITTA_DB_PATH'])/'blocked';blocked.write_text('file')
raw=b'x'*7000
assert run('output_cap',data=raw,CHITTA_DB_PATH=str(blocked)).stdout==raw
print('PASS: rewrite exclusions, opt-out, input fields, exit status, cap boundaries, binary/UTF-8 round-trip, ranges, TTL, storage failure')
PY
