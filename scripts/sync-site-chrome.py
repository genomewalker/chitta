#!/usr/bin/env python3
"""Refresh the shared menu and footer in already structured static pages."""
from __future__ import annotations

import re

from site_common import DOCS, footer, navigation

for page in sorted(DOCS.rglob('*.html')):
    source = page.read_text(encoding='utf-8')
    url = '/' + page.relative_to(DOCS).as_posix()
    source, nav_count = re.subn(
        r'<nav class="nav site-nav".*?</nav>', navigation(url), source, flags=re.S)
    source, footer_count = re.subn(
        r'<footer class="footer site-footer".*?</footer>', footer(url), source, flags=re.S)
    if (nav_count, footer_count) != (1, 1):
        raise SystemExit(f'{page}: expected one site menu and footer')
    page.write_text(source, encoding='utf-8')
print('Updated site navigation and footers.')
