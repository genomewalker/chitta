"""Copy a validated eval family; benchmark only a private child daemon."""
import argparse
import hashlib
import json
import math
import os
import re
import socket
import statistics
import subprocess
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / "benchmarks/field-perf"


def selection(source):
    return subprocess.check_output([
        "python3", str(ROOT / "scripts/eval-replica-select.py"), str(source)
    ], text=True)


def fingerprint(source):
    return hashlib.sha256(b"".join(
        p.name.encode() + p.read_bytes() for p in
        (source / "MANIFEST.1", source / "MANIFEST.2") if p.exists()
    )).hexdigest()


def copy_family(source, dest):
    before = fingerprint(source)
    selected = selection(source)
    # Same family/manifest/WAL and marker rules as scripts/eval-replica.sh.
    for p in source.glob("*.tmp"):
        if time.time() - p.stat().st_mtime < 600:
            raise RuntimeError("active temporary file in replica: " + p.name)
    for line in selected.splitlines():
        kind, name, *rest = line.split("\t")
        if kind not in ("FILE", "MANIFEST", "WAL"):
            continue
        rel = Path("segments") / name if kind == "WAL" else Path(name)
        target = dest / rel
        target.parent.mkdir(parents=True, exist_ok=True)
        subprocess.run(["cp", "--reflink=auto", "--preserve=mode,timestamps",
                        str(source / rel), str(target)], check=True)
        assert target.stat().st_size == (source / rel).stat().st_size == int(rest[0])
    for name in ("embed_1536_v1.migrated", "ssl_gloss_v1.migrated", "lite_encoder.bin"):
        if (source / name).exists():
            subprocess.run(["cp", "--preserve=mode,timestamps", str(source / name),
                            str(dest / name)], check=True)
    assert fingerprint(source) == before and selection(source) == selected
    assert selection(dest) == selected, "copied family failed validation"
    return selected


def rpc(sock, name, args, timeout=180):
    start = time.monotonic()
    with socket.socket(socket.AF_UNIX) as s:
        s.settimeout(timeout)
        s.connect(str(sock))
        s.sendall((json.dumps({"jsonrpc": "2.0", "id": 1, "method": "tools/call",
                              "params": {"name": name, "arguments": args}}) + "\n").encode())
        with s.makefile("rb") as f:
            result = json.loads(f.readline())
    return (time.monotonic() - start) * 1000, result


def rss(pid):
    text = Path(f"/proc/{pid}/status").read_text()
    return {key: int(re.search(r"^" + key + r":\s+(\d+)", text, re.M)[1])
            for key in ("VmRSS", "VmHWM", "Threads")}


def shape(value):
    if isinstance(value, dict):
        return {k: shape(v) for k, v in sorted(value.items())}
    if isinstance(value, list):
        return [shape(value[0])] if value else []
    return type(value).__name__


