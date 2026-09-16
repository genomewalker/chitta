# Static site UX maintenance

The published artifact is docs/ from .github/workflows/pages.yml. This change
uses the existing static HTML/CSS structure; it does not deploy anything.

- `scripts/site_common.py` defines the menu, status date, footer, and version
  derived from the first numbered release in CHANGELOG.md.
- After menu/date/release changes, run `python3 scripts/sync-site-chrome.py`.
  Checked-in HTML keeps navigation usable without JavaScript.
- `docs/styles.css` retains the design tokens and owns responsive navigation,
  code overflow, and scrollable table regions. `docs/site-shell.css` loads last
  to keep shared controls consistent despite page-local styles.
- `docs/content-styles.css` holds deduplicated former static style attributes.
  Visualization scripts may still set runtime positions, colors, and visibility.
- Root-relative chrome/assets make the custom-domain 404 work at any URL depth.
  A site served under a path prefix would need its URLs adapted.
- Menu destinations mark themselves with `aria-current="page"`. Dream articles
  and 404 use their footer permalink because no menu item is that exact page.
- All pages have a focusable main landmark containing their h1. Tables have
  named, keyboard-scrollable regions. TOCs point to sections with real headings.
- The existing dark content palette is preserved. Shared chrome follows light
  OS preference; native controls use dark color-scheme on dark content.

Gates (no browser, network, service or third-party Python packages required):

```sh
python3 scripts/check-site.py
bash scripts/check-docs-links.sh
bash scripts/check-citations.sh
python3 -m unittest discover -s scripts/tests -p test_check_site.py
```

The legacy `python3 scripts/check-docs-links.py` entrypoint delegates to the same
recursive shell checker. It checks same-document and cross-page HTML anchors,
root-relative links, nested index pages, and percent-encoded URLs. Remote URLs
are excluded; successful local checks do not imply external-service health.

The per-page changes and exact checker table are recorded in
`docs/DOCS-AUDIT-2026-09-16.md`, under “Site UX pass”. CSS contracts and parsed
HTML do not prove visual layout, contrast, keyboard behavior in browsers,
WebGL rendering, or live backend connectivity.

After changing HTML line counts, run `bash scripts/check-citations.sh --write`
to refresh citation usage locations, then rerun the read-only citations gate.
The generated References blocks must remain intact inside the main landmark.
