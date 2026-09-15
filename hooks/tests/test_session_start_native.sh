#!/bin/bash
# Native lifecycle arguments and byte-for-byte parity with the Python cards.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
PYTHONDONTWRITEBYTECODE=1 python3 - "$ROOT" <<'PY'
import json
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

root = Path(sys.argv[1])
sys.path.insert(0, str(root / 'chitta-mcp'))
import task_ledger

with tempfile.TemporaryDirectory(prefix='native-session-test-') as tmp:
    base = Path(tmp)
    source = (root / 'hooks/session-start-hook.sh').read_text()
    functions = base / 'functions.sh'
    functions.write_text('source "$ROOT/hooks/lib.sh"\n' + '\n'.join(
        re.search(r'^' + name + r'\(\) \{\n.*?^\}', source, re.M | re.S).group()
        for name in ('_register_session', '_render_tasks', '_collect_corrections')))
    cli = base / 'chitta'
    cli.write_text('''#!/bin/bash
jq -nc --args '$ARGS.positional' -- "$@" >> "$CALLS"
case "$1 $*" in
*'--op session_get'*) echo '{"value":{"thread_id":"prior-thread"}}' ;;
*'--op inbox_list'*) cat "$INBOX" ;;
*'--op thread_list'*) cat "$THREADS" ;;
transcript_register*) exit "${TRANSCRIPT_FAIL:-0}" ;;
queue_write*) exit 1 ;;
esac
''')
    cli.chmod(0o700)
    env = dict(os.environ, ROOT=str(root), CHITTA_BIN=str(cli), MAX_WAIT='2',
               CALLS=str(base / 'calls'), INBOX=str(base / 'inbox'), THREADS=str(base / 'threads'),
               REALM='project:parity', _ld=tmp, MIND_PATH=tmp, SESSION_ID='native-session',
               TRANSCRIPT_PATH='/tmp/transcript with spaces-α.jsonl', PROJECT_DIR=str(root),
               _SESSION_START_PARENT_PID='1234', CHITTA_QUEUE=str(base / 'queue'),
               INPUT=json.dumps(dict(session_id='native-session', source='resume', model='model-α')))

    def run(function, **overrides):
        return subprocess.run(['bash', '-c', 'source "$1"; ' + function, '_', str(functions)],
                              env=dict(env, **overrides), text=True, capture_output=True, check=True).stdout

    for items, threads in [([], []),
        ([dict(event_type='completed', digest='α' * 120), dict(event_type='failure', digest='failed'),
          dict(event_type='other', digest='embedded\nnewline\n\n')],
         [dict(title='Thread α\nsecond line', thread_id='12345678-long')]),
        ([], [dict(title='Only thread', thread_id='t')]),
        ([dict(event_type='failed', digest='inbox only')], [])]:
        (base / 'inbox').write_text(json.dumps(dict(value=dict(rows=items))))
        (base / 'threads').write_text(json.dumps(dict(value=dict(rows=threads))))
        task_ledger.inbox_list = lambda **kwargs: items
        task_ledger.thread_list = lambda **kwargs: threads
        expected = ''.join('\n' + card.rstrip('\n') + '\n' for card in
                           (task_ledger.render_inbox(env['REALM'], 5),
                            task_ledger.render_threads(env['REALM'], 3)) if card)
        assert run('_render_tasks') == expected
    # A failed inbox must not suppress the other card.
    (base / 'inbox').write_text('')
    threads = [dict(title='Surviving thread', thread_id='thread')]
    (base / 'threads').write_text(json.dumps(dict(value=dict(rows=threads))))
    assert run('_render_tasks') == '\n' + task_ledger.render_threads(env['REALM'], 3) + '\n'

    correction = dict(id=18446744073709551614, text='unicode α "quoted 123" \\ slash\n' + '漢' * 130)
    (base / 'corrections_raw').write_text(json.dumps(dict(results=[correction,
        dict(id=2, text='skip verified'), dict(id=3, text='skip applied', correction_state='applied')])))
    run('_collect_corrections')
    records = (base / 'correction_records').read_bytes().split(b'\0')
    assert records == [str(correction['id']).encode(), correction['text'][:120].encode(), b''], records

    (base / 'calls').write_text('')
    run('_register_session' , TRANSCRIPT_FAIL='1')
    calls = [json.loads(line) for line in (base / 'calls').read_text().splitlines()]
    register = next(call for call in calls if call[0] == 'session_register')
    args = dict(zip(register[1::2], register[2::2]))
    assert args['--pid'] == '1234'
    assert args['--transcript_path'] == env['TRANSCRIPT_PATH']
    assert args['--project_dir'] == str(root) and args['--realm'] == env['REALM']
    metadata = json.loads(args['--metadata'])
    assert metadata['thread_id'] == 'prior-thread' and metadata['client'] == 'claude'
    assert metadata['model'] == 'model-α' and metadata['hook_source'] == 'resume'
    lease = next(call for call in calls if '--op' in call and 'lease_claim' in call)
    assert json.loads(lease[lease.index('--args') + 1]) == dict(session_id='native-session', thread_id='prior-thread')
    queued = json.loads((base / 'queue').read_text())
    assert queued['tool'] == 'transcript_register'
    assert queued['args']['transcript_path'] == env['TRANSCRIPT_PATH']
print('ok: native registration, resumed thread lease, transcript queue, exact Python card parity')
PY
