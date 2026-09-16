#!/usr/bin/env python3
"""Static browser-experience gate for every HTML document under docs/.

Stdlib only; no network, browser, daemon or JavaScript execution. Run alongside
bash scripts/check-docs-links.sh (which also checks Markdown/repository links).
CSS checks assert source contracts, not rendered layout or visual correctness.
"""
from __future__ import annotations

import re
import sys
from collections import Counter
from html.parser import HTMLParser
from urllib.parse import unquote, urlsplit

from site_common import DOCS, MENU, STATUS_DATE, footer, navigation

VOID = set('area base br col embed hr img input link meta param source track wbr'.split())
COLUMNS = ('Chrome', 'Metadata', 'Headings', 'Layout', 'Links')


class Element:
    def __init__(self, tag, attrs, parent=None, line=0):
        self.tag = tag
        self.attrs = dict(attrs)
        self.parent = parent
        self.line = line
        self.text = ''

    def has_class(self, name):
        return name in self.attrs.get('class', '').split()

    def inside(self, predicate):
        parent = self.parent
        while parent:
            if predicate(parent):
                return True
            parent = parent.parent
        return False


class Page(HTMLParser):
    def __init__(self, source):
        super().__init__(convert_charrefs=True)
        self.elements = []
        self.stack = []
        self.unmatched = []
        self.feed(source)
        self.close()

    def handle_starttag(self, tag, attrs):
        node = Element(tag, attrs, self.stack[-1] if self.stack else None, self.getpos()[0])
        self.elements.append(node)
        if tag not in VOID:
            self.stack.append(node)

    def handle_startendtag(self, tag, attrs):
        self.handle_starttag(tag, attrs)
        if tag not in VOID:
            self.handle_endtag(tag)

    def handle_endtag(self, tag):
        for i in range(len(self.stack) - 1, -1, -1):
            if self.stack[i].tag == tag:
                # These are not optional-end-tag elements. Bad nesting can move
                # the navigation/main/footer in the browser's repaired DOM.
                for node in self.stack[i + 1:]:
                    if node.tag in {'main', 'nav', 'footer', 'div', 'section', 'a'}:
                        self.unmatched.append(f'unclosed {node.tag} at line {node.line}')
                del self.stack[i:]
                return
        self.unmatched.append(f'unmatched </{tag}> at line {self.getpos()[0]}')

    def handle_data(self, data):
        for node in self.stack:
            if node.tag not in {'script', 'style'}:
                node.text += data

    def tags(self, tag):
        return [e for e in self.elements if e.tag == tag]

    @property
    def ids(self):
        return {e.attrs['id'] for e in self.elements if 'id' in e.attrs}


def destination(source, raw):
    url = urlsplit(raw)
    # Treat the site's own production links as local, including fragments.
    if url.netloc and url.netloc != 'chitta.vaklab.dev':
        return None, ''
    if url.scheme and url.scheme not in {'http', 'https'}:
        return None, ''
    path = unquote(url.path)
    if path.startswith('/'):
        target = DOCS / path.lstrip('/')
    elif path:
        target = source.parent / path
    else:
        target = source
    if target.is_dir():
        target /= 'index.html'
    return target.resolve(), unquote(url.fragment)


