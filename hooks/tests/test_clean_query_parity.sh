#!/bin/bash
# Exact original Python regex vs production sed cleaner: 20 fixture prompts.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
PYTHONDONTWRITEBYTECODE=1 python3 - "$ROOT" <<'PY'
import re
import subprocess
import sys

fixtures = [
    '  Explain the hook  \n',
    '<task-notification>finished\noutput</task-notification>',
    'before<system-reminder>secret</system-reminder>after',
    '<command-name>/recap</command-name><command-message>recap</command-message>continue',
    '<local-command-stdout>output</local-command-stdout> user',
    '<local-command-stderr code="1">error\ntrace</local-command-stderr>',
    '<SYSTEM-REMINDER>UPPER</system-reminder> Mixed',
    '<system-reminder>outer<system-reminder>inner</system-reminder>tail</system-reminder>',
    '<system-reminder>A</system-reminder>keep<system-reminder>B</system-reminder>',
    '<system-reminder>unclosed <task-notification>done</task-notification> keep',
    'literal <unknown>keep</unknown> and <system-reminder/>keep',
    '<local-command-a_b9>drop</local-command-a_b9>keep',
    '<local-command-é漢>drop</local-command-é漢> café Ελληνικά 漢字 🐍',
    '\u0085\u00a0\u2007\u202f café\n漢字 \u3000\x1c',
    'before\n<system-reminder>one\ntwo\n</system-reminder>\nafter',
    '<system-reminder data=">">body</system-reminder>keep',
    '<system-reminder>x</task-notification>keep',
    '<system-reminder>1</system-reminder>\n\n<task-notification>2</task-notification>\nend',
    '<sys<command-name>x</command-name>tem-reminder>keep</system-reminder>',
    'quotes " \' \\ $HOME `echo nope`\t<local-command-a-b>keep</local-command-a-b>',
]
pattern = r'<(task-notification|system-reminder|command-name|command-message|local-command-\w+)[^>]*>.*?</\1>'
assert len(fixtures) == 20
for index, prompt in enumerate(fixtures, 1):
    expected = re.sub(pattern, '', prompt + '\n', flags=re.DOTALL | re.IGNORECASE).strip()
    result = subprocess.run(['bash', '-c', 'source "$1/hooks/lib.sh"; clean_query', '_', sys.argv[1]],
                            input=prompt + '\n', text=True, capture_output=True, check=True)
    assert result.stdout == expected, (index, repr(expected), repr(result.stdout), result.stderr)
print('ok: all 20 clean_query fixtures match the original Python cleaner byte for byte')
PY
