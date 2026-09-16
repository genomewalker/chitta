"""Mutation checks for the structural gate, without changing site files."""
from __future__ import annotations

import importlib.util
import sys
import unittest
from pathlib import Path

SCRIPTS = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(SCRIPTS))
spec = importlib.util.spec_from_file_location('check_site', SCRIPTS / 'check-site.py')
site = importlib.util.module_from_spec(spec)
spec.loader.exec_module(site)


class SiteGateTests(unittest.TestCase):
    def setUp(self):
        self.path = site.DOCS / 'index.html'
        self.source = '''<!DOCTYPE html><html lang="en"><head>
<title>chitta — Home</title><meta name="viewport" content="width=device-width">
<meta name="description" content="Memory for coding agents">
<link rel="icon" href="/favicon.svg"><link rel="stylesheet" href="/styles.css">
<link rel="stylesheet" href="/site-shell.css"></head><body>
<a class="skip-link" href="#main-content">Skip to content</a>
''' + site.navigation('/index.html') + '''
<main id="main-content" tabindex="-1"><h1>Home</h1>
<section id="intro"><h2>Introduction</h2><p>Memory.</p></section>
</main>''' + site.footer('/index.html') + '</body></html>'

    def errors(self, source):
        page = site.Page(source)
        return site.check(self.path, source, page, {self.path: page})

    def test_valid_page(self):
        self.assertFalse(any(self.errors(self.source).values()))

    def test_mutations_fail_in_expected_category(self):
        cases = [
            ('menu drift', 'Chrome', 'Get Started', 'Install'),
            ('wrong current', 'Chrome', 'aria-current="page"', 'aria-current="location"'),
            ('stale footer', 'Chrome', 'Status as of 2026-09-16', 'Status as of 2020-01-01'),
            ('skip destination', 'Chrome', 'href="#main-content"', 'href="#absent"'),
            ('missing main', 'Chrome', '<main id=', '<div id='),
            ('language', 'Metadata', 'lang="en"', 'lang="fr"'),
            ('title', 'Metadata', 'chitta — Home', 'Home'),
            ('description', 'Metadata', 'name="description"', 'name="keywords"'),
            ('favicon', 'Metadata', 'rel="icon"', 'rel="alternate"'),
            ('heading gap', 'Headings', '<h2>Introduction</h2>', '<h3>Introduction</h3>'),
            ('two titles', 'Headings', '<h2>Introduction</h2>', '<h1>Introduction</h1>'),
            ('duplicate ID', 'Headings', '<p>Memory.</p>', '<p id="intro">Memory.</p>'),
            ('image alt', 'Layout', '<p>Memory.</p>', '<img src="/favicon.svg">'),
            ('table overflow', 'Layout', '<p>Memory.</p>', '<table><tr><td>x</td></tr></table>'),
            ('inline CSS', 'Layout', '<p>Memory.</p>', '<p style="width:2000px">Memory.</p>'),
            ('broken anchor', 'Links', '<p>Memory.</p>', '<a href="#absent">Missing</a>'),
            ('broken file', 'Links', '<p>Memory.</p>', '<a href="/missing.html">Missing</a>'),
            ('unpublished file', 'Links', '<p>Memory.</p>', '<a href="../CHANGELOG.md">History</a>'),
            ('TOC without heading', 'Headings', '<p>Memory.</p>',
             '<p id="empty">Memory.</p><a class="submenu-toc-link" href="#empty">Empty</a>'),
        ]
        for label, category, old, new in cases:
            with self.subTest(label=label):
                self.assertTrue(self.errors(self.source.replace(old, new))[category])

    def test_root_relative_nested_and_encoded_fragments(self):
        path, fragment = site.destination(site.DOCS / 'dreams/index.html', '/index.html#%69ntro')
        self.assertEqual(path, self.path)
        self.assertEqual(fragment, 'intro')
        self.assertEqual(site.destination(self.path, 'https://chitta.vaklab.dev/#intro'),
                         (self.path, 'intro'))
        self.assertEqual(site.destination(self.path, 'https://example.org/#intro'), (None, ''))

    def test_script_strings_are_not_html_elements(self):
        source = self.source.replace('</main>', '<script>const x = "<h1>fake</h1>";</script></main>')
        self.assertFalse(any(self.errors(source).values()))


if __name__ == '__main__':
    unittest.main()
