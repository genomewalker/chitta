#!/usr/bin/env bash
# Launch one Codex (Astra) stream with the repo's turn and token discipline.
#
#   scripts/codex-stream.sh <name> <branch> <spec.md> [effort] [base]
#     name    stream id (worktree codex-wt-<name>, pidfile codex-specs/<name>.pid)
#     branch  new branch for the worktree (cut from base, default origin/main)
#     spec    the task; codex-plugin/stream-preamble.md is prepended
#     effort  high (implementation, default) | medium (docs, tables, measurement runs) | low
#
# Prints the pid, the log path and the resume command. Never installs, deploys
# or pushes; the lead reviews and merges. Resume with:
#   codex exec resume <session id from the log> ... "$(cat follow-up.md)"
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
name=${1:?name}; branch=${2:?branch}; spec=${3:?spec}; effort=${4:-high}; base=${5:-origin/main}
case "$effort" in high|medium|low) ;; *) echo "effort must be high|medium|low (never ultra unattended)" >&2; exit 2 ;; esac
S="${CHITTA_CODEX_SPECS:-/projects/caeg/scratch/kbd606/tmp/codex-specs}"
W="${CHITTA_CODEX_WORKTREES:-/projects/caeg/scratch/kbd606/tmp}/codex-wt-$name"
mkdir -p "$S"
[[ -f "$spec" ]] || { echo "spec not found: $spec" >&2; exit 2; }
git -C "$ROOT" fetch -q origin
if [[ ! -d "$W" ]]; then
    git -C "$ROOT" worktree add -q -b "$branch" "$W" "$base"
    git -C "$W" submodule update --init -q chitta-field
fi
prompt="$S/$name.prompt.md"
{ cat "$spec"; printf '\n\n---\n'; cat "$ROOT/codex-plugin/stream-preamble.md"; } > "$prompt"
( cd "$W" && setsid nohup codex exec -C "$W" --approve-for-me --skip-git-repo-check \
    -m "${CHITTA_CODEX_MODEL:-gpt-6-astra}" -c "model_reasoning_effort=$effort" \
    -o "$S/$name.last.md" "$(cat "$prompt")" </dev/null >"$S/$name.log" 2>&1 &
  echo $! >"$S/$name.pid" )
sleep 2
printf 'stream %s: pid %s effort %s\n  worktree %s\n  log %s\n  status: kill -0 $(cat %s/%s.pid)\n' \
    "$name" "$(cat "$S/$name.pid")" "$effort" "$W" "$S/$name.log" "$S" "$name"
