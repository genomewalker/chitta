#!/usr/bin/env bash
# Install the three evolution-loop cadences as user systemd timers.
#   nightly  — gather proposals + literature cards + ranked dry-run (no implementation)
#   weekly   — one full cycle: select → bet → implement → gates → replica eval → verdict → PR
#   monthly  — rotate the held-out split, recalibrate noise bands, prune stale proposals
# Only `nightly` is enabled by default; weekly/monthly are installed disabled until
# benchmarks/noise.json exists and the human-merge review flow has run once by hand.
# Usage: scripts/install-evolve-timers.sh [--enable-weekly] [--enable-monthly] [--enable-canary]
set -euo pipefail
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
UNIT_DIR="$HOME/.config/systemd/user"
LOG_DIR="${CHITTA_DB_PATH:-$HOME/.claude/mind}/evolve"
PY=/maps/projects/fernandezguerra/apps/opt/conda/envs/bioinfo/bin/python3
mkdir -p "$UNIT_DIR" "$LOG_DIR"

unit() { # name description exec calendar
  cat > "$UNIT_DIR/chitta-evolve-$1.service" <<EOF
[Unit]
Description=chitta evolve: $2
After=chittad.service
Wants=chittad.service

[Service]
Type=oneshot
WorkingDirectory=$REPO
Environment=HOME=%h
Environment=CHITTA_PY=$PY
Environment=EVOLVE_PYTHON=$PY
Environment=PATH=%h/.local/bin:%h/.claude/bin:/usr/local/bin:/usr/bin:/bin
ExecStart=/bin/bash -lc '$3 >> $LOG_DIR/$1.log 2>&1'
EOF
  cat > "$UNIT_DIR/chitta-evolve-$1.timer" <<EOF
[Unit]
Description=chitta evolve: $2 (timer)

[Timer]
OnCalendar=$4
RandomizedDelaySec=20m
Persistent=true

[Install]
WantedBy=timers.target
EOF
}

unit nightly "gather + rank (dry run)" \
  "(cd chitta-mcp && $PY -m evolve.sota_watch --max-papers 3 </dev/null); bash scripts/evolve-cycle.sh --dry-run </dev/null" \
  "*-*-* 02:30"
unit weekly "one full cycle (human merge)" \
  "bash scripts/evolve-cycle.sh --implementer codex --model gpt-6-astra --real-eval --open-pr --max-minutes 240 </dev/null" \
  "Sun *-*-* 03:00"
unit monthly "rotate holdout, recalibrate noise, prune" \
  "$PY benchmarks/smriti/split.py rotate --n 2 </dev/null; bash scripts/eval-replica.sh start && CHITTA_EVAL_SOCKET=\$(grep -oE '/[^ =]*\\.sock' \${CHITTA_EVAL_MIND:-/projects/caeg/scratch/kbd606/tmp/chitta-eval-mind}/replica.env 2>/dev/null | head -1) bash scripts/eval-noise.sh; bash scripts/eval-replica.sh stop" \
  "*-*-01 04:00"

# Replica canary is installed disabled; it has no dependency on the live daemon.
cat > "$UNIT_DIR/chitta-replica-canary.service" <<EOF
[Unit]
Description=chitta frozen replica regression canary

[Service]
Type=oneshot
WorkingDirectory=$REPO
Environment=CHITTA_PY=$PY
ExecStart=/bin/bash $REPO/scripts/nightly-replica-canary.sh
TimeoutStartSec=15min
EOF
cat > "$UNIT_DIR/chitta-replica-canary.timer" <<'EOF'
[Unit]
Description=chitta frozen replica regression canary (opt-in)

[Timer]
OnCalendar=*-*-* 01:30
RandomizedDelaySec=20m
Persistent=true

[Install]
WantedBy=timers.target
EOF

systemctl --user daemon-reload
systemctl --user enable --now chitta-evolve-nightly.timer
[[ " $* " == *" --enable-weekly "* ]] && systemctl --user enable --now chitta-evolve-weekly.timer
[[ " $* " == *" --enable-monthly "* ]] && systemctl --user enable --now chitta-evolve-monthly.timer
[[ " $* " == *" --enable-canary "* ]] && systemctl --user enable --now chitta-replica-canary.timer
systemctl --user list-timers --all 2>/dev/null | grep -E 'chitta-evolve|chitta-replica-canary|NEXT' || true
