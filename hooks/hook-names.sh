#!/usr/bin/env bash
# Single definition of the entry-point hook names that may appear as legacy
# user-level commands in ~/.claude/settings.json.
#
# The plugin's hooks.json is authoritative once the plugin is enabled, so both
# scripts/smart-install.sh and scripts/sync-installed-hooks.sh strip settings
# entries matching these names. Keeping the list in one place stops the three
# former copies of the alternation from drifting apart.
#
# This is a data file: sourcing it must stay free of side effects so installers
# can read it before any hook is installed.
#
# NOT the same set as hooks/install-manifest.txt — that manifest also carries
# shared cores and libraries (lib.sh, prompt-core.sh, distill.sh, *.py) which
# are never named directly in a settings.json command.

CC_SOUL_SETTINGS_HOOK_NAMES=(
    subconscious
    session-start-hook
    compact-restore-hook
    resume-inject-hook
    prompt-hook
    stop-hook
    session-end-hook
    pre-compact-hook
    subagent-stop-hook
    file-changed-hook
    pre-tool-hook
    post-bash-hook
    log-bash-history
    memory-intercept
)

# Oniguruma regex for jq's test(); pass via --arg, never interpolated.
printf -v CC_SOUL_SETTINGS_HOOK_RE '(%s)\\.sh' \
    "$(IFS='|'; printf '%s' "${CC_SOUL_SETTINGS_HOOK_NAMES[*]}")"
