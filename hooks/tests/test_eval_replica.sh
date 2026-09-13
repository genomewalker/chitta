#!/usr/bin/env bash
# Unit tests for manifest-family selection, complete copying, and grader plumbing.
set -u

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
REPLICA="$ROOT/scripts/eval-replica.sh"
FAIL=0
assert() { if ! eval "$2"; then echo "FAIL: $1"; FAIL=1; else echo "ok: $1"; fi; }

T="$(mktemp -d)"
cleanup() {
    if [[ -f "$T/eval/replica.pid" ]]; then
        kill "$(<"$T/eval/replica.pid")" 2>/dev/null || true
    fi
    rm -rf "$T"
}
trap cleanup EXIT
mkdir -p "$T/live/chitta-field/segments" "$T/eval" "$T/bin"

python3 - "$T/live/chitta-field" <<'PY'
import json
import os
import pathlib
import struct
import sys
import time

root = pathlib.Path(sys.argv[1])
SNAP_MAGIC = 0xF01157417E000017
PLD_MAGIC = 0x504C440000000001


def family(snapshot_id, seqno):
    snapshot = root / f"chitta.{snapshot_id}.snapshot"
    snapshot.write_bytes(struct.pack("<QQ", SNAP_MAGIC, seqno) + b"snapshot-body")
    pld = root / f"chitta.{snapshot_id}.pld"
    pld.write_bytes(struct.pack("<QQ", PLD_MAGIC, 0))
    shdr = root / f"chitta.{snapshot_id}.shdr"
    shdr.write_bytes(b"optional-header")
    return {
        "snapshot": {"name": snapshot.name, "size_bytes": snapshot.stat().st_size},
        "sidecars": [
            {"name": pld.name, "size_bytes": pld.stat().st_size},
            {"name": shdr.name, "size_bytes": shdr.stat().st_size},
        ],
        "snapshot_seqno": seqno,
        "covered": {},
    }


def manifest(generation, families, checkpoint):
    return {
        "magic": "CHITTA_FIELD_MANIFEST_V1",
        "generation": generation,
        "store_uuid": "00000000-0000-4000-8000-000000000000",
        "format_version": 1,
        "embedding_model": "test-model",
        "embedding_dim": 1536,
        "next_memory_id": 1,
        "next_artifact_id": 1,
        "last_seqno": checkpoint["snapshot_seqno"],
        "clean_shutdown": False,
        "segments": [],
        "checkpoints": checkpoint,
        "families": families,
    }

committed = family("aa000001", 100)
newer_slot_family = family("cc000003", 90)
# MANIFEST.2 has the newest scalar generation, but Manifest::load joins the
# higher-seqno per-writer entry from MANIFEST.1. Both slots must be copied.
(root / "MANIFEST.1").write_text(json.dumps(manifest(40, {"writer": committed}, committed)))
(root / "MANIFEST.2").write_text(
    json.dumps(manifest(41, {"writer": newer_slot_family}, newer_slot_family))
)

# An uncommitted family is newer by mtime and must never win.
uncommitted = family("bb000002", 200)
future = time.time() + 30
for path in root.glob("chitta.bb000002.*"):
    os.utime(path, (future, future))

# Optional selected-family material omitted from the manifest must still copy.
(root / "chitta.aa000001.rsf").write_bytes(b"optional-rsf")
(root / "cortex.aa000001.snapshot").write_bytes(b"optional-cortex")
(root / "seen_offsets.aa000001.json").write_text("{}")
segment = root / "segments" / "aa000001_000000000001.seg"
segment.write_bytes(b"CFLOG003" + (1).to_bytes(8, "big") + bytes(32) + bytes(8))
PY

selected="$(CHITTA_LIVE_MIND="$T/live" CHITTA_EVAL_MIND="$T/eval" "$REPLICA" snapshot-id)"
assert "manifest-committed family wins over newest mtime" "[[ '$selected' == aa000001 ]]"

