#!/bin/bash
# Test hooks/hook-names.sh: it is the single definition of the legacy
# settings.json hook-name regex that scripts/smart-install.sh and
# scripts/sync-installed-hooks.sh strip with. Regressions here silently delete
# a user's unrelated settings.json hooks, so pin the exact regex text, the
# matching behavior through jq, and the absence of side effects.
set -u
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ROOT_DIR="$(dirname "$SCRIPT_DIR")"
FAIL=0
assert() { if ! eval "$2"; then echo "FAIL: $1"; FAIL=1; else echo "ok: $1"; fi; }

source "$SCRIPT_DIR/hook-names.sh"

# --- the regex is exactly what the three former inline copies contained ---
EXPECTED='(subconscious|session-start-hook|compact-restore-hook|resume-inject-hook|prompt-hook|stop-hook|session-end-hook|pre-compact-hook|subagent-stop-hook|file-changed-hook|pre-tool-hook|post-bash-hook|log-bash-history|memory-intercept)\.sh'
assert "regex matches the pre-refactor literal" "[[ \"\$CC_SOUL_SETTINGS_HOOK_RE\" == '$EXPECTED' ]]"
assert "14 hook names" "[[ \${#CC_SOUL_SETTINGS_HOOK_NAMES[@]} -eq 14 ]]"
assert "regex is never empty" "[[ -n \"\$CC_SOUL_SETTINGS_HOOK_RE\" ]]"

# --- jq --arg round trip: every listed name matches, unrelated hooks do not ---
for name in "${CC_SOUL_SETTINGS_HOOK_NAMES[@]}"; do
    # Literal settings.json command, intentionally not a home-directory expansion.
    # shellcheck disable=SC2088
    got=$(jq -n --arg re "$CC_SOUL_SETTINGS_HOOK_RE" \
              --arg c "~/.claude/hooks/${name}.sh" '$c | test($re)')
    assert "matches $name.sh" "[[ '$got' == true ]]"
done
for other in my-own-hook.sh notify.sh prompt-hook.py stop-hook.txt; do
    # Literal settings.json command, intentionally not a home-directory expansion.
    # shellcheck disable=SC2088
    got=$(jq -n --arg re "$CC_SOUL_SETTINGS_HOOK_RE" \
              --arg c "~/.claude/hooks/$other" '$c | test($re)')
    assert "does not match $other" "[[ '$got' == false ]]"
done

# --- sourcing must stay side-effect free: installers read it before install ---
BEFORE=$(mktemp); AFTER=$(mktemp)
trap 'rm -f "$BEFORE" "$AFTER"' EXIT
env | sort > "$BEFORE"
( source "$SCRIPT_DIR/hook-names.sh"; env | sort ) > "$AFTER"
assert "sourcing exports nothing new" "diff -q '$BEFORE' '$AFTER' >/dev/null"
assert "sourcing writes no output" "[[ -z \"\$(source '$SCRIPT_DIR/hook-names.sh' 2>&1)\" ]]"

# --- the two consumers must not have re-inlined a private copy ---
for f in "$ROOT_DIR/scripts/smart-install.sh" "$ROOT_DIR/scripts/sync-installed-hooks.sh"; do
    assert "no inline copy in $(basename "$f")" "! grep -q 'subconscious|session-start-hook' '$f'"
    assert "$(basename "$f") sources hook-names.sh" "grep -q 'hook-names.sh' '$f'"
done

exit $FAIL
