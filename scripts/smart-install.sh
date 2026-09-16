#!/bin/bash
# Smart Install Script for cc-soul
#
# Tries to download pre-built binaries, falls back to building from source.
# Runs as first hook on SessionStart.

set -e

# Ignore signals that might come from daemon shutdown
trap '' USR1 USR2

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PLUGIN_DIR="$(dirname "$SCRIPT_DIR")"
# Legacy settings-hook name list. `set -e` aborts if it is missing: an empty
# CC_SOUL_SETTINGS_HOOK_RE would make jq's test() match every hook command and
# strip the user's unrelated entries from settings.json.
# shellcheck source=../hooks/hook-names.sh
source "$PLUGIN_DIR/hooks/hook-names.sh"
CHITTA_DIR="$PLUGIN_DIR/chitta"
BUILD_DIR="$CHITTA_DIR/build"
BIN_DIR="${HOME}/.claude/bin"
MODELS_DIR="${HOME}/.claude/models"
MIND_PATH="${CHITTA_DB_PATH:-${HOME}/.claude/mind}"
MARKER="$PLUGIN_DIR/.install-complete"

# GitHub release URL base
GITHUB_REPO="genomewalker/chitta"
RELEASE_URL="https://github.com/$GITHUB_REPO/releases/download"

# Temp dir for downloaded source (cleaned up on exit)
SOURCE_BUILD_DIR=""
_DOWNLOAD_TMP=""
cleanup_tmp() { [[ -n "$_DOWNLOAD_TMP" ]] && rm -rf "$_DOWNLOAD_TMP"; }
trap cleanup_tmp EXIT

# Detect platform
detect_platform() {
    local os=$(uname -s | tr '[:upper:]' '[:lower:]')
    local arch=$(uname -m)

    case "$os" in
        linux)
            case "$arch" in
                x86_64) echo "linux-x64" ;;
                aarch64) echo "linux-arm64" ;;
                *) echo "unknown" ;;
            esac
            ;;
        darwin)
            case "$arch" in
                x86_64) echo "macos-x64" ;;
                arm64) echo "macos-arm64" ;;
                *) echo "unknown" ;;
            esac
            ;;
        *) echo "unknown" ;;
    esac
}

# Download file with curl or wget
download() {
    local url="$1"
    local output="$2"

    if command -v curl &> /dev/null; then
        curl -fsSL -o "$output" "$url" 2>/dev/null
    elif command -v wget &> /dev/null; then
        wget -q -O "$output" "$url" 2>/dev/null
    else
        return 1
    fi
}

# Verify checksum (empty expected = skip verification)
verify_checksum() {
    local file="$1"
    local expected="$2"

    [[ -z "$expected" ]] && return 0  # Skip if no checksum provided

    local actual
    if command -v sha256sum &>/dev/null; then
        actual=$(sha256sum "$file" | cut -d' ' -f1)
    elif command -v shasum &>/dev/null; then
        actual=$(shasum -a 256 "$file" | cut -d' ' -f1)
    else
        return 0  # Can't verify, assume OK
    fi

    [[ "$actual" == "$expected" ]]
}

# Fetch latest release version tag from GitHub API
fetch_latest_version() {
    local tmp
    tmp=$(mktemp)
    local api_url="https://api.github.com/repos/$GITHUB_REPO/releases/latest"
    if download "$api_url" "$tmp" && [[ -s "$tmp" ]]; then
        local tag
        if command -v jq &>/dev/null; then
            tag=$(jq -r '.tag_name // ""' "$tmp" 2>/dev/null)
        else
            tag=$(grep -o '"tag_name":"[^"]*"' "$tmp" | head -1 | sed 's/.*:"\([^"]*\)"/\1/')
        fi
        rm -f "$tmp"
        [[ -n "$tag" ]] && echo "${tag#v}" && return 0
    fi
    rm -f "$tmp"
    return 1
}

# Compare semver strings: returns 0 if $1 >= $2
semver_gte() {
    [[ "$(printf '%s\n' "$1" "$2" | sort -V | head -1)" == "$2" ]]
}

has_cxx_compiler() {
    command -v c++ &>/dev/null || command -v g++ &>/dev/null || command -v clang++ &>/dev/null
}

# Download source tarball for a given version into a temp dir.
# Sets SOURCE_BUILD_DIR to the extracted directory containing chitta/.
download_source_tarball() {
    local version="$1"
    _DOWNLOAD_TMP=$(mktemp -d)
    local tarball="$_DOWNLOAD_TMP/cc-soul.tar.gz"
    local url="https://github.com/$GITHUB_REPO/archive/refs/tags/v$version.tar.gz"

    echo "[cc-soul] Downloading source v$version..."
    if download "$url" "$tarball" && [[ -s "$tarball" ]]; then
        tar -xzf "$tarball" -C "$_DOWNLOAD_TMP" 2>/dev/null
        local extracted_dir
        extracted_dir=$(find "$_DOWNLOAD_TMP" -maxdepth 1 -type d \( -name "chitta-*" -o -name "cc-soul-*" \) | head -1)
        if [[ -n "$extracted_dir" && -d "$extracted_dir/chitta" ]]; then
            SOURCE_BUILD_DIR="$extracted_dir"
            return 0
        fi
    fi
    return 1
}

# Try to download pre-built binaries
download_binaries() {
    local version="$1"
    local platform="$2"
    local url="$RELEASE_URL/v$version/chitta-$platform.tar.gz"

    echo "[cc-soul] Downloading pre-built binaries ($platform)..."
    local tmp_file=$(mktemp)

    if download "$url" "$tmp_file"; then
        local tmp_extract
        tmp_extract=$(mktemp -d)
        if tar -xzf "$tmp_file" -C "$tmp_extract" 2>/dev/null; then
            rm -f "$tmp_file"
            # Verify extracted binaries work before touching BIN_DIR
            if "$tmp_extract/chittad" --help >/dev/null 2>&1 && \
               "$tmp_extract/chitta" --help >/dev/null 2>&1; then
                mkdir -p "$BIN_DIR"
                # Atomic install: running daemon keeps its old inode until restarted
                install -m 0755 "$tmp_extract/chittad" "$BIN_DIR/chittad"
                install -m 0755 "$tmp_extract/chitta"  "$BIN_DIR/chitta"
                rm -rf "$tmp_extract"
                echo "[cc-soul] Pre-built binaries installed"
                return 0
            else
                echo "[cc-soul] Pre-built binaries incompatible, will build from source"
                rm -rf "$tmp_extract"
                return 1
            fi
        fi
        rm -f "$tmp_file"
        rm -rf "$tmp_extract"
    fi

    return 1
}

