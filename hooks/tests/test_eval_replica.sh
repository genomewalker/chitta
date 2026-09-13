#!/usr/bin/env bash
# Unit tests for manifest-family selection and grader socket plumbing.
set -u

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
REPLICA="$ROOT/scripts/eval-replica.sh"
FAIL=0
assert() { if ! eval "$2"; then echo "FAIL: $1"; FAIL=1; else echo "ok: $1"; fi; }

T="$(mktemp -d)"
trap 'rm -rf "$T"' EXIT
mkdir -p "$T/live/chitta-field" "$T/eval"

python3 - "$T/live/chitta-field" <<'PY'
import json
import pathlib
import sys

root = pathlib.Path(sys.argv[1])
families = {}
for snapshot_id, seqno in (("aa000001", 10), ("bb000002", 20)):
    names = {
        f"chitta.{snapshot_id}.snapshot": f"snapshot-{snapshot_id}",
        f"chitta.{snapshot_id}.pld": f"payload-{snapshot_id}",
        f"chitta.{snapshot_id}.shdr": f"header-{snapshot_id}",
        f"chitta.{snapshot_id}.sup.json": "{}",
        f"cortex.{snapshot_id}.snapshot": f"cortex-{snapshot_id}",
    }
    for name, content in names.items():
        (root / name).write_text(content)
    snap = root / f"chitta.{snapshot_id}.snapshot"
    sidecars = []
    for suffix in ("pld", "shdr", "sup.json"):
        path = root / f"chitta.{snapshot_id}.{suffix}"
        sidecars.append({"name": path.name, "size_bytes": path.stat().st_size})
    families[snapshot_id] = {
        "snapshot": {"name": snap.name, "size_bytes": snap.stat().st_size},
        "sidecars": sidecars,
        "snapshot_seqno": seqno,
        "covered": {},
    }

manifest = {
    "magic": "CHITTA_FIELD_MANIFEST_V1",
    "generation": 2,
    "format_version": 1,
    "families": families,
    "checkpoints": families["aa000001"],
}
(root / "MANIFEST.1").write_text(json.dumps(manifest))
PY

selected="$(CHITTA_LIVE_MIND="$T/live" CHITTA_EVAL_MIND="$T/eval" "$REPLICA" snapshot-id)"
assert "newest consistent family is selected" "[[ '$selected' == bb000002 ]]"

touch "$T/live/chitta-field/MANIFEST.1.tmp"
if CHITTA_LIVE_MIND="$T/live" CHITTA_EVAL_MIND="$T/eval" \
    CHITTAD_BIN=/bin/true CHITTA_BIN=/bin/true \
    CHITTA_EVAL_EMBED_MODEL="$T/live/chitta-field/chitta.bb000002.snapshot" \
    "$REPLICA" start >"$T/save.out" 2>"$T/save.err"; then
    save_rc=0
else
    save_rc=$?
fi
assert "an active manifest temp file blocks the copy" "[[ $save_rc -ne 0 ]]"
assert "active-save refusal is explicit" "grep -q 'snapshot save appears active' '$T/save.err'"
rm -f "$T/live/chitta-field/MANIFEST.1.tmp"

mkdir -p "$T/bad/chitta-field" "$T/bad-eval"
python3 - "$T/bad/chitta-field" <<'PY'
import json
import pathlib
import sys

root = pathlib.Path(sys.argv[1])
snapshot = root / "chitta.bad00001.snapshot"
snapshot.write_text("snapshot")
family = {
    "snapshot": {"name": snapshot.name, "size_bytes": snapshot.stat().st_size},
    "sidecars": [{"name": "chitta.bad00001.pld", "size_bytes": 99}],
    "snapshot_seqno": 30,
    "covered": {},
}
manifest = {
    "magic": "CHITTA_FIELD_MANIFEST_V1",
    "generation": 1,
    "format_version": 1,
    "families": {"bad00001": family},
    "checkpoints": family,
}
(root / "MANIFEST.1").write_text(json.dumps(manifest))
PY
if CHITTA_LIVE_MIND="$T/bad" CHITTA_EVAL_MIND="$T/bad-eval" \
    "$REPLICA" snapshot-id >"$T/bad.out" 2>"$T/bad.err"; then
    bad_rc=0
else
    bad_rc=$?
fi
assert "inconsistent family is refused" "[[ $bad_rc -ne 0 ]]"
assert "inconsistent refusal explains the missing sidecar" "grep -q 'no consistent snapshot family.*missing chitta.bad00001.pld' '$T/bad.err'"

mkdir -p "$T/bin"
cat > "$T/bin/chitta" <<'STUB'
#!/usr/bin/env bash
printf '%s\n' "$*" >> "$STUB_LOG"
printf '%s\n' '{"results":[]}'
STUB
chmod +x "$T/bin/chitta"
: > "$T/chitta.log"
PATH="$T/bin:$PATH" STUB_LOG="$T/chitta.log" \
    CHITTA_EVAL_SOCKET="$T/eval.sock" \
    python3 - "$ROOT/hooks/grade-recall.py" <<'PY'
import importlib.util
import pathlib
import sys

path = pathlib.Path(sys.argv[1])
spec = importlib.util.spec_from_file_location("grade_recall", path)
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
module._recall_full("ping", 1, "hybrid")
PY
assert "grader appends --socket-path when CHITTA_EVAL_SOCKET is set" \
    "grep -q -- '--socket-path $T/eval.sock' '$T/chitta.log'"

exit "$FAIL"
