#!/bin/bash
# Local envelopes retain exact queue bytes and project-path identity.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
PYTHONDONTWRITEBYTECODE=1 python3 - "$ROOT" <<'PY'
import json
import os
import sys
import tempfile
import uuid
from pathlib import Path
sys.path.insert(0, str(Path(sys.argv[1]) / 'chitta-mcp'))
from hook_client import Client
from hook_session import project_dir
with tempfile.TemporaryDirectory(prefix='envelope') as temporary:
    base = Path(temporary)
    os.environ.update(CHITTA_DB_PATH=str(base/'mind'), CHITTA_QUEUE=str(base/'queue'), CHITTA_RUNTIME_LOCAL='0')
    client=Client({'session_id':'envelope'})
    values=('', 'plain', '"quote" \\ slash\n\tcarriage\rα', 'tail\n\n')
    for value in values:
        client.queue('observe', {'category':'wisdom','content':value,'id':18446744073709551614})
    raw=(base/'queue').read_bytes().splitlines()
    assert len(raw)==len(values), 'one complete JSONL record per acknowledged write'
    records=[json.loads(line) for line in raw]
    assert [row['args']['content'] for row in records]==list(values)
    assert all(row['args']['id']==18446744073709551614 for row in records)
    assert len({uuid.UUID(row['ack_id']) for row in records})==len(values)
    (base/'project-name/sub dir').mkdir(parents=True)
    for path in (base/'project-name',base/'project-name/sub dir',base/'missing/child'):
        assert project_dir({'transcript_path':'/home/projects/'+str(path).replace('/','-')+'/session.jsonl'})==str(path)
    assert project_dir({})==''
print('ok: exact Unicode/u64 JSONL records, unique queue acknowledgements and project paths')
PY
