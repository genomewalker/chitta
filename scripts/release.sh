#!/bin/bash
# Release automation script for chitta
#
# Usage:
#   ./scripts/release.sh patch       # Bug fixes (2.56.0 → 2.56.1)
#   ./scripts/release.sh minor       # New features (2.56.0 → 2.57.0)
#   ./scripts/release.sh major       # Breaking changes (2.56.0 → 3.0.0)
#   ./scripts/release.sh 2.57.0      # Explicit version
#   ./scripts/release.sh minor -y    # Skip confirmation
#
# SemVer Guidelines:
#   MAJOR: Breaking changes (protocol change, removed tool, renamed param)
#   MINOR: New features, backward compatible (new tool, new param, new skill)
#   PATCH: Bug fixes, no new features (fix crash, fix logic error)

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(dirname "$SCRIPT_DIR")"
cd "$REPO_DIR"

# Get current version from version.hpp
get_current_version() {
    grep '#define CHITTA_VERSION' chitta/include/chitta/version.hpp | sed 's/.*"\([^"]*\)".*/\1/'
}

# Bump version based on type
bump_version() {
    local current="$1"
    local type="$2"

    IFS='.' read -r major minor patch <<< "$current"

    case "$type" in
        major)
            echo "$((major + 1)).0.0"
            ;;
        minor)
            echo "$major.$((minor + 1)).0"
            ;;
        patch)
            echo "$major.$minor.$((patch + 1))"
            ;;
        *)
            echo "$type"  # Explicit version
            ;;
    esac
}

# Validate version format
validate_version() {
    [[ "$1" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]
}

# Main
BUMP_TYPE=""
AUTO_CONFIRM=false
LOCAL_BUILD=false

for arg in "$@"; do
    case "$arg" in
        -y|--yes)
            AUTO_CONFIRM=true
            ;;
        -l|--local)
            LOCAL_BUILD=true
            ;;
        *)
            if [[ -z "$BUMP_TYPE" ]]; then
                BUMP_TYPE="$arg"
            fi
            ;;
    esac
done

if [[ -z "$BUMP_TYPE" ]]; then
    echo "Usage: $0 <patch|minor|major|X.Y.Z> [-y|--yes] [-l|--local]"
    echo ""
    echo "Options:"
    echo "  -y, --yes    Skip confirmation prompt"
    echo "  -l, --local  Also build and install locally after push"
    echo ""
    echo "SemVer Guidelines:"
    echo "  patch  Bug fixes only (2.56.0 → 2.56.1)"
    echo "  minor  New features, backward compatible (2.56.0 → 2.57.0)"
    echo "  major  Breaking changes (2.56.0 → 3.0.0)"
    echo ""
    echo "Examples:"
    echo "  $0 patch          # Fixed a bug"
    echo "  $0 minor -y       # Added new tool, skip confirm"
    echo "  $0 patch -y -l    # Bug fix + local build"
    exit 1
fi

CURRENT_VERSION=$(get_current_version)
NEW_VERSION=$(bump_version "$CURRENT_VERSION" "$BUMP_TYPE")

if ! validate_version "$NEW_VERSION"; then
    echo "Error: Invalid version format: $NEW_VERSION"
    echo "Version must be in format X.Y.Z"
    exit 1
fi

echo "=== Release: $CURRENT_VERSION → $NEW_VERSION ==="

# Check for uncommitted changes
if ! git diff --quiet || ! git diff --cached --quiet; then
    echo "Error: Uncommitted changes. Commit or stash first."
    exit 1
fi

# FFI declaration drift gate: the hand-rolled extern block in field_store.hpp
# must match ffi.rs by name AND arity (C linkage makes arity drift silent UB).
if ! python3 "$REPO_DIR/tools/ffi_decl_check.py"; then
    echo "Error: FFI declaration drift — fix field_store.hpp vs chitta-field/src/ffi.rs."
    exit 1
fi

