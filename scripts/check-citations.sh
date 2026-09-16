#!/usr/bin/env bash
# Verify canonical citations, page references, claim anchors and fetch coverage.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."
python3 - "$@" <<'PY'
from collections import Counter, defaultdict
from html import escape
from pathlib import Path
import json
import re
import sys

WRITE = sys.argv[1:] == ['--write']
if sys.argv[1:] not in ([], ['--write']):
    sys.exit('usage: check-citations.sh [--write]')
INDEX = Path('docs/CITATIONS.md')
BEGIN, END = '<!-- BEGIN CITATIONS -->', '<!-- END CITATIONS -->'
BLOCK = re.compile(re.escape(BEGIN) + r'.*?' + re.escape(END), re.S)
URL = re.compile(r'https?://[^\s<>]+')
MD_CITE = re.compile(r'\[(\d+)\]\(#ref-(\d+)\)')
HTML_CITE = re.compile(r'<sup><a href="#ref-(\d+)">(\d+)</a></sup>')
rows = {}
errors = []
index_text = INDEX.read_text()
for line in index_text.splitlines():
    if not re.match(r'^\| \d+ \|', line):
        continue
    fields = [s.strip() for s in line.strip('|').split('|')]
    if len(fields) != 5:
        sys.exit('malformed index row: ' + line)
    key = int(fields[0])
    if key in rows:
        errors.append(f'duplicate index ID {key}')
    rows[key] = fields
if not rows:
    sys.exit('no index rows')
files = sorted(set([Path('README.md'), Path('CLAUDE.md'),
                   Path('codex-plugin/AGENTS.md'), Path('CHANGELOG.md'),
                   *Path('docs').glob('*.md'), *Path('docs').glob('*.html'),
                   *Path('benchmarks').rglob('README.md'),
                   *Path('benchmarks').rglob('protocol.md')]) - {INDEX})
usages = defaultdict(set)
listed = Counter()

def urls(key):
    return [] if rows[key][2] == '—' else rows[key][2].split()

def unfenced(text):
    # Code examples and quoted historical search output are not live citations.
    fence = None
    result = []
    for line in text.splitlines():
        m = re.match(r'^\s*(`{3,}|~{3,})', line)
        if m:
            if fence is None:
                fence = m[1][0]
            elif fence == m[1][0]:
                fence = None
            result.append('')
        else:
            result.append('' if fence else line)
    return '\n'.join(result)

def references(keys, html):
    entries = []
    for key in sorted(keys):
        canonical = rows[key][1]
        if html:
            links = ' '.join(f'<a href="{escape(u, quote=True)}">{escape(u)}</a>'
                             for u in urls(key))
            entries.append(f'    <li id="ref-{key}" value="{key}">{escape(canonical)} {links}</li>')
        else:
            links = ' '.join(f'[source](<{u}>)' for u in urls(key))
            entries.append(f'- <a id="ref-{key}"></a>**[{key}]** {canonical} {links}'.rstrip())
    if html:
        return (BEGIN + '\n<section id="references" class="references">\n'
                '  <div class="container">\n'
                '  <h2 class="heading-section">References</h2>\n  <ol>\n'
                + '\n'.join(entries) +
                '\n  </ol>\n  </div>\n</section>\n' + END)
    return BEGIN + '\n## References\n\n' + '\n'.join(entries) + '\n' + END

