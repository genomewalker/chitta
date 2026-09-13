#!/bin/bash
# End-to-end SessionStart cards against a socket-backed CLI stub.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
unset CHITTA_HEADLESS CC_SOUL_HEADLESS
export HOME="$T/home" XDG_RUNTIME_DIR="$T/runtime" CHITTA_DB_PATH="$T/mind"
export CHITTA_QUEUE="$T/queue" CHITTA_TASK_LEDGER="$T/tasks.sqlite"
export CHITTA_PLUGIN_DIR="$T/plugin" CC_SOUL_PLUGIN_DIR="$T/plugin"
export CHITTA_BIN="$T/chitta" STUB_CARD_DIR="$T"
mkdir -p "$HOME/.claude/mind" "$CHITTA_DB_PATH" "$T/plugin/chitta-mcp"
cat > "$CHITTA_BIN" <<'STUB'
#!/bin/bash
case "$1" in
    realm_detect) echo project:cards ;;
    recall)
        if [[ " $* " == *' --text-only '* ]]; then
            if [[ " $* " == *' --realm '* ]]; then cat "$STUB_CARD_DIR/scoped"
            else cat "$STUB_CARD_DIR/fallback"; fi
        fi ;;
esac
exit 0
STUB
chmod +x "$CHITTA_BIN"
source "$ROOT/hooks/lib.sh"
SOCK=$(get_socket_path)
mkdir -p "$(dirname "$SOCK")"
python3 -c 'import socket,sys; s=socket.socket(socket.AF_UNIX); s.bind(sys.argv[1])' "$SOCK"
for fixture in header warning metadata episode populated fallback; do
    case "$fixture" in
        header) printf 'Found 2 results:\nFound 1 results:\n' > "$T/scoped" ;;
        warning) printf '[weak: no strong matches]\nFound 0 results:\n' > "$T/scoped" ;;
        metadata) printf 'Found 1 results:\n#7 [80%%] [wisdom]   \n' > "$T/scoped" ;;
        episode) printf 'Found 1 results:\n#7 [80%%] [episode] ignored body\n' > "$T/scoped" ;;
        populated) printf '#7 [80%%] [wisdom] useful card body\n' > "$T/scoped" ;;
        fallback) printf 'Found 0 results:\n' > "$T/scoped" ;;
    esac
    cp "$T/scoped" "$T/fallback"
    if [[ "$fixture" == fallback ]]; then
        printf '#7 [80%%] [wisdom] useful card body\n' > "$T/fallback"
    fi
    printf '{"session_id":"card-%s","source":"startup"}' "$fixture" |
        bash "$ROOT/hooks/session-start-hook.sh" > "$T/out" 2> "$T/err"
    if [[ "$fixture" == populated || "$fixture" == fallback ]]; then
        grep -q '\[recall:project:cards\]' "$T/out"
        grep -q '#7 \[80%\] \[wisdom\] useful card body' "$T/out"
    else
        ! grep -q '\[recall:' "$T/out"
    fi
    echo "ok: SessionStart $fixture"
done
