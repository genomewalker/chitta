#!/usr/bin/env bash
# Reduced from the repository's hook pattern; never executed by the test.
source ./lib.sh
. ./config.sh
render_context() {
    printf '%s\n' "$1"
}
function prompt_hook {
    local context
    context=$(render_context ready)
    render_context "$context"
}
prompt_hook
# Comments and heredocs are data, not definitions or commands.
cat <<'TEXT'
fake_function() { prompt_hook; }
TEXT