# A stale temp belonging to another family is irrelevant to the selected commit.
touch "$T/live/chitta-field/chitta.bb000002.pld.tmp"
touch -d '2 hours ago' "$T/live/chitta-field/chitta.bb000002.pld.tmp"

# A fresh temp for the selected family must block.
touch "$T/live/chitta-field/chitta.aa000001.pld.tmp"
if CHITTA_LIVE_MIND="$T/live" CHITTA_EVAL_MIND="$T/eval" \
    CHITTAD_BIN=/bin/true CHITTA_BIN=/bin/true \
    CHITTA_EVAL_EMBED_MODEL="$T/live/chitta-field/chitta.aa000001.snapshot" \
    "$REPLICA" start >"$T/selected-tmp.out" 2>"$T/selected-tmp.err"; then
    selected_tmp_rc=0
else
    selected_tmp_rc=$?
fi
assert "active temp of selected family blocks the copy" "[[ $selected_tmp_rc -ne 0 ]]"
assert "selected-family active-save refusal is explicit" \
    "grep -q 'snapshot save appears active.*chitta.aa000001.pld.tmp' '$T/selected-tmp.err'"
rm -f "$T/live/chitta-field/chitta.aa000001.pld.tmp"

# Manifest temp files remain globally relevant commit points.
touch "$T/live/chitta-field/MANIFEST.1.tmp"
if CHITTA_LIVE_MIND="$T/live" CHITTA_EVAL_MIND="$T/eval" \
    CHITTAD_BIN=/bin/true CHITTA_BIN=/bin/true \
    CHITTA_EVAL_EMBED_MODEL="$T/live/chitta-field/chitta.aa000001.snapshot" \
    "$REPLICA" start >"$T/manifest-tmp.out" 2>"$T/manifest-tmp.err"; then
    manifest_tmp_rc=0
else
    manifest_tmp_rc=$?
fi
assert "an active manifest temp file blocks the copy" "[[ $manifest_tmp_rc -ne 0 ]]"
rm -f "$T/live/chitta-field/MANIFEST.1.tmp"

# A v11+ manifest family with no recorded .pld models the observed failure:
# manifest byte-size validation passes, but daemon open would refuse it.
mkdir -p "$T/bad/chitta-field" "$T/bad-eval"
python3 - "$T/bad/chitta-field" <<'PY'
import json
import pathlib
import struct
import sys

root = pathlib.Path(sys.argv[1])
snapshot = root / "chitta.bad00001.snapshot"
snapshot.write_bytes(struct.pack("<QQ", 0xF01157417E000017, 30) + b"snapshot-body")
family = {
    "snapshot": {"name": snapshot.name, "size_bytes": snapshot.stat().st_size},
    "sidecars": [],
    "snapshot_seqno": 30,
    "covered": {},
}
manifest = {
    "magic": "CHITTA_FIELD_MANIFEST_V1",
    "generation": 7,
    "store_uuid": "00000000-0000-4000-8000-000000000000",
    "format_version": 1,
    "embedding_model": "test-model",
    "embedding_dim": 1536,
    "next_memory_id": 1,
    "next_artifact_id": 1,
    "last_seqno": 30,
    "clean_shutdown": False,
    "segments": [],
    "checkpoints": family,
    "families": {"bad00001": family},
}
(root / "MANIFEST.1").write_text(json.dumps(manifest))
PY
if CHITTA_LIVE_MIND="$T/bad" CHITTA_EVAL_MIND="$T/bad-eval" \
    "$REPLICA" snapshot-id >"$T/bad.out" 2>"$T/bad.err"; then
    bad_rc=0
else
    bad_rc=$?
fi
assert "missing required modern .pld is refused" "[[ $bad_rc -ne 0 ]]"
assert "required-sidecar refusal names the missing file" \
    "grep -q 'required sidecar chitta.bad00001.pld is missing' '$T/bad.err'"

