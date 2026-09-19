"""Shared static site chrome, also used by the structural gate.

Root-relative URLs keep the GitHub Pages custom-domain 404 usable at any depth.
After editing the menu, run `python3 scripts/sync-site-chrome.py`.
"""
from __future__ import annotations

import html
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DOCS = ROOT / 'docs'
STATUS_DATE = '2026-09-19'
REPO = 'https://github.com/genomewalker/chitta'
MENU = (
    ('/index.html', 'Home'),
    ('/getting-started.html', 'Get Started'),
    ('/philosophy.html', 'Philosophy'),
    ('/architecture.html', 'Architecture'),
    ('/recall.html', 'Recall'),
    ('/benchmarks.html', 'Benchmarks'),
    ('/chitta-field.html', 'chitta-field'),
    ('/sadhana.html', 'Sadhana'),
    ('/context.html', 'Context'),
    ('/tools.html', 'Tools'),
    ('/cli.html', 'CLI'),
    ('/hooks.html', 'Hooks'),
    ('/skills.html', 'Skills'),
    ('/changelog.html', 'Changelog'),
    ('/constellation.html', 'Constellation'),
    ('/dreams/index.html', 'Dreams'),
    ('/brain-viz/index.html', 'Brain Architecture'),
    ('/mind-viz/index.html', 'Mind Interface'),
    ('/vedanta/index.html', 'Vedanta'),
    ('/evolve-bridge-live/index.html', 'Evolve Evidence'),
    (REPO, 'GitHub'),
)


def version():
    changelog = (ROOT / 'CHANGELOG.md').read_text(encoding='utf-8')
    return re.search(r'^## \[(\d+\.\d+\.\d+)\]', changelog, re.M)[1]


def navigation(url):
    items = []
    for href, label in MENU:
        current = ' aria-current="page"' if href == url else ''
        items.append(f'      <li><a href="{href}"{current}>{label}</a></li>')
    return ('<nav class="nav site-nav" aria-label="Main navigation">\n'
            '  <div class="nav-inner">\n'
            '    <a href="/index.html" class="nav-brand">chitta</a>\n'
            '    <ul class="nav-links">\n' + '\n'.join(items) + '\n'
            '    </ul>\n  </div>\n</nav>')


def footer(url):
    # Articles and the error page have a self-link; do not mark a parent as
    # aria-current="page", which would misidentify the current document.
    current = ' aria-current="page"' if url not in dict(MENU) else ''
    return ('<footer class="footer site-footer">\n'
            '  <div class="container">\n'
            '    <p class="footer-text">\n'
            f'      <a href="{REPO}">GitHub repository</a> · '
            '<a href="https://opensource.org/licenses/MIT">MIT license</a> · '
            f'<a href="{html.escape(url, quote=True)}"{current}>This page</a>\n'
            '    </p>\n'
            f'    <p class="site-status">Status as of {STATUS_DATE} · '
            f'<a href="/changelog.html">Unreleased + v{version()}</a></p>\n'
            '  </div>\n</footer>')
