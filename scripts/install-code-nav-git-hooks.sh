#!/usr/bin/env bash
# Preserve existing hooks and custom core.hooksPath; install only when invoked.
set -euo pipefail
repo=${1:-.}
script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
root=$(git -C "$repo" rev-parse --show-toplevel)
hooks=$(git -C "$root" rev-parse --path-format=absolute --git-path hooks)
mkdir -p "$hooks"
for event in post-commit post-checkout; do
    hook=$hooks/$event
    if [[ -L "$hook" ]]; then
        printf 'code-nav: refusing to edit symlink hook %s\n' "$hook" >&2
        continue
    fi
    if [[ -f "$hook" ]] && grep -q '# chitta-code-nav' "$hook"; then continue; fi
    previous=$hook.before-chitta
    if [[ -e "$previous" ]]; then
        printf 'code-nav: backup already exists: %s\n' "$previous" >&2
        exit 1
    fi
    if [[ -e "$hook" ]]; then mv "$hook" "$previous"; fi
    {
        printf '#!/usr/bin/env bash\n# chitta-code-nav\nrc=0\n'
        printf 'if [[ -x %q ]]; then %q "$@" || rc=$?; fi\n' "$previous" "$previous"
        # shellcheck disable=SC2016 # Expand rc in the installed hook.
        printf 'bash %q\nexit "$rc"\n' "$script_dir/code-nav-git-hook.sh"
    } > "$hook"
    chmod +x "$hook"
done