def check(path, source, page, pages):
    errors = {column: [] for column in COLUMNS}

    def require(ok, column, message):
        if not ok:
            errors[column].append(message)

    url = '/' + path.relative_to(DOCS).as_posix()
    navs = [e for e in page.tags('nav') if e.has_class('site-nav')]
    require(len(navs) == 1 and navigation(url) in source,
            'Chrome', 'shared menu markup/order/current-page marker differs')
    require(len(page.tags('footer')) == 1 and footer(url) in source,
            'Chrome', 'shared footer/version/date differs')
    current = [e for e in page.elements if e.attrs.get('aria-current') == 'page']
    require(len(current) == 1 and current[0].attrs.get('href') == url,
            'Chrome', 'exactly one current-page link must identify this document')
    skips = [e for e in page.tags('a') if e.has_class('skip-link')]
    mains = page.tags('main')
    require(len(skips) == 1 and skips[0].attrs.get('href') == '#main-content',
            'Chrome', 'missing skip-to-content link')
    require(len(mains) == 1 and mains[0].attrs.get('id') == 'main-content'
            and mains[0].attrs.get('tabindex') == '-1',
            'Chrome', 'one focusable main landmark required')
    require(not any(e.inside(lambda p: p.tag == 'main') for e in navs + page.tags('footer')),
            'Chrome', 'site navigation/footer must be outside main')
    if skips and navs:
        require(page.elements.index(skips[0]) < page.elements.index(navs[0]),
                'Chrome', 'skip link must precede navigation')
    stamps = re.findall(r'Status as of ([^<\n·]+)', source)
    require(bool(stamps) and all(s.strip() == STATUS_DATE for s in stamps),
            'Chrome', 'inconsistent status line format/date')

    roots = page.tags('html')
    titles = page.tags('title')
    require(len(roots) == 1 and roots[0].attrs.get('lang') == 'en', 'Metadata', 'html lang=en required')
    require(len(titles) == 1 and re.fullmatch(r'chitta — \S.*', titles[0].text.strip()) is not None
            and not titles[0].text.strip().endswith(' — chitta'),
            'Metadata', 'title must be chitta — <page> (no repeated suffix)')
    for meta in page.tags('meta'):
        require(all(key in {'name', 'content', 'property', 'charset', 'http-equiv'}
                    for key in meta.attrs), 'Metadata', f'malformed meta attributes at line {meta.line}')
    for name in ('viewport', 'description'):
        metas = [e for e in page.tags('meta') if e.attrs.get('name') == name]
        require(len(metas) == 1 and bool(metas[0].attrs.get('content', '').strip()),
                'Metadata', f'one nonempty {name} meta required')
        if name == 'viewport' and metas:
            require('width=device-width' in metas[0].attrs.get('content', ''),
                    'Metadata', 'viewport must use device width')
    icons = [e for e in page.tags('link') if 'icon' in e.attrs.get('rel', '').split()]
    require(len(icons) == 1, 'Metadata', 'one favicon required')

    headings = [e for e in page.elements if re.fullmatch(r'h[1-6]', e.tag)]
    require(len(page.tags('h1')) == 1, 'Headings', 'exactly one h1 required')
    level = 0
    for heading in headings:
        next_level = int(heading.tag[1])
        require(next_level <= level + 1, 'Headings', f'heading gap at line {heading.line}')
        require(bool(heading.text.strip()), 'Headings', f'empty heading at line {heading.line}')
        require(heading.inside(lambda e: e.tag == 'main'), 'Headings', 'heading outside main')
        level = next_level
    ids = Counter(e.attrs['id'] for e in page.elements if 'id' in e.attrs)
    require(all(count == 1 for count in ids.values()), 'Headings',
            'duplicate IDs: ' + ', '.join(key for key, count in ids.items() if count > 1))
    for error in page.unmatched:
        require(False, 'Layout', error)
    require(not page.stack, 'Layout', 'unclosed document elements')
    for e in page.elements:
        require('style' not in e.attrs, 'Layout', f'inline style at line {e.line}; use a class')
        if e.tag == 'img':
            require('alt' in e.attrs, 'Layout', f'image without alt at line {e.line}')
        if e.tag == 'table':
            require(e.inside(lambda p: p.has_class('table-scroll')
                             and p.attrs.get('tabindex') == '0'
                             and bool(p.attrs.get('aria-label'))),
                    'Layout', f'table needs a named keyboard-scrollable wrapper at line {e.line}')
    stylesheets = [e.attrs.get('href', '') for e in page.tags('link')
                  if e.attrs.get('rel') == 'stylesheet']
    require(any(destination(path, href)[0] == DOCS / 'styles.css' for href in stylesheets),
            'Layout', 'shared stylesheet missing')
    require(bool(stylesheets) and stylesheets[-1] == '/site-shell.css',
            'Layout', 'site shell must load after local styles')

    for e in page.elements:
        is_toc = e.tag == 'a' and ('toc' in e.attrs.get('class', '')
                                  or e.inside(lambda n: 'toc' in n.attrs.get('class', '')))
        if is_toc and e.attrs.get('href', '').startswith('#'):
            anchor = unquote(e.attrs['href'][1:])
            target = next((n for n in page.elements if n.attrs.get('id') == anchor), None)
            require(target is not None and any(h is target or h.inside(lambda n, target=target: n is target)
                                                for h in headings),
                    'Headings', f'TOC target #{anchor} needs a heading')
        for attr in ('href', 'src', 'poster'):
            raw = e.attrs.get(attr)
            if raw is None:
                continue
            require(raw not in {'', '#'}, 'Links', f'empty link at line {e.line}')
            target, fragment = destination(path, raw)
            if target is None:
                continue
            require(target.is_relative_to(DOCS) and target.is_file(),
                    'Links', f'missing/unpublished target at line {e.line}: {raw}')
            if fragment and target.suffix == '.html' and target in pages:
                require(fragment in pages[target].ids,
                        'Links', f'missing anchor at line {e.line}: {raw}')
    # A missing menu destination must fail even if it has no inbound content link.
    for href, _ in MENU:
        target, _ = destination(path, href)
        require(target is None or target.is_file(), 'Links', f'menu target missing: {href}')
    return errors


def main():
    paths = sorted(DOCS.rglob('*.html'))
    sources = {p: p.read_text(encoding='utf-8') for p in paths}
    pages = {p: Page(source) for p, source in sources.items()}
    css = (DOCS / 'styles.css').read_text(encoding='utf-8')
    shell = (DOCS / 'site-shell.css').read_text(encoding='utf-8')
    contracts = {
        'responsive wrapping menu': r'@media\s*\(max-width:\s*700px\).*?\.site-nav \.nav-links\s*\{[^}]*flex-wrap:\s*wrap',
        'table scrolling': r'\.table-scroll\s*\{[^}]*overflow-x:\s*auto',
        'code scrolling': r'pre,\s*\.code-block,\s*\.quickstart-code\s*\{[^}]*overflow-x:\s*auto',
    }
    css_errors = [name for name, pattern in contracts.items() if not re.search(pattern, css, re.S)]
    for contract in ('prefers-color-scheme', 'prefers-reduced-motion', '.skip-link:focus', ':focus-visible'):
        if contract not in shell:
            css_errors.append(contract)
    print('| Page | ' + ' | '.join(COLUMNS) + ' | Result |')
    print('|---|' + '---|' * (len(COLUMNS) + 1))
    diagnostics = []
    failed = 0
    for path in paths:
        errors = check(path, sources[path], pages[path], pages)
        errors['Layout'].extend('missing CSS contract: ' + name for name in css_errors)
        name = path.relative_to(DOCS).as_posix()
        bad = any(errors.values())
        failed += bool(bad)
        print('| ' + name + ' | ' + ' | '.join('FAIL' if errors[c] else 'PASS' for c in COLUMNS)
              + ' | ' + ('FAIL' if bad else 'PASS') + ' |')
        for category, messages in errors.items():
            diagnostics.extend(f'{name} [{category}]: {m}' for m in messages)
    print(f'\n{len(paths)} pages checked; {failed} failed.')
    for diagnostic in diagnostics:
        print(diagnostic)
    return int(bool(failed))


if __name__ == '__main__':
    sys.exit(main())
