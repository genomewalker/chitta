#!/usr/bin/env bash
# Synchronize the canonical repo hook set into active Claude/Codex installs.
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
HOOKS_SRC="$ROOT_DIR/hooks"
HOOK_MANIFEST="$HOOKS_SRC/install-manifest.txt"
# shellcheck source=../hooks/hook-names.sh
source "$HOOKS_SRC/hook-names.sh"
CHECK_ONLY=false
[[ "${1:-}" == "--check" ]] && CHECK_ONLY=true

[[ -f "$HOOK_MANIFEST" ]] || {
    echo "[cc-soul] missing $HOOK_MANIFEST" >&2
    exit 1
}

# Ownership rule (single sync owner, see repo CLAUDE.md "Dev install"): dev-
# install.sh SYMLINKS ~/.claude/hooks/*.sh and the marketplace chitta-mcp/*.py
# straight at this repo — that live path is dev-install's, not ours. Never
# overwrite a destination that is already such a symlink with a copy; skip it
# and say so. Every other destination this script touches (the plugin cache
# under installed_plugins.json's installPath, and the Codex cache) has no
# symlink alternative and is owned by this script alone.
drift=0
sync_one() {
    local source="$1" target="$2" mode="${3:-0755}"
    if [[ -L "$target" ]]; then
        local resolved
        resolved="$(readlink -f "$target" 2>/dev/null || true)"
        if [[ -n "$resolved" && "$resolved" == "$ROOT_DIR"/* ]]; then
            echo "[owner:symlink] $target -> repo (dev-install.sh owns this destination; not copying)"
            return 0
        fi
    fi
    if [[ -f "$target" ]] && cmp -s "$source" "$target"; then
        return 0
    fi
    drift=$((drift + 1))
    if [[ "$CHECK_ONLY" == "true" ]]; then
        echo "[drift] $target"
        return 0
    fi
    mkdir -p "$(dirname "$target")"
    local stage="$(dirname "$target")/.$(basename "$target").cc-soul-sync"
    install -m "$mode" "$source" "$stage"
    mv -f "$stage" "$target"
    echo "[sync] $target (sync-installed-hooks.sh owns this destination)"
}

# User-level fallback hooks. These are removed from settings when the plugin is
# enabled, but keeping the files synchronized supports plugin-disabled installs.
# Skips any file dev-install.sh already symlinked straight at the repo.
echo "[cc-soul] owner: ~/.claude/hooks/* (dev-install.sh symlinks these when run; this script only backstops non-symlinked installs)"
while IFS= read -r script; do
    [[ -z "$script" || "$script" == \#* ]] && continue
    sync_one "$HOOKS_SRC/$script" "$HOME/.claude/hooks/$script"
done < "$HOOK_MANIFEST"

# Claude executes hooks from its versioned plugin cache. During local
# development, refresh that exact active install without guessing the version.
# This destination is never symlinked by dev-install.sh — sync-installed-
# hooks.sh owns it outright.
installed_json="$HOME/.claude/plugins/installed_plugins.json"
# Every Claude plugin cache that can still execute our hooks: the current key,
# the pre-rename key, and any leftover cache directory on disk. A session that
# started under the old name keeps running from the old directory for its whole
# life (seen 2026-09-14: that copy was frozen at 09-02 and every hook fix since
# was invisible to it), so all of them are synced, not just the first match.
claude_roots=()
if [[ -f "$installed_json" ]]; then
    while IFS= read -r p; do [[ -n "$p" && -d "$p" ]] && claude_roots+=("$p"); done < <(jq -r '
      [.plugins["chitta@genomewalker-chitta"], .plugins["cc-soul@genomewalker-cc-soul"]] | flatten
      | .[]? | select(.scope == "user") | .installPath
    ' "$installed_json" 2>/dev/null)
fi
for legacy in "$HOME"/.claude/plugins/cache/genomewalker-cc-soul/cc-soul/*/ "$HOME"/.claude/plugins/cache/genomewalker-chitta/chitta/*/; do
    [[ -d "$legacy/hooks" ]] || continue
    legacy="${legacy%/}"
    case " ${claude_roots[*]} " in *" $legacy "*) ;; *) claude_roots+=("$legacy") ;; esac
done
for claude_root in "${claude_roots[@]}"; do
    echo "[cc-soul] owner: $claude_root/{hooks,chitta-mcp}/* (plugin cache — sync-installed-hooks.sh owns this outright)"
    while IFS= read -r script; do
        [[ -z "$script" || "$script" == \#* ]] && continue
        sync_one "$HOOKS_SRC/$script" "$claude_root/hooks/$script"
    done < "$HOOK_MANIFEST"
    sync_one "$HOOKS_SRC/hooks.json" "$claude_root/hooks/hooks.json" 0644
    # hook_*.py: the thin shell hooks exec these (Phase 5b); a cache without
    # them fails every hook with 'can't open file hook_client.py'.
    for module in session_registry.py resume_selector.py task_ledger.py \
                  thread_inference.py resume_capsule.py \
                  outcome_ledger.py daemon_client.py \
                  hook_client.py hook_ancillary.py hook_maintenance.py \
                  hook_prompt.py hook_session.py; do
        [[ -f "$ROOT_DIR/chitta-mcp/$module" ]] && sync_one "$ROOT_DIR/chitta-mcp/$module" "$claude_root/chitta-mcp/$module" 0755
    done
done

# Codex hook commands point at ROOT_DIR, while its cached skills need their own
# refresh so both frontends follow the same recap/resume ownership protocol.
# Also never symlinked by dev-install.sh — sync-installed-hooks.sh owns it.
codex_root="$HOME/.codex/plugins/cache/local/chitta/local"
[[ -d "$codex_root" ]] || codex_root="$HOME/.codex/plugins/cache/local/cc-soul/local"
if [[ -d "$codex_root" ]]; then
    echo "[cc-soul] owner: $codex_root/skills/* (Codex cache — sync-installed-hooks.sh owns this outright)"
    for skill in recap resume; do
        sync_one "$ROOT_DIR/codex-plugin/skills/$skill/SKILL.md" \
                 "$codex_root/skills/$skill/SKILL.md" 0644
    done
fi

settings="$HOME/.claude/settings.json"
if [[ "$CHECK_ONLY" == "true" && -f "$settings" ]] && \
   jq -e '(if (.enabledPlugins // {} | has("chitta@genomewalker-chitta")) then .enabledPlugins["chitta@genomewalker-chitta"] else .enabledPlugins["cc-soul@genomewalker-cc-soul"] end) == true' "$settings" >/dev/null; then
    if jq -e --arg re "$CC_SOUL_SETTINGS_HOOK_RE" '
      [.hooks[][]?.hooks[]?
       | (.command? // "")
       | select(test($re))]
      | length > 0
    ' "$settings" >/dev/null; then
        echo "[drift] duplicate user-level cc-soul hook entries in $settings"
        drift=$((drift + 1))
    fi
fi

# Compare Codex's effective hook JSON by applying the idempotent configurator
# to a staged copy, preserving unrelated hooks in the comparison.
if [[ "$CHECK_ONLY" == "true" ]]; then
    codex_hooks="$HOME/.codex/hooks.json"
    codex_stage=$(mktemp)
    if [[ -f "$codex_hooks" ]]; then
        install -m 0600 "$codex_hooks" "$codex_stage"
    else
        printf '%s\n' '{"hooks":{}}' > "$codex_stage"
    fi
    HOOKS_FILE="$codex_stage" "$ROOT_DIR/scripts/configure-codex-hooks.sh" >/dev/null
    if [[ ! -f "$codex_hooks" ]] || ! cmp -s "$codex_hooks" "$codex_stage"; then
        echo "[drift] $codex_hooks"
        drift=$((drift + 1))
    fi
    rm -f "$codex_stage"
fi

if [[ "$CHECK_ONLY" == "false" ]]; then
    "$ROOT_DIR/scripts/configure-codex-hooks.sh" >/dev/null

    # Plugin hooks.json is authoritative. Strip only cc-soul's legacy global
    # hook commands while preserving unrelated user hooks in the same event.
    if [[ -f "$settings" ]] && \
       jq -e '(if (.enabledPlugins // {} | has("chitta@genomewalker-chitta")) then .enabledPlugins["chitta@genomewalker-chitta"] else .enabledPlugins["cc-soul@genomewalker-cc-soul"] end) == true' "$settings" >/dev/null; then
        stage="${settings}.cc-soul-sync"
        jq --arg re "$CC_SOUL_SETTINGS_HOOK_RE" '
          def is_cc_soul_hook:
            ((.command? // "") | test($re));
          def strip_cc_soul:
            map(.hooks = ((.hooks // []) | map(select((is_cc_soul_hook | not)))))
            | map(select((.hooks | length) > 0));
          .hooks = ((.hooks // {}) | with_entries(.value |= strip_cc_soul))
          | .hooks = (.hooks | with_entries(select((.value | length) > 0)))
        ' "$settings" > "$stage"
        if cmp -s "$settings" "$stage"; then
            rm -f "$stage"
        else
            mv -f "$stage" "$settings"
            echo "[sync] removed duplicate user-level cc-soul hook entries"
        fi
    fi
fi

if [[ "$CHECK_ONLY" == "true" ]]; then
    if (( drift > 0 )); then
        echo "[cc-soul] $drift installed hook/skill files differ from the repo" >&2
        exit 1
    fi
    echo "[cc-soul] installed hooks and shared skills match the repo"
else
    echo "[cc-soul] hook/skill synchronization complete ($drift files updated)"
fi