for path in files:
    text = path.read_text()
    html = path.suffix == '.html'
    blocks = list(BLOCK.finditer(text))
    if len(blocks) > 1:
        errors.append(f'{path}: multiple generated References blocks')
    body = BLOCK.sub(lambda m: '\n' * m[0].count('\n'), text)
    if re.search(r'^## References\s*$', body, re.M) or re.search(
            r'<section\b[^>]*\bid=["\']references["\']', body):
        errors.append(f'{path}: unmanaged References section')
    scan = body if html else unfenced(body)
    citations = list((HTML_CITE if html else MD_CITE).finditer(scan))
    keys = set()
    for m in citations:
        key = int(m[1])
        if m[1] != m[2]:
            errors.append(f'{path}: mismatched citation label {m[0]}')
        if key not in rows:
            errors.append(f'{path}: unknown reference {key}')
            continue
        keys.add(key)
        usages[key].add(f'{path}:{scan.count(chr(10), 0, m.start()) + 1}')
    # Reject malformed/dangling links, not just recognized citation syntax.
    all_links = re.findall(r'(?:href=["\']|\]\()#ref-(\d+)', scan)
    for key in map(int, all_links):
        if key not in keys:
            errors.append(f'{path}: noncanonical or dangling claim link ref-{key}')
    expected = references(keys, html) if keys else None
    if WRITE and expected:
        if blocks:
            text = BLOCK.sub(lambda m: expected, text)
        elif html:
            footer = re.search(r'<footer\b', text)
            if not footer:
                errors.append(f'{path}: no footer for References insertion')
                continue
            text = text[:footer.start()] + expected + '\n\n' + text[footer.start():]
        else:
            text = text.rstrip() + '\n\n' + expected + '\n'
        path.write_text(text)
    elif expected and (not blocks or blocks[0][0] != expected):
        errors.append(f'{path}: References missing or differs from canonical index (run --write)')
    elif not expected and blocks:
        errors.append(f'{path}: References has no claim citations')
    if expected:
        listed.update(keys)
    # Enforce placement before footer / at end of Markdown.
    final_blocks = list(BLOCK.finditer(text))
    if final_blocks:
        b = final_blocks[0]
        if html:
            footer = re.search(r'<footer\b', text)
            if footer and b.start() > footer.start():
                errors.append(f'{path}: References is after footer')
        elif text[b.end():].strip():
            errors.append(f'{path}: References is not at end')

# JSON evidence has no native References section; its sources also appear in
# EVOLVE-BRIDGE's on-page evidence bibliography.
by_url = {u: k for k in rows for u in urls(k)}
for path in sorted(Path('chitta-mcp/evolve/proposals.d').glob('*.json')):
    text = path.read_text()
    data = json.loads(text)
    # Cards written by the nightly literature watch (kind == "sota-card") cite
    # their paper in evidence[].source and arrive without a hand-written index
    # entry; they are self-citing. Hand-written hypothesis cards stay indexed.
    self_citing = data.get('kind') == 'sota-card'
    for evidence in data.get('evidence', []):
        source = evidence.get('source', '')
        if source.startswith(('http://', 'https://')):
            if source not in by_url:
                if not self_citing:
                    errors.append(f'{path}: evidence source absent from index: {source}')
            else:
                for n, line in enumerate(text.splitlines(), 1):
                    if '"source":' in line and source in line:
                        usages[by_url[source]].add(f'{path}:{n}')
for key in rows:
    if not listed[key]:
        errors.append(f'index {key}: absent from every page References section')
    usage = '; '.join(sorted(usages[key])) or 'pending'
    if WRITE:
        rows[key][3] = usage
    elif rows[key][3] != usage:
        errors.append(f'index {key}: stale file:line usages (run --write)')

if WRITE:
    lines = []
    for line in index_text.splitlines():
        m = re.match(r'^\| (\d+) \|', line)
        lines.append('| ' + ' | '.join(rows[int(m[1])]) + ' |' if m else line)
    INDEX.write_text('\n'.join(lines) + '\n')
    index_text = INDEX.read_text()

log = Path('docs/citations-check-2026-09-16.txt')
logged = {}
if log.exists():
    for n, line in enumerate(log.read_text().splitlines(), 1):
        m = re.fullmatch(r'(\d{3}) (https?://\S+)', line)
        if not m:
            errors.append(f'{log}:{n}: expected HTTP status and URL only')
        elif m[2] in logged:
            errors.append(f'{log}:{n}: duplicate URL')
        else:
            logged[m[2]] = m[1]
for url in set(u.rstrip('.,;') for u in URL.findall(index_text)):
    if url not in logged:
        errors.append(f'index URL has no recorded fetch: {url}')
if errors:
    for error in errors:
        print('ERROR:', error)
    sys.exit(1)
counts = Counter(v[4].split(' — ')[0].split(';')[0] for v in rows.values())
added = sum('; added' in v[4] for v in rows.values())
print(f'{len(rows)} references; {sum(listed.values())} page entries; {len(logged)} fetched URLs; 0 errors')
print('; '.join(f'{k}: {v}' for k, v in sorted(counts.items())) + f'; added: {added}')
PY
