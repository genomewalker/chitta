#!/usr/bin/env bash
# Check local links and fragments without network access or third-party packages.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."
python3 - "$@" <<'PY'
from collections import Counter
from html import unescape
from html.parser import HTMLParser
from pathlib import Path
import re
import sys
from urllib.parse import unquote, urlsplit

ROOT = Path.cwd()
FILES = [ROOT / 'README.md', *sorted((ROOT / 'docs').glob('*.md')),
         *sorted((ROOT / 'docs').rglob('*.html'))]
arguments = [arg for arg in sys.argv[1:] if arg != '--verbose-history']
if arguments:
    FILES = [Path(arg).resolve() for arg in arguments]


class Page(HTMLParser):
    def __init__(self):
        super().__init__()
        self.ids = set()
        self.links = []

    def handle_starttag(self, tag, attrs):
        attrs = dict(attrs)
        for attr in ('id', 'name' if tag == 'a' else 'id'):
            if attrs.get(attr):
                self.ids.add(attrs[attr])
        for attr in ('href', 'src', 'poster', 'data' if tag == 'object' else 'src'):
            if attrs.get(attr):
                self.links.append((self.getpos()[0], attrs[attr]))
        if attrs.get('srcset'):
            self.links.extend((self.getpos()[0], item.strip().split()[0])
                              for item in attrs['srcset'].split(',') if item.strip())


def unfenced(text):
    lines = []
    fence = None
    for line in text.splitlines():
        match = re.match(r'^\s{0,3}(`{3,}|~{3,})', line)
        if match:
            token = match[1]
            if fence is None:
                fence = token
            elif token[0] == fence[0] and len(token) >= len(fence):
                fence = None
            lines.append('')
        else:
            lines.append('' if fence or line.startswith('    ') else line)
    return '\n'.join(lines)


def slug(text):
    text = unescape(re.sub(r'<[^>]+>', '', text))
    text = re.sub(r'\[([^]]+)\]\([^)]*\)', r'\1', text)
    return re.sub(r'[^\w\- ]', '', text.lower()).replace(' ', '-')


def parse(path):
    text = path.read_text(encoding='utf-8')
    markdown = path.suffix.lower() == '.md'
    if markdown:
        text = unfenced(text)
    page = Page()
    page.feed(text)
    if markdown:
        counts = Counter()
        for heading in re.findall(r'^ {0,3}#{1,6}\s+(.+?)\s*#*$', text, re.M):
            key = slug(heading)
            page.ids.add(key + ('-' + str(counts[key]) if counts[key] else ''))
            counts[key] += 1
        for heading in re.findall(r'^([^\n]+)\n(?:={3,}|-{3,})\s*$', text, re.M):
            key = slug(heading.strip())
            page.ids.add(key + ('-' + str(counts[key]) if counts[key] else ''))
            counts[key] += 1
        # Inline code is not a link. Keep newlines for diagnostic line numbers.
        text = re.sub(r'(`+)([^`\n]+)\1', lambda m: ' ' * len(m[0]), text)
        target = r'(<[^>\n]+>|(?:\\.|[^\s()]|\([^()]*\))+?)'
        inline = re.compile(r'!?\[(?:[^\[\]]|\[[^\[\]]*\])*\]\(\s*' + target + r'(?:\s+["\'][^\n]*?["\'])?\s*\)')
        for match in inline.finditer(text):
            page.links.append((text.count('\n', 0, match.start()) + 1, match[1].strip('<>')))
        definitions = {m[1].casefold(): m[2].strip('<>') for m in re.finditer(
            r'^ {0,3}\[([^]]+)\]:\s*(<[^>]+>|\S+)', text, re.M)}
        for match in re.finditer(r'!?\[([^]\n]+)\](?:\[([^]\n]*)\])?(?!\s*[:(])', text):
            key = (match[2] or match[1]).casefold()
            if key in definitions:
                page.links.append((text.count('\n', 0, match.start()) + 1, definitions[key]))
    return page


cache = {}
errors = []
checked = 0
historical = set()
for source in FILES:
    cache.setdefault(source, parse(source))
    for line, raw in sorted(set(cache[source].links)):
        link = unescape(raw)
        # Immutable decision memos contain editor-style path:line citations,
        # including deleted code and private evidence. These are historical
        # citations, not portable site links; report every exemption.
        if source.name in {'DECISION-2026-09-15-learning-experiment.md',
                           'DECISION-2026-09-15-mdl-analogy.md'} and re.search(r':\d+$', link):
            historical.add((source.name, line, link))
            continue
        url = urlsplit(link)
        if url.scheme or url.netloc:
            continue
        path = unquote(url.path)
        fragment = unquote(url.fragment)
        if not path and not fragment:
            continue
        if path.startswith('/'):
            destination = ROOT / 'docs' / path.lstrip('/')
        elif path:
            destination = source.parent / path
        else:
            destination = source
        destination = destination.resolve()
        if destination.is_dir():
            destination /= 'index.html'
        checked += 1
        reason = None
        if not destination.is_file():
            reason = 'missing file'
        elif fragment:
            if destination.suffix.lower() in {'.html', '.md', '.svg'}:
                if destination not in cache:
                    cache[destination] = parse(destination)
                if fragment not in cache[destination].ids:
                    reason = 'missing anchor'
            else:
                reason = 'anchor on non-document target'
        if source.suffix == '.html' and not destination.is_relative_to(ROOT / 'docs'):
            reason = 'outside published docs/; use a repository URL'
        if reason:
            label = source.relative_to(ROOT) if source.is_relative_to(ROOT) else source
            errors.append(f'{label}:{line}: {reason}: {link}')
for error in errors:
    print(error)
print(f'Checked {len(FILES)} pages, {checked} local links: {len(errors)} errors.')
print(f'Preserved {len(historical)} historical path:line citations in immutable decision memos (not portable hyperlinks).')
if '--verbose-history' in sys.argv:
    for item in sorted(historical):
        print('%s:%s: %s' % item)
sys.exit(bool(errors))
PY