def ids(value):
    if isinstance(value, dict):
        found = [str(v) for k, v in value.items() if k in ("memory_id", "id")]
        return found + [i for k, v in value.items() if k not in ("memory_id", "id") for i in ids(v)]
    if isinstance(value, list):
        return [i for v in value for i in ids(v)]
    return []


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("label")
    parser.add_argument("--source", type=Path, default=Path("/projects/caeg/scratch/kbd606/tmp/chitta-eval-mind/chitta-field"))
    parser.add_argument("--binary", type=Path, default=ROOT / "bin/chittad")
    parser.add_argument("--runs", type=int, default=20)
    parser.add_argument("--query", default="chitta recall performance lock contention")
    parser.add_argument("--realm", default="project:cc-soul")
    parser.add_argument("--port", type=int, default=17439)
    parser.add_argument("--profile", action="store_true")
    parser.add_argument("--checkpoint", action="store_true", help="save a clean snapshot in this scratch copy after measuring")
    args = parser.parse_args()
    if not re.fullmatch(r"[a-zA-Z0-9_-]+", args.label):
        parser.error("label must be a filename-safe token")
    mind = Path(tempfile.mkdtemp(prefix="field-perf-", dir="/tmp"))
    field = mind / "chitta-field"
    field.mkdir()
    selected = copy_family(args.source.resolve(), field)
    (mind / ".quiesce").touch()
    runtime = mind / "run"
    (runtime / "chitta").mkdir(parents=True)
    h = 5381
    for b in str(mind).encode():
        h = (h * 33 + b) & 0xffffffff
    sock = runtime / "chitta" / f"chitta-{h}.sock"
    env = os.environ.copy()
    env.update(XDG_RUNTIME_DIR=str(runtime), CHITTA_RPC_PORT=str(args.port),
               CHITTA_NO_QUEUE="1", CHITTA_HINT_ENRICHER="/bin/true",
               CHITTA_RECALL_PROFILE="1" if args.profile else "0")
    for key in ("CHITTA_HEADLESS", "CC_SOUL_HEADLESS", "CHITTA_SOCKET_PATH", "CHITTA_DB_PATH"):
        env.pop(key, None)
    log_path = OUT / f"daemon-{args.label}.log"
    result = {"label": args.label, "family": selected, "scratch": str(mind),
              "query": args.query, "realm": args.realm, "runs": args.runs,
              "loadavg": os.getloadavg(), "binary": str(args.binary), "binary_sha256": hashlib.sha256(args.binary.read_bytes()).hexdigest(), "methods": {}}
    command = [str(args.binary.resolve()), "daemon", "--path", str(mind), "--foreground",
               "--no-autonomous", "--no-distill", "--no-enrich", "--no-hygiene",
               "--no-embed-interval", "--embed-model",
               "/maps/projects/caeg/people/kbd606/models/nomic-embed-text-v1.5.gguf",
               "--rpc-port", str(args.port)]
    with log_path.open("w") as log:
        started = time.monotonic()
        proc = subprocess.Popen(command, env=env, stdin=subprocess.DEVNULL,
                                stdout=log, stderr=subprocess.STDOUT)
        try:
            while time.monotonic() - started < 900:
                if proc.poll() is not None:
                    raise RuntimeError(f"daemon exited {proc.returncode}; see {log_path}")
                try:
                    _, reply = rpc(sock, "health_check", {}, timeout=1)
                    if "warming_up" not in json.dumps(reply) and "error" not in reply:
                        break
                except (OSError, ValueError):
                    pass
                time.sleep(.1)
            else:
                raise TimeoutError("startup timeout")
            result["ready_ms"] = (time.monotonic() - started) * 1000
            common = {"query": args.query, "realm": args.realm, "limit": 5, "no_learn": True}
            elapsed, first = rpc(sock, "recall", common)
            result["first_recall_ms"] = elapsed
            result["first_ordered_ids"] = ids(first.get("result", {}))
            result["first_shape"] = shape(first)
            # Let the baseline's asynchronous Turbo worker finish; the first RPC above
            # is deliberately earlier. The same settling interval applies to both arms.
            time.sleep(30)
            result["rss_before"] = rss(proc.pid)
            _, accounting = rpc(sock, "health_check", {"memory_breakdown": True, "details": True})
            result["memory_breakdown"] = accounting.get("result", {}).get("structured", {})
            for block in accounting.get("result", {}).get("content", []):
                for line in block.get("text", "").splitlines():
                    if line.startswith("memory_breakdown "):
                        result["memory_breakdown"] = json.loads(line[len("memory_breakdown "):])
            methods = {
                "hybrid": ("recall", dict(common, strategy="hybrid")),
                "keyword": ("recall", dict(common, strategy="keyword")),
                "fused": ("recall", common),
                "smart": ("smart_recall", common),
                "recall_lanes": ("recall_lanes", dict(common, lanes=["sem", "ctx", "hyb", "kw", "corr"]))}
            for label, (method, params) in methods.items():
                samples, responses = [], []
                for _ in range(args.runs):
                    elapsed, reply = rpc(sock, method, params)
                    if "error" in reply or reply.get("result", {}).get("isError"):
                        raise RuntimeError(f"{label}: {reply}")
                    samples.append(elapsed)
                    responses.append(reply)
                result["methods"][label] = {
                    "p50_ms": statistics.median(samples),
                    "p95_ms": sorted(samples)[math.ceil(.95 * len(samples)) - 1],
                    "samples_ms": samples, "ordered_ids": [ids(r.get("result", {})) for r in responses],
                    "shape": shape(responses[0])}
            result["rss_after"] = rss(proc.pid)
            result["contracts"] = {}
            for method in ("recall", "smart_recall", "hybrid_recall", "recall_keyword", "recall_lanes"):
                params = methods["recall_lanes"][1] if method == "recall_lanes" else common
                _, reply = rpc(sock, method, params)
                result["contracts"][method] = shape(reply)
            if args.checkpoint:
                elapsed, reply = rpc(sock, "compact_wal", {}, timeout=600)
                if "error" in reply or reply.get("result", {}).get("isError"):
                    raise RuntimeError("scratch checkpoint failed")
                result["checkpoint_ms"] = elapsed
        finally:
            # Only this Popen-owned scratch child; never a daemon lookup or service command.
            proc.terminate()
            try:
                proc.wait(timeout=30)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()
    text = log_path.read_text()
    result["phases_ms"] = {m[1]: int(m[2]) for m in re.finditer(
        r"\[chitta-field\] load phase=(\w+) ms=(\d+)", text)}
    ready = re.search(r"\[daemon\] ready ms=(\d+)", text)
    result["process_ready_ms"] = int(ready[1]) if ready else None
    stages = {}
    for stage, micros in re.findall(r"stage=(\w+) us=(\d+)", text):
        stages.setdefault(stage, []).append(int(micros) / 1000)
    result["profile_stages_ms"] = {
        stage: {"count": len(values), "median": statistics.median(values), "max": max(values)}
        for stage, values in stages.items()}
    (OUT / f"results-{args.label}.json").write_text(json.dumps(result, indent=2) + "\n")
    rows = ["| Metric | " + args.label + " |", "|---|---:|",
            f"| Ready (ms) | {result['ready_ms']:.1f} |",
            f"| First recall (ms) | {result['first_recall_ms']:.1f} |",
            f"| RSS (MiB) | {result['rss_after']['VmRSS'] / 1024:.1f} |"]
    for name, data in result["methods"].items():
        rows.append(f"| {name} p50 / p95 (ms) | {data['p50_ms']:.1f} / {data['p95_ms']:.1f} |")
    for name, ms in result["phases_ms"].items():
        rows.append(f"| Load {name} (ms) | {ms} |")
    table = "\n".join(rows) + "\n"
    (OUT / f"table-{args.label}.md").write_text(table)
    print(table)


if __name__ == "__main__":
    main()