# Ensure all hook scripts are executable (git on some filesystems strips the bit)
chmod +x hooks/*.sh 2>/dev/null || true

# Verify submodule commits exist on remote
echo "Verifying submodules are pushed..."
git submodule foreach --quiet '
    LOCAL_COMMIT=$(git rev-parse HEAD)
    REMOTE_BRANCH=$(git rev-parse --abbrev-ref HEAD)
    if ! git branch -r --contains "$LOCAL_COMMIT" 2>/dev/null | grep -q "origin/$REMOTE_BRANCH"; then
        echo "Error: submodule $name commit $LOCAL_COMMIT not pushed to origin/$REMOTE_BRANCH"
        echo "  Run: cd $toplevel/$sm_path && git push origin $REMOTE_BRANCH"
        exit 1
    fi
' || exit 1

# Confirm
if [[ "$AUTO_CONFIRM" != "true" ]]; then
    read -p "Proceed with release v$NEW_VERSION? [y/N] " confirm
    if [[ "$confirm" != "y" && "$confirm" != "Y" ]]; then
        echo "Aborted."
        exit 0
    fi
fi

# Cross-platform sed -i (macOS needs '', Linux doesn't)
sedi() {
    if [[ "$OSTYPE" == "darwin"* ]]; then
        sed -i '' "$@"
    else
        sed -i "$@"
    fi
}

# Update version.hpp
echo "Updating chitta/include/chitta/version.hpp..."
sedi "s/#define CHITTA_VERSION \"[^\"]*\"/#define CHITTA_VERSION \"$NEW_VERSION\"/" \
    chitta/include/chitta/version.hpp

# Update plugin.json (Claude Code + Codex)
echo "Updating .claude-plugin/plugin.json..."
sedi "s/\"version\": \"[^\"]*\"/\"version\": \"$NEW_VERSION\"/" \
    .claude-plugin/plugin.json

echo "Updating codex-plugin/.codex-plugin/plugin.json..."
sedi "s/\"version\": \"[^\"]*\"/\"version\": \"$NEW_VERSION\"/" \
    codex-plugin/.codex-plugin/plugin.json

# Sync skills from source to codex plugin
echo "Syncing skills to codex-plugin/..."
rsync -a --delete --exclude='_conventions' skills/ codex-plugin/skills/

# Update chitta-mcp/pyproject.toml
echo "Updating chitta-mcp/pyproject.toml..."
sedi "s/^version = \"[^\"]*\"/version = \"$NEW_VERSION\"/" \
    chitta-mcp/pyproject.toml

# Update docs/index.html badge
echo "Updating docs/index.html badge..."
# Site: the shared chrome comes from scripts/site_common.py (STATUS_DATE, version from
# CHANGELOG.md); stamp today's date, re-sync every page, then align in-page stamps.
sedi "s/^STATUS_DATE = '[0-9-]*'/STATUS_DATE = '$(date +%F)'/" scripts/site_common.py
python3 scripts/sync-site-chrome.py > /dev/null || { echo "site chrome sync failed"; exit 1; }
# site_common.footer() is the authority and reads the version from CHANGELOG.md,
# so re-syncing the chrome after the bump is all the site needs. Do not rewrite
# that footer text here: check-site.py compares pages against footer() verbatim.
for page in docs/*.html docs/vedanta/index.html; do
    sedi -E "s/Status as of 20[0-9]{2}-[0-9]{2}-[0-9]{2}/Status as of $(date +%F)/g" "$page"
done

# The contract snapshot carries plugin.json's version: regenerate it with the bump.
echo "Regenerating contracts/ for the new version..."
bash scripts/contract-snapshot.sh write > /dev/null || { echo "contract snapshot failed"; exit 1; }

# Verify updates
grep -q "\"$NEW_VERSION\"" chitta/include/chitta/version.hpp || { echo "version.hpp update failed"; exit 1; }
grep -q "\"$NEW_VERSION\"" .claude-plugin/plugin.json || { echo "plugin.json update failed"; exit 1; }
grep -q "\"$NEW_VERSION\"" codex-plugin/.codex-plugin/plugin.json || { echo "codex plugin.json update failed"; exit 1; }
grep -q "\"$NEW_VERSION\"" chitta-mcp/pyproject.toml || { echo "pyproject.toml update failed"; exit 1; }
grep -q "v$NEW_VERSION" docs/index.html || { echo "docs/index.html update failed"; exit 1; }

# Commit version bump
echo "Committing version bump..."
git add contracts docs/*.html docs/vedanta/index.html scripts/site_common.py chitta/include/chitta/version.hpp .claude-plugin/plugin.json codex-plugin/.codex-plugin/plugin.json codex-plugin/skills chitta-mcp/pyproject.toml docs/index.html
git commit -m "chore: bump version to $NEW_VERSION"

# Create and push tag
echo "Creating tag v$NEW_VERSION..."
git tag "v$NEW_VERSION"

echo "Pushing to origin..."
git push origin main
git push origin "v$NEW_VERSION"

echo ""
echo "=== Release v$NEW_VERSION initiated ==="
echo ""
echo "GitHub Actions will build and publish:"
echo "  • linux-x64 binaries"
echo "  • macos-x64 binaries"
echo "  • macos-arm64 binaries"
echo "  • embedding model (bge-large-en-v1.5.gguf)"
echo ""
echo "Monitor: https://github.com/genomewalker/chitta/actions"
echo "Release: https://github.com/genomewalker/chitta/releases/tag/v$NEW_VERSION"

# Local build if requested
if [[ "$LOCAL_BUILD" == "true" ]]; then
    echo ""
    echo "=== Local Build ==="

    # Build
    echo "Building chitta..."
    cd "$REPO_DIR/chitta"
    if cmake --build build --parallel; then
        echo "Build successful"
    else
        echo "Build failed"
        exit 1
    fi

    # Stop daemon
    echo "Stopping daemon..."
    pkill -9 chittad 2>/dev/null || true
    sleep 2

    # Install
    echo "Installing binaries..."
    cp "$REPO_DIR/bin/chitta" "$REPO_DIR/bin/chittad" ~/.claude/bin/
    # chitta_hintd only exists when built with CHITTA_WITH_LLAMA_CPP=ON.
    [ -f "$REPO_DIR/bin/chitta_hintd" ] && cp "$REPO_DIR/bin/chitta_hintd" ~/.claude/bin/

    # Verify
    INSTALLED=$("$HOME/.claude/bin/chitta" --version 2>/dev/null | grep -oE '[0-9]+\.[0-9]+\.[0-9]+')
    echo "Installed: chitta $INSTALLED"

    # Restart MCP
    echo "Restarting MCP server..."
    pkill -f "chitta mcp" 2>/dev/null || true

    echo ""
    echo "=== Local installation complete ==="
fi
