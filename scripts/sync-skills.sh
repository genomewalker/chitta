#!/usr/bin/env bash
# Update only the repository's shipped mirror; never installed plugin caches.
set -euo pipefail
root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"

# Match release.sh's source and convention exclusion. Remove obsolete mirror
# files too, including accidentally copied private conventions.
rsync -a --delete --delete-excluded --exclude='/_conventions/' \
    "$root/skills/" "$root/codex-plugin/skills/"
bash "$root/scripts/check-skills-sync.sh"
