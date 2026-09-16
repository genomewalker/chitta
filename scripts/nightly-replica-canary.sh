#!/usr/bin/env bash
# Fresh private replica; read-only panels; never start/stop the source daemon.
# Defaults can be overridden for development without writing report artifacts to git.
set -euo pipefail
ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
PY="${CHITTA_PY:-/maps/projects/fernandezguerra/apps/opt/conda/envs/bioinfo/bin/python3}"
REPORT_ROOT="${CHITTA_CANARY_REPORT_ROOT:-/projects/caeg/scratch/kbd606/tmp/canary}"
export CHITTAD_BIN="${CHITTAD_BIN:-$ROOT/bin/chittad}"
export CHITTA_BIN="${CHITTA_BIN:-$ROOT/bin/chitta}"
export CHITTA_LIVE_MIND="${CHITTA_CANARY_SOURCE:-/projects/caeg/scratch/kbd606/tmp/chitta-eval-mind}"
export PATH="$(dirname "$PY"):$(dirname "$CHITTA_BIN"):$PATH"
mkdir -p "$REPORT_ROOT"
REPORT_DIR="$(mktemp -d "$REPORT_ROOT/$(date -u +%Y-%m-%dT%H%M%SZ).XXXXXX")"
PRIVATE="$(mktemp -d /tmp/chitta-canary.XXXXXX)"
mkdir "$REPORT_DIR/mind"
ln -s "$REPORT_DIR/mind" "$PRIVATE/m"
export CHITTA_EVAL_MIND="$PRIVATE/m"
export CHITTA_EVAL_PORT
CHITTA_EVAL_PORT="$($PY -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1",0)); print(s.getsockname()[1])')"
export HOME="$PRIVATE/home" XDG_RUNTIME_DIR="$PRIVATE/m/run"
export CHITTA_DB_PATH="$PRIVATE/m" CHITTA_QUEUE="$PRIVATE/m/queue.jsonl" CHITTA_NO_QUEUE=1
unset CHITTA_SOCKET_PATH CHITTA_HEADLESS CC_SOUL_HEADLESS MIND_PATH CHITTA_MIND
mkdir -p "$HOME/.claude/mind"
cleanup() {
    local rc=$?
    trap - EXIT
    # Metadata came exclusively from this invocation's fresh helper directory.
    "$PY" - "$CHITTA_EVAL_MIND" "$REPORT_DIR" "$rc" <<'PY'
import json
import os
from pathlib import Path
import signal
import sys
import time
mind, report, rc = Path(sys.argv[1]), Path(sys.argv[2]), int(sys.argv[3])
pidfile = mind / 'replica.pid'
if pidfile.exists():
    pid = int(pidfile.read_text())
    proc = Path('/proc') / str(pid)
    if proc.exists():
        args = (proc / 'cmdline').read_bytes().split(b'\0')
        if b'--path' in args and os.fsencode(mind) in args and b'daemon' in args:
            os.kill(pid, signal.SIGTERM)
            deadline = time.monotonic() + 15
            while proc.exists() and time.monotonic() < deadline:
                if (proc / 'stat').read_text().split()[2] == 'Z':
                    break
                time.sleep(.1)
            else:
                if proc.exists():
                    os.kill(pid, signal.SIGKILL)
summary = report / 'report.json'
if not summary.exists():
    summary.write_text(json.dumps({'status': 'ERROR', 'exit_code': rc,
                                  'reason': 'replica startup or panel execution failed; see run.log'}, indent=2)+'\n')
PY
    rm -rf "$PRIVATE"
    # Reports retain scores/logs, not another multi-GB replica every night.
    rm -rf "$REPORT_DIR/mind"
    printf 'canary report: %s/report.json\n' "$REPORT_DIR"
    exit "$rc"
}
trap cleanup EXIT
bash "$ROOT/scripts/eval-replica.sh" start > "$REPORT_DIR/run.log" 2>&1
# shellcheck disable=SC1091
source "$CHITTA_EVAL_MIND/replica.env"
export CHITTA_EVAL_SOCKET CHITTA_EVAL_SNAPSHOT_ID
export CHITTA_SOCKET_PATH="$CHITTA_EVAL_SOCKET"
"$PY" - "$ROOT" "$REPORT_DIR" >> "$REPORT_DIR/run.log" 2>&1 <<'PY'
import importlib.util
import json
import math
import os
from pathlib import Path
import statistics
import subprocess
import sys
from datetime import datetime, timezone
root, report_dir = map(Path, sys.argv[1:])
report = {'date': datetime.now(timezone.utc).isoformat(), 'status': 'ERROR',
          'snapshot_id': os.environ['CHITTA_EVAL_SNAPSHOT_ID'], 'metrics': {}, 'errors': [], 'skips': []}
try:
    noise = json.loads((root / 'benchmarks/noise.json').read_text())
    if not noise.get('acceptance_ready') or noise.get('mode') != 'replica':
        raise ValueError('noise bands are not acceptance-ready replica bands')
    if noise['snapshot_id'] != report['snapshot_id']:
        raise ValueError('snapshot differs from calibrated noise bands; recalibration required')
    if noise['golden_config'] != {'limit': 20, 'strategy': 'hybrid', 'reranker': False}:
        raise ValueError('golden configuration differs from calibrated bands')
    spec = importlib.util.spec_from_file_location('canary_noise', root / 'benchmarks/noise.py')
    panel = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(panel)
    samples, snapshot = panel.golden_runs(3)
    assert snapshot == report['snapshot_id']
    values = {'golden.ndcg': statistics.mean(samples)}
    report['golden_samples'] = samples
    truth = root / 'benchmarks/current_truth/run.py'
    if truth.exists():
        # A present panel is mandatory, and must publish scalar metrics keyed
        # exactly like noise.json. Missing calibration is an error, never a pass.
        output = report_dir / 'current-truth.json'
        subprocess.run([sys.executable, str(truth), '--socket', os.environ['CHITTA_EVAL_SOCKET'],
                        '--output', str(output)], check=True, timeout=300, stdin=subprocess.DEVNULL)
        data = json.loads(output.read_text())
        metrics = data['metrics']
        if not metrics or not all(key.startswith('current_truth.') for key in metrics):
            raise ValueError('current-truth panel lacks calibrated metric keys')
        values.update(metrics)
    else:
        report['skips'].append('current_truth: panel is not present in this revision')
    for key, value in values.items():
        band = noise['metrics'][key]['band_95']
        if not all(math.isfinite(float(v)) for v in (value, *band)):
            raise ValueError(f'{key}: non-finite score or band')
        # These panels are accuracy/ranking metrics: higher is better.
        passed = value >= band[0]
        report['metrics'][key] = {'value': value, 'band_95': band, 'pass': passed}
        if not passed:
            report['errors'].append(f'{key} below calibrated lower band')
    report['status'] = 'REGRESSION' if report['errors'] else 'PASS'
except (OSError, ValueError, KeyError, AssertionError, subprocess.SubprocessError) as exc:
    report['errors'].append(str(exc))
finally:
    (report_dir / 'report.json').write_text(json.dumps(report, indent=2)+'\n')
print(json.dumps(report))
sys.exit(0 if report['status'] == 'PASS' else 1)
PY