# Stubs allow the successful-copy path to run without opening a real store.
printf '%s\n' '#!/usr/bin/env bash' \
    'exec -a chittad /bin/bash -c '\''trap "exit 0" TERM INT; while :; do sleep 1; done'\'' "$@"' \
    > "$T/bin/chittad"
printf '%s\n' '#!/usr/bin/env bash' '[[ -z "${STUB_LOG:-}" ]] || printf '\''%s\n'\'' "$*" >> "$STUB_LOG"' \
    'printf '\''%s\n'\'' '\''{"results":[]}'\''' > "$T/bin/chitta"
chmod +x "$T/bin/chittad" "$T/bin/chitta"
touch "$T/embed.gguf"

if CHITTA_LIVE_MIND="$T/live" CHITTA_EVAL_MIND="$T/eval" \
    CHITTAD_BIN="$T/bin/chittad" CHITTA_BIN="$T/bin/chitta" \
    CHITTA_EVAL_EMBED_MODEL="$T/embed.gguf" CHITTA_EVAL_START_TIMEOUT=10 \
    "$REPLICA" start >"$T/start.out" 2>"$T/start.err"; then
    start_rc=0
else
    start_rc=$?
fi
assert "stale foreign temp is ignored and replica starts" "[[ $start_rc -eq 0 ]]"
assert "staged family is verified before daemon launch" \
    "grep -q 'verified snapshot=aa000001 manifest_generation=41 before daemon start' '$T/start.out'"
assert "both manifest slots are copied" \
    "[[ -f '$T/eval/chitta-field/MANIFEST.1' && -f '$T/eval/chitta-field/MANIFEST.2' ]]"
assert "recorded and unrecorded optional family files are copied" \
    "[[ -f '$T/eval/chitta-field/chitta.aa000001.shdr' && -f '$T/eval/chitta-field/chitta.aa000001.rsf' ]]"
assert "cortex and seen-offset files are copied when present" \
    "[[ -f '$T/eval/chitta-field/cortex.aa000001.snapshot' && -f '$T/eval/chitta-field/seen_offsets.aa000001.json' ]]"
assert "canonical WAL segments are copied when present" \
    "[[ -f '$T/eval/chitta-field/segments/aa000001_000000000001.seg' ]]"
assert "uncommitted family is not copied" "[[ ! -e '$T/eval/chitta-field/chitta.bb000002.snapshot' ]]"

status_out="$(CHITTA_EVAL_MIND="$T/eval" CHITTA_BIN="$T/bin/chitta" "$REPLICA" status)"
assert "status reports committed snapshot id" "grep -q 'snapshot=aa000001' <<< '$status_out'"
assert "status reports serving manifest generation" "grep -q 'manifest_generation=41' <<< '$status_out'"

CHITTA_EVAL_SOCKET="$T/eval.sock" CHITTA_EVAL_MIND="$T/eval" \
    PATH="$T/bin:$PATH" STUB_LOG="$T/chitta.log" \
    python3 - "$ROOT/hooks/grade-recall.py" <<'PY'
import importlib.util
import pathlib
import sys

path = pathlib.Path(sys.argv[1])
spec = importlib.util.spec_from_file_location("grade_recall", path)
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
assert module._replica_snapshot_id() == "aa000001"
module._recall_full("ping", 1, "hybrid")
PY
assert "grader appends --socket-path when CHITTA_EVAL_SOCKET is set" \
    "grep -q -- '--socket-path $T/eval.sock' '$T/chitta.log'"

if CHITTA_EVAL_MIND="$T/eval" "$REPLICA" stop >"$T/stop.out" 2>"$T/stop.err"; then
    stop_rc=0
else
    stop_rc=$?
fi
assert "stub replica stops cleanly" "[[ $stop_rc -eq 0 ]]"

exit "$FAIL"