# Download bge-large-en-v1.5 embedding model from GitHub release or HuggingFace
download_embed_model() {
    local version="$1"
    local dest="$MODELS_DIR/bge-large-en-v1.5.gguf"
    [[ -f "$dest" ]] && return 0

    mkdir -p "$MODELS_DIR"
    echo "[cc-soul] Downloading embedding model..."

    # Try release artifact first
    local url="$RELEASE_URL/v$version/embed-models.tar.gz"
    local tmp=$(mktemp)
    if download "$url" "$tmp" && [[ -s "$tmp" ]]; then
        local td=$(mktemp -d)
        tar -xzf "$tmp" -C "$td" 2>/dev/null && mv "$td"/*.gguf "$dest" 2>/dev/null && rm -rf "$td" "$tmp" && echo "[cc-soul] Embedding model installed" && return 0
        rm -rf "$td" "$tmp"
    fi
    rm -f "$tmp"

    # Fallback: download direct from HuggingFace
    if command -v python3 &>/dev/null; then
        python3 -c "
from huggingface_hub import hf_hub_download
import shutil, os
path = hf_hub_download(repo_id='CompendiumLabs/bge-large-en-v1.5-gguf', filename='bge-large-en-v1.5-q8_0.gguf')
shutil.copy(path, '$dest')
print('[cc-soul] Embedding model installed from HuggingFace')
" 2>/dev/null && return 0
    fi

    echo "[cc-soul] WARNING: Could not download embedding model — semantic recall will use LiteEncoder fallback"
    return 1
}

# Locate cargo, preferring rustup's toolchain over conda/system cargo
find_cargo() {
    # rustup gives us the toolchain-pinned cargo (respects rust-toolchain.toml)
    local rc
    rc=$(rustup which cargo 2>/dev/null) && echo "$rc" && return 0
    # Fall back to PATH, skip conda wrappers that may carry an old version
    command -v cargo 2>/dev/null && return 0
    return 1
}

# Build chitta-field Rust library (prerequisite for chittad)
# Sets CHITTA_FIELD_ROOT for the subsequent cmake call.
build_chitta_field() {
    local src_root="$1"
    local cf_dir="$src_root/chitta-field"

    # Preserve the embedding identity of an existing personal deployment. The
    # public release defaults to 1024-d BGE, while older/personal stores commonly
    # use 768-d Nomic. Rebuilding without this inference creates query vectors
    # that the existing chitta-field store rejects after an otherwise successful
    # upgrade.
    if [[ -z "${CHITTA_EMBED_DIM:-}" ]]; then
        local existing_service="${HOME}/.config/systemd/user/chittad.service"
        local existing_model=""
        if [[ -f "$existing_service" ]]; then
            existing_model=$(grep -oP '(?<=--embed-model )\S+' "$existing_service" 2>/dev/null || true)
        fi
        if [[ "$existing_model" == *nomic-embed-text* ]]; then
            export CHITTA_EMBED_DIM=768
            export CHITTA_EMBED_MODEL_ID="${CHITTA_EMBED_MODEL_ID:-nomic-embed-text-v1.5}"
            echo "[cc-soul] Preserving embedding identity: ${CHITTA_EMBED_MODEL_ID} (${CHITTA_EMBED_DIM}-d)"
        fi
    fi

    local build_identity="${CHITTA_EMBED_DIM:-1024}:${CHITTA_EMBED_MODEL_ID:-bge-large-en-v1.5}"
    local identity_file="$cf_dir/target/release/.chitta-embed-identity"

    # Already built? Require both fresh source and the same embedding identity.
    local lib_a="$cf_dir/target/release/libchitta_field.a"
    if [[ -f "$lib_a" ]]; then
        local ffi_src="$cf_dir/src/ffi.rs"
        local prior_identity=""
        [[ -f "$identity_file" ]] && prior_identity=$(<"$identity_file")
        if { [[ ! -f "$ffi_src" ]] || [[ "$lib_a" -nt "$ffi_src" ]]; } && \
           [[ "$prior_identity" == "$build_identity" ]]; then
            echo "[cc-soul] chitta-field already built (up to date)"
            export CHITTA_FIELD_ROOT="$cf_dir"
            return 0
        fi
        echo "[cc-soul] chitta-field source changed, rebuilding..."
    fi

    # Clone submodule if absent (source tarball won't include it)
    if [[ ! -f "$cf_dir/Cargo.toml" ]]; then
        if ! command -v git &>/dev/null; then
            echo "[cc-soul] ERROR: git not found, cannot fetch chitta-field" >&2
            return 1
        fi
        echo "[cc-soul] Fetching chitta-field..."
        git clone --depth 1 https://github.com/genomewalker/chitta-field.git "$cf_dir" 2>&1 | tail -3
    fi

    if ! command -v rustup &>/dev/null; then
        if ! find_cargo &>/dev/null; then
            echo "[cc-soul] ERROR: cargo/rustup not found. Install rustup: https://rustup.rs" >&2
            return 1
        fi
    fi

    # Read pinned toolchain from rust-toolchain.toml, fall back to stable
    local toolchain="stable"
    if [[ -f "$cf_dir/rust-toolchain.toml" ]]; then
        toolchain=$(grep -oP '(?<=channel = ")[^"]+' "$cf_dir/rust-toolchain.toml" 2>/dev/null || echo "stable")
    elif [[ -f "$cf_dir/rust-toolchain" ]]; then
        toolchain=$(cat "$cf_dir/rust-toolchain" | tr -d '[:space:]')
    fi

    echo "[cc-soul] Building chitta-field (toolchain: $toolchain)..."
    # Ensure the pinned toolchain is installed
    rustup toolchain install "$toolchain" --no-self-update 2>/dev/null || true

    # Resolve rustup-managed cargo and rustc explicitly.
    # This bypasses conda or system rustc (e.g. 1.70.0) that may appear first in PATH.
    local cargo_cmd rustc_cmd
    cargo_cmd=$(rustup which cargo 2>/dev/null) || cargo_cmd=$(find_cargo)
    rustc_cmd=$(rustup which rustc 2>/dev/null) || rustc_cmd=$(command -v rustc 2>/dev/null)

    if [[ -z "$cargo_cmd" || -z "$rustc_cmd" ]]; then
        echo "[cc-soul] ERROR: could not locate cargo/rustc via rustup" >&2
        return 1
    fi

    local pyo3_python=""
    if [[ -n "${PYO3_PYTHON:-}" && -x "${PYO3_PYTHON}" ]]; then
        pyo3_python="${PYO3_PYTHON}"
    elif command -v chitta-mcp &>/dev/null; then
        local mcp_python
        mcp_python=$(head -n 1 "$(command -v chitta-mcp)" 2>/dev/null | sed 's/^#!//')
        [[ -x "$mcp_python" ]] && pyo3_python="$mcp_python"
    elif command -v python3 &>/dev/null; then
        pyo3_python="$(command -v python3)"
    elif command -v python &>/dev/null; then
        pyo3_python="$(command -v python)"
    fi
    export CHITTA_BUILD_PYTHON="$pyo3_python"

    rm -f "$lib_a"
    (cd "$cf_dir" && \
        unset CARGO_TARGET_X86_64_UNKNOWN_LINUX_GNU_LINKER LDFLAGS CFLAGS CXXFLAGS PYO3_ENVIRONMENT_SIGNATURE VIRTUAL_ENV CONDA_PREFIX && \
        RUSTC="$rustc_cmd" PYO3_PYTHON="$pyo3_python" PYTHON_SYS_EXECUTABLE="$pyo3_python" "$cargo_cmd" build --release --lib 2>&1 | tail -8)

    if [[ ! -f "$cf_dir/target/release/libchitta_field.a" ]]; then
        echo "[cc-soul] ERROR: chitta-field build failed" >&2
        return 1
    fi

    printf '%s\n' "$build_identity" > "$identity_file"

    export CHITTA_FIELD_ROOT="$cf_dir"
    echo "[cc-soul] chitta-field built"
}

# Build from source
# Optional first arg: path to source root (defaults to $PLUGIN_DIR)
build_from_source() {
    local src_root="${1:-$PLUGIN_DIR}"
    local src_chitta="$src_root/chitta"
    local build_dir="$src_chitta/build"

    echo "[cc-soul] Building from source..."

    # Check dependencies
    if ! command -v cmake &> /dev/null; then
        echo "[cc-soul] ERROR: cmake not found. Please install cmake." >&2
        return 1
    fi

    if ! command -v make &> /dev/null; then
        echo "[cc-soul] ERROR: make not found. Please install make." >&2
        return 1
    fi

    # Build chitta-field first (sets CHITTA_FIELD_ROOT)
    build_chitta_field "$src_root" || return 1

    local plugin_bin="$src_root/bin"
    mkdir -p "$BIN_DIR" "$plugin_bin"

    # Clean build directory to avoid CMake cache conflicts, but preserve
    # FetchContent deps from a previous plugin version to avoid re-downloading.
    local prev_deps=""
    local _cache_dir
    for _cache_dir in "$HOME/.claude/plugins/cache/genomewalker-chitta" \
                      "$HOME/.claude/plugins/cache/genomewalker-cc-soul"; do
        [[ -d "$_cache_dir" ]] || continue
        # Find most recent prior version's _deps (sorted descending, skip current)
        local current_ver
        current_ver=$(basename "$PLUGIN_DIR")
        while IFS= read -r prev_dir; do
            local candidate="$prev_dir/chitta/build/_deps"
            if [[ "$(basename "$prev_dir")" != "$current_ver" && -d "$candidate" ]]; then
                prev_deps="$candidate"
                break
            fi
        done < <(find "$_cache_dir" -mindepth 2 -maxdepth 2 \
                      -name "chitta" -type d | sed 's|/chitta$||' | sort -Vr)
        [[ -n "$prev_deps" ]] && break
    done

    rm -rf "$build_dir"
    mkdir -p "$build_dir"

    # Reuse FetchContent deps from previous version (saves 5-8 min of CMake configure)
    if [[ -n "$prev_deps" ]]; then
        echo "[cc-soul] Reusing FetchContent deps from $(basename "$(dirname "$(dirname "$prev_deps")")")"
        cp -r "$prev_deps" "$build_dir/_deps"
    fi

    cd "$build_dir"

    # Use ccache if available (speeds up C++ recompilation significantly)
    local cmake_args="-DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_FLAGS= -DCMAKE_C_FLAGS="
    if command -v ccache &>/dev/null; then
        cmake_args="$cmake_args -DCMAKE_CXX_COMPILER_LAUNCHER=ccache -DCMAKE_C_COMPILER_LAUNCHER=ccache"
        echo "[cc-soul] ccache enabled"
    fi
    # Pass chitta-field root to cmake (set by build_chitta_field)
    if [[ -n "$CHITTA_FIELD_ROOT" ]]; then
        cmake_args="$cmake_args -DCHITTA_FIELD_ROOT=$CHITTA_FIELD_ROOT"
    fi
    # CMake's FindPython3 needs development headers/libraries, not merely the
    # PATH-first interpreter. Reuse the interpreter already validated for pyo3
    # so conda/HPC installs with a minimal system Python configure correctly.
    if [[ -n "${CHITTA_BUILD_PYTHON:-}" && -x "${CHITTA_BUILD_PYTHON}" ]]; then
        local python_root
        python_root=$(cd "$(dirname "${CHITTA_BUILD_PYTHON}")/.." && pwd)
        cmake_args="$cmake_args -DPython3_EXECUTABLE=${CHITTA_BUILD_PYTHON} -DPython3_ROOT_DIR=${python_root}"

        # HPC login nodes often expose an old system GCC even when the Python
        # environment carries the modern C++ toolchain required by chittad
        # (C++20 semaphores). Prefer that matching compiler pair when present.
        local conda_cc="$python_root/bin/x86_64-conda-linux-gnu-gcc"
        local conda_cxx="$python_root/bin/x86_64-conda-linux-gnu-g++"
        if [[ -x "$conda_cc" && -x "$conda_cxx" ]]; then
            cmake_args="$cmake_args -DCMAKE_C_COMPILER=${conda_cc} -DCMAKE_CXX_COMPILER=${conda_cxx}"
        fi
    fi
    # Pass rustup-managed cargo explicitly so cmake doesn't pick up conda/system cargo
    local _cargo_exe
    _cargo_exe=$(find_cargo 2>/dev/null) && cmake_args="$cmake_args -DCARGO_EXECUTABLE=$_cargo_exe"
    cmake_args="$cmake_args -DCHITTA_WITH_LLAMA_CPP=ON"

    # Configure - cmake .. runs from build_dir, so source is one level up ($src_chitta)
    cmake "$src_chitta" $cmake_args 2>&1 | tail -10
    local cmake_rc=${PIPESTATUS[0]}
    if [[ $cmake_rc -ne 0 ]]; then
        echo "[cc-soul] ERROR: cmake configuration failed" >&2
        return 1
    fi

    # Build only the production binaries. Some optional helpers/tests have
    # additional host-runtime requirements and must not block installation.
    local nproc_val=$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)
    cmake --build . --target chitta_tool chittad --parallel "$nproc_val" 2>&1 | tail -10
    local make_rc=${PIPESTATUS[0]}
    if [[ $make_rc -ne 0 ]]; then
        echo "[cc-soul] ERROR: build failed" >&2
        return 1
    fi

    # Copy binaries from src bin to install location (~/.claude/bin)
    # Only chitta and chittad are required; migrate/import are legacy optional tools
    # Skip cp when BIN_DIR already symlinks to the same binary (dev install)
    local all_built=true
    for bin in chitta chittad; do
        if [[ -x "$plugin_bin/$bin" ]]; then
            local src_real dst_real
            src_real=$(readlink -f "$plugin_bin/$bin" 2>/dev/null || echo "")
            dst_real=$(readlink -f "$BIN_DIR/$bin" 2>/dev/null || echo "")
            if [[ -L "$BIN_DIR/$bin" && "$src_real" == "$dst_real" && -n "$src_real" ]]; then
                true  # symlink already points to the built binary
            else
                install -m 0755 "$plugin_bin/$bin" "$BIN_DIR/$bin"
            fi
        else
            echo "[cc-soul] ERROR: $bin not built" >&2
            all_built=false
        fi
    done

    # Install hint_enricher.py so daemon can discover it at runtime
    local enricher_src="$PLUGIN_DIR/chitta-mcp/enrichers/hint_enricher.py"
    if [[ -f "$enricher_src" ]]; then
        install -m 0644 "$enricher_src" "$BIN_DIR/hint_enricher.py"
    fi

    $all_built && echo "[cc-soul] Build complete"
}

# Configure bash permissions for chitta commands (global settings)
# Note: MCP server config is separate - use /cc-soul-mcp command
configure_permissions() {
    local settings_file="${HOME}/.claude/settings.json"

    # Permissions to add (global - applies to all projects)
    local perms=(
        'Bash(~/.claude/bin/chitta:*)'
        'Bash(~/.claude/bin/chittad:*)'
        'Bash(chitta:*)'
        'Bash(chittad:*)'
        'Bash(systemctl --user * chittad:*)'
    )

    # Always use global settings.json
    mkdir -p "${HOME}/.claude"

    # If no settings file exists, create minimal one
    if [[ ! -f "$settings_file" ]]; then
        echo '{}' > "$settings_file"
    fi

    # Check if jq is available
    if ! command -v jq &> /dev/null; then
        echo "[cc-soul] jq not found, skipping permission config" >&2
        return 0
    fi

    # Read current settings
    local current
    current=$(cat "$settings_file")

    # Ensure permissions.allow exists
    if ! echo "$current" | jq -e '.permissions.allow' &>/dev/null; then
        current=$(echo "$current" | jq '.permissions = {"allow": []}')
    fi

    # Add each permission if not already present
    local updated="$current"
    local added=0
    for perm in "${perms[@]}"; do
        if ! echo "$updated" | jq -e --arg p "$perm" '.permissions.allow | index($p)' &>/dev/null; then
            updated=$(echo "$updated" | jq --arg p "$perm" '.permissions.allow += [$p]')
            ((added++)) || true
        fi
    done

    # Disable Claude Code's built-in auto-memory (chitta owns recall)
    if ! echo "$updated" | jq -e '.env.CLAUDE_CODE_DISABLE_AUTO_MEMORY' &>/dev/null; then
        updated=$(echo "$updated" | jq '.env.CLAUDE_CODE_DISABLE_AUTO_MEMORY = "1"')
        ((added++)) || true
        echo "[cc-soul] Disabled Claude Code auto-memory (chitta handles recall)"
    fi

    # Write back if changed
    if [[ $added -gt 0 ]]; then
        echo "$updated" | jq '.' > "$settings_file"
        echo "[cc-soul] Added $added bash permissions for chitta (global)"
    fi
}

# Create directories (symlinks no longer needed - mind is at ~/.claude/mind)
create_directories() {
    mkdir -p "${HOME}/.claude/mind"
    mkdir -p "${HOME}/.claude/bin"
    mkdir -p "${HOME}/.claude/hooks"
}

link_user_binaries() {
    local user_bin="${HOME}/.local/bin"
    mkdir -p "$user_bin"

    for bin in chitta chittad; do
        if [[ -x "$BIN_DIR/$bin" ]]; then
            ln -sf "$BIN_DIR/$bin" "$user_bin/$bin"
            echo "[cc-soul] Linked $bin → $user_bin/$bin"
        fi
    done
}

# Install hooks
install_hooks() {
    local hooks_src="$PLUGIN_DIR/hooks"
    local hooks_dst="${HOME}/.claude/hooks"
    local hooks_manifest="$hooks_src/install-manifest.txt"

    mkdir -p "$hooks_dst"

    [[ -f "$hooks_manifest" ]] || {
        echo "[cc-soul] Missing canonical hook manifest: $hooks_manifest" >&2
        return 1
    }
    while IFS= read -r script; do
        [[ -z "$script" || "$script" == \#* ]] && continue
        if [[ -f "$hooks_src/$script" ]]; then
            chmod +x "$hooks_src/$script"
            if [[ ! -f "$hooks_dst/$script" ]] || \
               ! cmp -s "$hooks_src/$script" "$hooks_dst/$script"; then
                local stage="$hooks_dst/.${script}.cc-soul-install"
                install -m 0755 "$hooks_src/$script" "$stage"
                mv -f "$stage" "$hooks_dst/$script"
                echo "[cc-soul] Installed $script"
            fi
        fi
    done < "$hooks_manifest"
}

# Install CLAUDE.md as user-level rule
install_claude_rules() {
    local rules_dir="${HOME}/.claude/rules"
    local source="$PLUGIN_DIR/CLAUDE.md"
    local target="$rules_dir/cc-soul.md"

    [[ ! -f "$source" ]] && return 0

    mkdir -p "$rules_dir"
    ln -sf "$source" "$target"
    echo "[cc-soul] Linked CLAUDE.md → ~/.claude/rules/cc-soul.md"
}

# Install Python packages (MCP server and TUI)
install_python_packages() {
    # Skip if Python not available
    if ! command -v python3 &> /dev/null && ! command -v python &> /dev/null; then
        return 0
    fi

    local python_cmd=""
    # Prefer the interpreter behind an existing chitta-mcp entrypoint.  On HPC
    # hosts PATH may put an old PyPy/conda `python3` first even though the working
    # MCP install uses a newer CPython; selecting PATH blindly then makes the
    # requires-python check fail and the quiet pip command leaves MCP stale.
    if command -v chitta-mcp &>/dev/null; then
        local existing_entry existing_python
        existing_entry=$(command -v chitta-mcp)
        existing_python=$(head -n 1 "$existing_entry" 2>/dev/null | sed 's/^#!//')
        if [[ -x "$existing_python" ]] && \
           "$existing_python" -c 'import sys; raise SystemExit(sys.version_info < (3, 10))' 2>/dev/null; then
            python_cmd="$existing_python"
        fi
    fi
    if [[ -z "$python_cmd" ]]; then
        local candidate
        for candidate in "$(command -v python3 2>/dev/null || true)" \
                         "$(command -v python 2>/dev/null || true)" /usr/bin/python3; do
            [[ -x "$candidate" ]] || continue
            if "$candidate" -c 'import sys; raise SystemExit(sys.version_info < (3, 10))' 2>/dev/null; then
                python_cmd="$candidate"
                break
            fi
        done
    fi
    [[ -n "$python_cmd" ]] || { echo "[cc-soul] Python >=3.10 not found; skipping Python packages" >&2; return 0; }

    # Install chitta-mcp (MCP server)
    local mcp_dir="$PLUGIN_DIR/chitta-mcp"
    if [[ -d "$mcp_dir" ]]; then
        local reinstall=false
        if ! command -v chitta-mcp &> /dev/null; then
            reinstall=true
        else
            # Reinstall if version changed
            local installed_ver
            installed_ver=$($python_cmd -c "import importlib.metadata; print(importlib.metadata.version('chitta-mcp'))" 2>/dev/null || echo "")
            local pkg_ver
            pkg_ver=$(grep '^version' "$mcp_dir/pyproject.toml" 2>/dev/null | head -1 | grep -oP '[\d.]+' || echo "")
            [[ -n "$pkg_ver" && "$installed_ver" != "$pkg_ver" ]] && reinstall=true
        fi
        if [[ "$reinstall" == "true" ]]; then
            if $python_cmd -m pip install -q -e "$mcp_dir" --user 2>/dev/null; then
                echo "[cc-soul] Installed chitta-mcp"
            fi
        fi
        # Symlink chitta-mcp to ~/.local/bin so it's on PATH for Codex and other tools
        local chitta_mcp_bin
        chitta_mcp_bin=$(command -v chitta-mcp 2>/dev/null || true)
        if [[ -n "$chitta_mcp_bin" && "$chitta_mcp_bin" != "${HOME}/.local/bin/chitta-mcp" ]]; then
            mkdir -p "${HOME}/.local/bin"
            ln -sf "$chitta_mcp_bin" "${HOME}/.local/bin/chitta-mcp"
        fi
        # Always update Claude Code MCP server config to use entrypoint (version-independent)
        if [[ -f "$PLUGIN_DIR/scripts/configure-mcp.sh" ]]; then
            bash "$PLUGIN_DIR/scripts/configure-mcp.sh" 2>/dev/null || true
        fi
    fi

    # Install sadhana-tui (optional Python TUI)
    local tui_dir="$PLUGIN_DIR/sadhana-tui"
    if [[ -d "$tui_dir" ]]; then
        if ! command -v sadhana-tui &> /dev/null; then
            if $python_cmd -m pip install -q -e "$tui_dir" --user 2>/dev/null; then
                echo "[cc-soul] Installed sadhana-tui"
            fi
        fi
    fi

    # Symlink cec-status to BIN_DIR so it's on PATH alongside chitta
    local cec_status_src="$PLUGIN_DIR/scripts/cec-status.sh"
    if [[ -f "$cec_status_src" ]]; then
        chmod +x "$cec_status_src"
        ln -sf "$cec_status_src" "$BIN_DIR/cec-status"
        echo "[cc-soul] Linked cec-status → $BIN_DIR/cec-status"
    fi


}

# Configure hooks in settings.json
configure_hooks() {
    local settings_file="${HOME}/.claude/settings.json"

    # Needs jq for JSON manipulation
    if ! command -v jq &> /dev/null; then
        echo "[cc-soul] jq not found, skipping hook config" >&2
        return 0
    fi

    # Skip if settings file doesn't exist
    [[ ! -f "$settings_file" ]] && return 0

    # When the plugin is enabled, its hooks.json is authoritative. Remove
    # legacy user-level cc-soul hook entries instead of merely skipping new
    # additions; otherwise Claude executes both copies with different timeouts.
    if jq -e '(if (.enabledPlugins // {} | has("chitta@genomewalker-chitta")) then .enabledPlugins["chitta@genomewalker-chitta"] else .enabledPlugins["cc-soul@genomewalker-cc-soul"] end) == true' "$settings_file" &>/dev/null; then
        local cleanup_stage="${settings_file}.cc-soul-cleanup"
        jq --arg re "$CC_SOUL_SETTINGS_HOOK_RE" '
          def is_cc_soul_hook:
            ((.command? // "") | test($re));
          def strip_cc_soul:
            map(.hooks = ((.hooks // []) | map(select((is_cc_soul_hook | not)))))
            | map(select((.hooks | length) > 0));
          .hooks = ((.hooks // {}) | with_entries(.value |= strip_cc_soul))
          | .hooks = (.hooks | with_entries(select((.value | length) > 0)))
        ' "$settings_file" > "$cleanup_stage"
        if ! cmp -s "$settings_file" "$cleanup_stage"; then
            mv -f "$cleanup_stage" "$settings_file"
            echo "[cc-soul] Plugin enabled; removed duplicate user-level cc-soul hooks"
        else
            rm -f "$cleanup_stage"
            echo "[cc-soul] Plugin enabled; hook config already canonical"
        fi
        return 0
    fi

    local current
    current=$(cat "$settings_file")

    # Ensure hooks object exists
    if ! echo "$current" | jq -e '.hooks' &>/dev/null; then
        current=$(echo "$current" | jq '.hooks = {}')
    fi

    local updated="$current"
    local added=0
    local hooks_dir="${HOME}/.claude/hooks"

    # ============================================================
    # cc-soul hooks - memory injection and learning
    # Uses ~/.claude/hooks/ for portability (independent of plugin location)
    # ============================================================

    # SessionStart: source-specific matchers (startup/compact/resume/clear)
    if ! echo "$updated" | jq -e '.hooks.SessionStart[]? | select(.hooks[]?.command | contains("session-start-hook.sh"))' &>/dev/null; then
        local ss_startup='{"matcher":"startup","hooks":[{"type":"command","command":"~/.claude/hooks/subconscious.sh start","timeout":8,"statusMessage":"awakening…"},{"type":"command","command":"~/.claude/hooks/session-start-hook.sh","timeout":10,"statusMessage":"recalling context…"}]}'
        local ss_compact='{"matcher":"compact","hooks":[{"type":"command","command":"~/.claude/hooks/compact-restore-hook.sh","timeout":5,"statusMessage":"restoring context…"}]}'
        local ss_resume='{"matcher":"resume","hooks":[{"type":"command","command":"~/.claude/hooks/subconscious.sh start","timeout":8,"statusMessage":"awakening…"},{"type":"command","command":"~/.claude/hooks/session-start-hook.sh","timeout":10,"statusMessage":"resuming…"},{"type":"command","command":"~/.claude/hooks/resume-inject-hook.sh","timeout":3,"statusMessage":"preparing recap…"}]}'
        local ss_clear='{"matcher":"clear","hooks":[{"type":"command","command":"~/.claude/hooks/subconscious.sh start","timeout":8,"statusMessage":"awakening…"},{"type":"command","command":"~/.claude/hooks/session-start-hook.sh","timeout":10,"statusMessage":"recalling context…"}]}'
        updated=$(echo "$updated" | jq --argjson s "$ss_startup" --argjson c "$ss_compact" --argjson r "$ss_resume" --argjson cl "$ss_clear" \
            '.hooks.SessionStart = ((.hooks.SessionStart // []) + [$s, $c, $r, $cl])')
        ((added++)) || true
    fi

    # FileChanged: auto-index project files when watched files change
    if ! echo "$updated" | jq -e '.hooks.FileChanged[]? | select(.hooks[]?.command | contains("file-changed-hook.sh"))' &>/dev/null; then
        local fc_hook='{"matcher":"*","hooks":[{"type":"command","command":"~/.claude/hooks/file-changed-hook.sh","timeout":15,"statusMessage":"re-indexing…"}]}'
        updated=$(echo "$updated" | jq --argjson hook "$fc_hook" '.hooks.FileChanged = ((.hooks.FileChanged // []) + [$hook])')
        ((added++)) || true
    fi

    # SubagentStop: capture team/agent learnings
    if ! echo "$updated" | jq -e '.hooks.SubagentStop[]? | select(.hooks[]?.command | contains("subagent-stop-hook.sh"))' &>/dev/null; then
        local sa_hook='{"matcher":"*","hooks":[{"type":"command","command":"~/.claude/hooks/subagent-stop-hook.sh","timeout":5,"statusMessage":"capturing learnings…","async":true}]}'
        updated=$(echo "$updated" | jq --argjson hook "$sa_hook" '.hooks.SubagentStop = ((.hooks.SubagentStop // []) + [$hook])')
        ((added++)) || true
    fi

    # UserPromptSubmit: memory resonance on each message
    if ! echo "$updated" | jq -e '.hooks.UserPromptSubmit[]? | select(.hooks[]?.command | contains("prompt-hook.sh"))' &>/dev/null; then
        local prompt_hook='{"matcher":"*","hooks":[{"type":"command","command":"~/.claude/hooks/prompt-hook.sh","timeout":10,"statusMessage":"resonating…"}]}'
        updated=$(echo "$updated" | jq --argjson hook "$prompt_hook" '.hooks.UserPromptSubmit = ((.hooks.UserPromptSubmit // []) + [$hook])')
        ((added++)) || true
    fi

    # Stop: preserve state and auto-learn
    if ! echo "$updated" | jq -e '.hooks.Stop[]? | select(.hooks[]?.command | contains("stop-hook.sh"))' &>/dev/null; then
        local stop_hook='{"matcher":"*","hooks":[{"type":"command","command":"~/.claude/hooks/stop-hook.sh","timeout":30,"statusMessage":"preserving state…"}]}'
        updated=$(echo "$updated" | jq --argjson hook "$stop_hook" '.hooks.Stop = ((.hooks.Stop // []) + [$hook])')
        ((added++)) || true
    fi

    # SessionEnd: close the durable session and release any thread lease.
    if ! echo "$updated" | jq -e '.hooks.SessionEnd[]? | select(.hooks[]?.command | contains("session-end-hook.sh"))' &>/dev/null; then
        local session_end_hook='{"matcher":"*","hooks":[{"type":"command","command":"~/.claude/hooks/session-end-hook.sh","timeout":5,"statusMessage":"releasing session…"}]}'
        updated=$(echo "$updated" | jq --argjson hook "$session_end_hook" '.hooks.SessionEnd = ((.hooks.SessionEnd // []) + [$hook])')
        ((added++)) || true
    fi

    # PreCompact: checkpoint before context loss
    if ! echo "$updated" | jq -e '.hooks.PreCompact[]? | select(.hooks[]?.command | contains("pre-compact-hook.sh"))' &>/dev/null; then
        local precompact_hook='{"matcher":"*","hooks":[{"type":"command","command":"~/.claude/hooks/pre-compact-hook.sh","statusMessage":"consolidating memory…"}]}'
        updated=$(echo "$updated" | jq --argjson hook "$precompact_hook" '.hooks.PreCompact = ((.hooks.PreCompact // []) + [$hook])')
        ((added++)) || true
    fi

    # PreToolUse (Read|Edit|Write): inject file context
    if ! echo "$updated" | jq -e '.hooks.PreToolUse[]? | select(.hooks[]?.command | contains("pre-tool-hook.sh"))' &>/dev/null; then
        local pretool_hook='{"matcher":"Read|Edit|Write","hooks":[{"type":"command","command":"~/.claude/hooks/pre-tool-hook.sh","timeout":5,"statusMessage":"gathering context…"}]}'
        updated=$(echo "$updated" | jq --argjson hook "$pretool_hook" '.hooks.PreToolUse = ((.hooks.PreToolUse // []) + [$hook])')
        ((added++)) || true
    fi

    # ============================================================
    # Bash history hook (existing functionality)
    # ============================================================
    if ! echo "$updated" | jq -e '.hooks.PostToolUse[]? | select(.matcher == "Bash") | .hooks[]? | select(.command | contains("log-bash-history"))' &>/dev/null; then
        local bash_hook='{"matcher":"Bash","hooks":[{"type":"command","command":"~/.claude/hooks/log-bash-history.sh \"$CLAUDE_TOOL_INPUT_command\" >/dev/null 2>&1","timeout":5,"statusMessage":"logging command…"}]}'
        updated=$(echo "$updated" | jq --argjson hook "$bash_hook" '.hooks.PostToolUse = ((.hooks.PostToolUse // []) + [$hook])')
        ((added++)) || true
    fi

    # Write back if changed
    if [[ $added -gt 0 ]]; then
        echo "$updated" | jq '.' > "$settings_file"
        echo "[cc-soul] Configured $added hooks in settings.json"
    fi
}

# Stop any running daemon (systemd first, then fallback to signals)
stop_daemon() {
    # Use systemd if the service is active
    if command -v systemctl &>/dev/null && systemctl --user is-active chittad &>/dev/null 2>&1; then
        echo "[cc-soul] Stopping daemon via systemd..."
        systemctl --user stop chittad
        return 0
    fi

    # Try graceful shutdown via chittad CLI
    if [[ -x "$BIN_DIR/chittad" ]]; then
        if "$BIN_DIR/chittad" shutdown 2>/dev/null; then
            for i in $(seq 1 20); do
                pgrep -f "chittad daemon" >/dev/null 2>&1 || break
                sleep 0.5
            done
            return 0
        fi
    fi

    # Fallback: signal daemon directly
    local daemon_pid
    daemon_pid=$(pgrep -f "chittad daemon" 2>/dev/null || true)
    if [[ -n "$daemon_pid" ]]; then
        echo "[cc-soul] Stopping daemon (pid $daemon_pid)..."
        kill -TERM "$daemon_pid" 2>/dev/null || true
        for i in $(seq 1 20); do
            kill -0 "$daemon_pid" 2>/dev/null || break
            sleep 0.5
        done
        kill -0 "$daemon_pid" 2>/dev/null && kill -9 "$daemon_pid" 2>/dev/null || true
    fi

    # Clean up stale files
    rm -f /tmp/chitta-*.sock /tmp/chitta-*.lock /tmp/chitta-*.pid 2>/dev/null || true
}

# Write only configuration; kept separate so tests never invoke service management.
write_primary_node_config() {
    local service_dir="$1"
    local primary_node="${CHITTA_DAEMON_NODE:-$(hostname -s)}"
    [[ "$primary_node" =~ ^[a-zA-Z0-9][a-zA-Z0-9._-]*$ ]] || {
        echo "[cc-soul] Invalid CHITTA_DAEMON_NODE: $primary_node" >&2
        return 1
    }
    mkdir -p "$MIND_PATH" "$service_dir/chittad.service.d" || return 1
    printf '%s\n' "$primary_node" > "$MIND_PATH/.daemon-node" || return 1
    cat > "$service_dir/chittad.service.d/primary-node.conf" <<EOF
[Service]
# One daemon per NFS store: chittad itself exits 75 when <mind>/.daemon-node
# names another host (checked in the main process, so this setting applies;
# an ExecStartPre exit is not covered by it, probed 2026-09-16).
RestartPreventExitStatus=75
EOF
}

# Install and enable the systemd user service for chittad (Linux only)
setup_systemd_service() {
    # Only Linux with systemd user session
    [[ "$(uname -s)" != "Linux" ]] && return 0
    command -v systemctl &>/dev/null || return 0
    systemctl --user status &>/dev/null 2>&1 || return 0

    local service_dir="${HOME}/.config/systemd/user"
    local service_file="$service_dir/chittad.service"

    mkdir -p "$service_dir"

    # Preserve --embed-model from existing service if present
    local embed_flag=""
    if [[ -f "$service_file" ]]; then
        local existing_embed
        existing_embed=$(grep -oP '(?<=--embed-model )\S+' "$service_file" 2>/dev/null || true)
        [[ -n "$existing_embed" ]] && embed_flag=" --embed-model $existing_embed"
    fi

    # Write service file (always update to pick up path changes)
    cat > "$service_file" << EOF
[Unit]
Description=chittad — cc-soul memory daemon
After=default.target

[Service]
Type=simple
Environment="PATH=$HOME/.bun/bin:$HOME/.local/bin:$HOME/.claude/bin:/usr/local/bin:/usr/bin:/bin"
# OpenBLAS defaults to nproc spin-wait threads; the daemon's matmuls are small
# (768-dim), so uncapped BLAS turns an embed backlog into a node-wide load
# convoy that starves the RPC pool (observed 2026-07-07, load ~70).
Environment=OPENBLAS_NUM_THREADS=4
Environment=OMP_NUM_THREADS=4
ExecStart=$BIN_DIR/chittad daemon --path $MIND_PATH --foreground --no-autonomous --distill-interval 60 --no-enrich${embed_flag}
Restart=always
RestartSec=10
KillMode=mixed
TimeoutStartSec=120
# Shutdown saves two ~900 MB snapshot files to NFS. A 30 s SIGKILL mid-save
# left a server-side lock on the store dir and 49 restarts failed (2026-09-15).
TimeoutStopSec=300
StandardOutput=journal
StandardError=journal

[Install]
WantedBy=default.target
EOF

    write_primary_node_config "$service_dir" || return 1

    systemctl --user daemon-reload
    systemctl --user enable chittad 2>/dev/null || true
    systemctl --user reset-failed chittad 2>/dev/null || true
    systemctl --user restart chittad 2>/dev/null || true
    echo "[cc-soul] systemd user service installed and enabled (chittad.service)"
}

validate_binaries() {
    if [[ ! -x "$BIN_DIR/chittad" ]]; then
        echo "[cc-soul] ERROR: chittad not found after install" >&2
        return 1
    fi
    if [[ ! -x "$BIN_DIR/chitta" ]]; then
        echo "[cc-soul] ERROR: chitta not found after install" >&2
        return 1
    fi

    local cli_help
    cli_help=$("$BIN_DIR/chitta" --help 2>&1 || true)
    if [[ -z "$cli_help" ]]; then
        echo "[cc-soul] ERROR: Unable to query chitta help" >&2
        return 1
    fi
    if ! echo "$cli_help" | grep -q -- "--socket-path"; then
        echo "[cc-soul] WARNING: chitta missing --socket-path support" >&2
    fi
    return 0
}

# Main
main() {
    # Plugin's own version (what's in the cached plugin directory)
    local plugin_version
    if command -v jq &>/dev/null; then
        plugin_version=$(jq -r '.version // "0.0.0"' "$PLUGIN_DIR/.claude-plugin/plugin.json" 2>/dev/null || echo "0.0.0")
    else
        plugin_version=$(grep '"version"' "$PLUGIN_DIR/.claude-plugin/plugin.json" 2>/dev/null | head -1 | sed 's/.*"version"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/' || echo "0.0.0")
    fi

    # Try to fetch the latest release from GitHub — always install the latest, not the cached plugin version
    local current_version="$plugin_version"
    local use_downloaded_source=false
    local github_version
    if github_version=$(fetch_latest_version 2>/dev/null) && [[ -n "$github_version" ]]; then
        if ! semver_gte "$plugin_version" "$github_version"; then
            echo "[cc-soul] Latest release v$github_version is newer than plugin cache v$plugin_version"
            current_version="$github_version"
            use_downloaded_source=true
        fi
    fi

    local installed_version=$(cat "$MARKER" 2>/dev/null || echo "")
    local installed_binary_version=""
    if [[ -x "$BIN_DIR/chitta" ]]; then
        installed_binary_version=$("$BIN_DIR/chitta" --version 2>/dev/null | awk '{print $NF}' | head -1)
    fi

    if [[ "$current_version" == "$installed_version" && \
          "$current_version" == "$installed_binary_version" && -x "$BIN_DIR/chitta" ]]; then
        echo "[cc-soul] Already at v$current_version"
        exit 0
    fi

    echo "[cc-soul] Installing v$current_version..."

    # Install binaries (try pre-built first, then build from source with llama.cpp)
    # NOTE: binaries are installed atomically (install cmd) so the running daemon
    # keeps its old inode in memory — stop_daemon happens AFTER install.
    local platform=$(detect_platform)
    local need_binaries=false

    if [[ ! -x "$BIN_DIR/chitta" || "$current_version" != "$installed_version" ]]; then
        need_binaries=true
    fi

    if $need_binaries; then
        # Determine source root: download latest if needed, otherwise use plugin cache
        local src_root="$PLUGIN_DIR"
        if $use_downloaded_source; then
            if download_source_tarball "$current_version"; then
                src_root="$SOURCE_BUILD_DIR"
            else
                echo "[cc-soul] WARNING: Could not download source v$current_version, falling back to plugin cache v$plugin_version"
                current_version="$plugin_version"
            fi
        fi

        if has_cxx_compiler; then
            echo "[cc-soul] Building from source with llama.cpp embeddings..."
            build_from_source "$src_root" || {
                if [[ -n "${CHITTA_EMBED_DIM:-}" && "${CHITTA_EMBED_DIM}" != "1024" ]]; then
                    echo "[cc-soul] ERROR: Source build failed; refusing public 1024-d binaries for a ${CHITTA_EMBED_DIM}-d store" >&2
                    exit 1
                fi
                echo "[cc-soul] WARNING: Source build failed, trying pre-built binaries" >&2
                download_binaries "$current_version" "$platform" || {
                    echo "[cc-soul] ERROR: Build failed and no compatible pre-built binaries were found" >&2
                    exit 1
                }
            }
        else
            echo "[cc-soul] No C++ compiler detected — installing pre-built binaries"
            download_binaries "$current_version" "$platform" || {
                echo "[cc-soul] ERROR: No C++ compiler detected and no compatible pre-built binaries were found" >&2
                exit 1
            }
        fi
    fi

    # Create directories
    create_directories

    # Expose the backend CLIs on the standard user PATH for Codex-side installers.
    link_user_binaries

    # Install hooks
    install_hooks

    # Link CLAUDE.md to user rules
    install_claude_rules

    # Configure bash permissions for chitta commands
    configure_permissions

    # Configure hooks in settings.json
    configure_hooks

    # Keep user hooks, the active Claude cache, Codex hook wiring, and mirrored
    # recap/resume skills on the same revision as this installer.
    if [[ -x "$PLUGIN_DIR/scripts/sync-installed-hooks.sh" ]]; then
        "$PLUGIN_DIR/scripts/sync-installed-hooks.sh"
    fi

    # Install Python packages (MCP server, TUI)
    download_embed_model "$current_version"
    install_python_packages

    # Set up systemd user service (Linux only, no-op on macOS)
    setup_systemd_service

    if ! validate_binaries; then
        echo "[cc-soul] ERROR: Installation incomplete (invalid binaries)" >&2
        exit 1
    fi

    # Binaries are on disk — now safe to stop and restart daemon
    stop_daemon

    # Run database migrations if needed
    if [[ -f "${HOME}/.claude/mind/chitta.duckdb" && -x "$BIN_DIR/chittad" ]]; then
        "$BIN_DIR/chittad" upgrade --path "${HOME}/.claude/mind" 2>/dev/null | grep -E "^\[migrations\]|Already at" || true
    fi

    # setup_systemd_service starts the daemon once, but stop_daemon above stops it
    # again so migrations can run against a quiescent store.  A manual systemd
    # stop suppresses Restart=always, so explicitly bring the enabled service back
    # after migration instead of leaving both frontends without memory until the
    # next hook happens to repair it.
    if command -v systemctl &>/dev/null && systemctl --user is-enabled chittad &>/dev/null 2>&1; then
        if systemctl --user start chittad; then
            echo "[cc-soul] chittad service restarted"
        else
            echo "[cc-soul] WARNING: chittad service did not restart" >&2
        fi
    fi

    # Version change notification
    if [[ -n "$installed_version" && "$installed_version" != "$current_version" ]]; then
        echo "[cc-soul] Updated: $installed_version → $current_version"
    fi

    # Train hook intent classifier if not present (uses bundled corpus, no API calls)
    CLASSIFIER_MODEL="${MIND_PATH:-${HOME}/.claude/mind}/hook-classifier.bin"
    TRAIN_SCRIPT="$(dirname "$0")/train-hook-classifier.sh"
    if [[ ! -f "$CLASSIFIER_MODEL" && -f "$TRAIN_SCRIPT" ]]; then
        echo "[cc-soul] Hook intent classifier not found — training in background (~30s)..."
        nohup bash "$TRAIN_SCRIPT" \
            > "${TMPDIR:-/tmp}/chitta-train-classifier.log" 2>&1 &
        echo "[cc-soul] Classifier training started (PID $!). Regex fallback active until done."
        echo "[cc-soul] Log: ${TMPDIR:-/tmp}/chitta-train-classifier.log"
    fi

    # Mark as installed
    echo "$current_version" > "$MARKER"

    # Write stack-state manifest (backend portion)
    write_stack_state_backend "$current_version"

    # Auto-refresh Codex adapter if Codex is present on this machine
    propagate_to_codex "$current_version"

    echo "[cc-soul] Installation complete (v$current_version)"
    echo "[cc-soul] Shared backend is ready at ~/.claude/bin and ~/.claude/mind"
    echo "[cc-soul] For dual frontend setup use: chitta-stack install all"
    echo "[cc-soul] Inspect frontend wiring with: chitta-stack status"
}

# Write or update ~/.claude/mind/.stack-state.json (backend fields only).
# Uses python3 for atomic JSON merge; falls back to raw overwrite.
write_stack_state_backend() {
    local version="$1"
    local state_file="${MIND_PATH}/.stack-state.json"
    local timestamp
    timestamp="$(date -u +%Y-%m-%dT%H:%M:%SZ)"

    mkdir -p "$MIND_PATH" 2>/dev/null || true

    if command -v python3 &>/dev/null; then
        python3 - "$state_file" "$version" "$timestamp" <<'PYEOF' 2>/dev/null || true
import json, os, sys, tempfile
path, version, ts = sys.argv[1], sys.argv[2], sys.argv[3]
data = {}
try:
    with open(path) as f: data = json.load(f)
except Exception: pass
data["backend"] = version
data["backend_updated_at"] = ts
data.setdefault("updated_at", ts)
data["updated_at"] = ts
tmp = tempfile.NamedTemporaryFile("w", dir=os.path.dirname(path) or ".", delete=False)
json.dump(data, tmp, indent=2)
tmp.close()
os.replace(tmp.name, path)
PYEOF
    else
        printf '{"backend":"%s","backend_updated_at":"%s","updated_at":"%s"}\n' \
            "$version" "$timestamp" "$timestamp" > "$state_file"
    fi
}

# If Codex is installed on this machine, refresh its adapter cache to match backend.
propagate_to_codex() {
    local version="$1"
    local codex_cache="${HOME}/.codex/plugins/cache/local/cc-soul/local"

    [[ -d "$codex_cache" ]] || return 0

    local stack_cmd
    if command -v chitta-stack &>/dev/null; then
        stack_cmd="chitta-stack"
    elif [[ -x "${BIN_DIR%/bin}/bin/chitta-stack" ]]; then
        stack_cmd="${BIN_DIR%/bin}/bin/chitta-stack"
    elif [[ -x "${HOME}/.local/bin/chitta-stack" ]]; then
        stack_cmd="${HOME}/.local/bin/chitta-stack"
    else
        echo "[cc-soul] Codex detected but chitta-stack not on PATH — skipping adapter refresh"
        echo "[cc-soul] Run: chitta-stack install codex"
        return 0
    fi

    echo "[cc-soul] Codex detected — refreshing adapter (v$version)"
    if "$stack_cmd" install codex --skip-bridge >/dev/null 2>&1; then
        echo "[cc-soul] Codex adapter refreshed"
        # Ensure Codex hook wiring is deterministic and matches repo adapters.
        if [[ -x "$PLUGIN_DIR/scripts/configure-codex-hooks.sh" ]]; then
            "$PLUGIN_DIR/scripts/configure-codex-hooks.sh" >/dev/null 2>&1 || \
                echo "[cc-soul] WARN: Codex hooks config failed — run scripts/configure-codex-hooks.sh"
        fi
    else
        echo "[cc-soul] WARN: Codex adapter refresh failed — run: chitta-stack install codex"
    fi
}

main "$@"
