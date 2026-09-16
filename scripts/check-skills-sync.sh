#!/usr/bin/env bash
# Check the shipped Codex mirror against the canonical marketplace skills.
set -euo pipefail
root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
source_dir="$root/skills"
mirror_dir="$root/codex-plugin/skills"

if [[ ! -d "$source_dir" || ! -d "$mirror_dir" ]]; then
    echo "skills source or Codex mirror missing" >&2
    exit 1
fi
if [[ -e "$mirror_dir/_conventions" || -L "$mirror_dir/_conventions" ]]; then
    echo "skills/_conventions must not be shipped in the Codex mirror" >&2
    exit 1
fi
# Anchor the exclusion at the root: a nested _conventions is skill content.
changes=$(rsync --recursive --links --checksum --dry-run --delete \
    --delete-excluded --itemize-changes --exclude='/_conventions/' \
    "$source_dir/" "$mirror_dir/")
if [[ -n "$changes" ]]; then
    printf '%s\n' "$changes" >&2
    echo "skill drift: run bash scripts/sync-skills.sh from the repository" >&2
    exit 1
fi
echo "skills in sync"
