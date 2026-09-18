#!/usr/bin/env python3
"""Immutable, resumable 100-memory Ollama ablation using the daemon's SSL prompt.

Run on a login node with at most two workers. Analyze with parser_cli compiled
against production ssl_parser.cpp. Ollama eval_count includes thinking; separate
thinking tokens are unavailable and deliberately null rather than estimated.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import re
import statistics
import subprocess
import time
import urllib.request
from collections import Counter
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def save(path, data):
    with path.open("x") as out:
        json.dump(data, out, ensure_ascii=False, indent=2)
        out.write("\n")


def freeze(args):
    root = args.output
    root.mkdir(parents=True, exist_ok=True)
    manifest = root / "manifest.json"
    if manifest.exists():
        data = json.loads(manifest.read_text())
        for name, expected in data["files"].items():
            if sha(root / name) != expected:
                raise ValueError(f"frozen file changed: {name}")
        return data
    teacher = json.loads((args.pairs / "teacher.json").read_text())
    header = (args.repo / "chitta/include/chitta/ssl_prompt.hpp").read_text()
    prefix = re.search(r'EXTRACTION_PROMPT\s*=\s*R"\((.*?)\)";', header, re.S).group(1)
    if prefix != teacher["prefix"]:
        raise ValueError("teacher prefix differs from daemon")
    native = (args.repo / "chitta/src/native_distiller.cpp").read_text()
    if not all(part in native for part in teacher["system"].split(". ")):
        raise ValueError("teacher system prompt differs from daemon")
    items = [json.loads(line) for line in (args.pairs / "dev.jsonl").read_text().splitlines()]
    items = sorted(items, key=lambda item: item["id"])[:100]
    labels = [json.loads(p.read_text()) for p in (args.pairs / "labels").glob("*.json")]
    labels.sort(key=lambda item: (item["response"].get("created_at", ""), item["id"]))
    if len(items) != 100 or len(labels) < 339:
        raise ValueError("need 100 dev memories and 339 teacher outputs")
    files = {}
    for name, value in [("dev.json", items), ("teacher.json", teacher), ("labels.json", labels[:339])]:
        save(root / name, value)
        files[name] = sha(root / name)
    data = {"files": files, "source": str(args.pairs), "count": 100, "label_count": 339,
            "selection": "dev: id ascending; labels: created_at then id ascending",
            "model": teacher["model"], "host": args.host,
            "settings": [[think, limit] for limit in (8192, 2048) for think in (True, False)],
            "rule": "At 8192: relaxed micro triplet F1 >= 0.90 and every SSL type count within 10%; all 100 pairs complete, nonempty and untruncated."}
    save(manifest, data)
    return data


def run(args, manifest):
    root = args.output
    teacher = json.loads((root / "teacher.json").read_text())
    items = json.loads((root / "dev.json").read_text())
    responses = root / ("control-responses" if args.control else "responses")
    responses.mkdir(exist_ok=True)

    def request(job):
        item, think, limit = job
        dest = responses / f'{item["id"]}-{int(think)}-{limit}.json'
        if dest.exists():
            return
        payload = {"model": teacher["model"], "stream": False, "think": think,
                   "messages": [{"role": "system", "content": teacher["system"]},
                                {"role": "user", "content": teacher["prefix"] + item["text"] + teacher["suffix"]}],
                   "options": {"temperature": 0.3, "num_predict": limit}}
        start = time.monotonic()
        try:
            req = urllib.request.Request(manifest["host"] + "/api/chat",
                                         data=json.dumps(payload).encode(),
                                         headers={"Content-Type": "application/json"})
            with urllib.request.urlopen(req, timeout=900) as response:
                result = json.load(response)
            if "error" in result or not result.get("done"):
                raise ValueError(str(result.get("error", "incomplete response")))
            message = result["message"]
            save(dest, {"id": item["id"], "think": think, "num_predict": limit,
                        "latency_seconds": time.monotonic() - start,
                        "generated_tokens_including_thinking": result.get("eval_count"),
                        "thinking_tokens": None, "thinking_chars": len(message.get("thinking", "")),
                        "ssl_chars": len(message.get("content", "")),
                        "ssl_lines": dict(Counter(re.findall(r"^\[([A-Z]+)\]", message.get("content", ""), re.M))),
                        "request_sha256": hashlib.sha256(json.dumps(payload, sort_keys=True).encode()).hexdigest(),
                        "response": result})
        except (OSError, ValueError, KeyError) as error:
            save(responses / f'{dest.stem}-error-{time.time_ns()}.json',
                 {"id": item["id"], "error": str(error)})

    # Alternate settings within each item and reverse on alternate items.
    jobs = [(item, think, limit) for limit in (8192, 2048)
            for i, item in enumerate(items)
            for think in ((True, False) if i % 2 == 0 else (False, True))]
    if args.control:
        control_manifest = root / "control-manifest.json"
        specification = {"manifest_sha256": sha(root / "manifest.json"),
                         "count": 100, "think": True, "num_predict": 8192,
                         "temperature": 0.3, "purpose": "independent thinking-on self-agreement"}
        if control_manifest.exists():
            if json.loads(control_manifest.read_text()) != specification:
                raise ValueError("control manifest changed")
        else:
            save(control_manifest, specification)
        jobs = [(item, True, 8192) for item in items]
    with ThreadPoolExecutor(max_workers=args.workers) as pool:
        list(pool.map(request, jobs))


def parse(binary, body):
    return json.loads(subprocess.check_output([str(binary)], input=body.encode()))["triplets"]


def normalized(triplet):
    return tuple(" ".join(re.findall(r"\w+", entity.lower().replace("_", " ")))
                 for entity in triplet)


def agreement(pairs, relaxed):
    matched = left = right = 0
    for a, b in pairs:
        a = {normalized(t) if relaxed else tuple(t) for t in a}
        b = {normalized(t) if relaxed else tuple(t) for t in b}
        left += len(a)
        right += len(b)
        matched += len(a & b)
    return {"matched": matched, "on": left, "off": right,
            "f1": 2 * matched / (left + right) if left + right else 0.0}


def relation_summary(relations):
    counts = [len({tuple(t) for t in row}) for row in relations]
    return {"n": len(counts), "relations": sum(counts),
            "relations_per_memory": sum(counts) / len(counts) if counts else None,
            "covered_memories": sum(n > 0 for n in counts),
            "coverage": sum(n > 0 for n in counts) / len(counts) if counts else None}


def analyze(args, manifest):
    root = args.output
    rows = [json.loads(p.read_text()) for p in (root / "responses").glob("*.json") if "-error-" not in p.name]
    report = {"manifest_sha256": sha(root / "manifest.json"), "parser_sha256": sha(args.parser),
              "rule": manifest["rule"], "arms": {}, "comparisons": {}, "default_think": True,
              "thinking_token_note": "eval_count includes thinking; separate thinking tokens unavailable; character counts exact."}
    for limit in (8192, 2048):
        arms = {}
        for think in (True, False):
            subset = [r for r in rows if r["think"] == think and r["num_predict"] == limit]
            arms[think] = {r["id"]: r for r in subset}
            counts = Counter()
            for row in subset:
                counts.update(row["ssl_lines"])
            report["arms"][f"{think}-{limit}"] = {
                "n": len(subset), "ssl_lines": dict(counts),
                **{f"median_{key}": statistics.median(r[key] for r in subset) if subset else None
                   for key in ("latency_seconds", "generated_tokens_including_thinking", "thinking_chars", "ssl_chars")}}
        ids = sorted(arms[True].keys() & arms[False].keys())
        pairs = [(parse(args.parser, arms[True][i]["response"]["message"].get("content", "")),
                  parse(args.parser, arms[False][i]["response"]["message"].get("content", ""))) for i in ids]
        counts = {think: Counter() for think in (True, False)}
        for think in counts:
            for i in ids:
                counts[think].update(arms[think][i]["ssl_lines"])
        drift = {t: abs(counts[False][t] - counts[True][t]) / counts[True][t]
                 if counts[True][t] else None for t in counts[True].keys() | counts[False].keys()}
        valid = len(ids) == 100 and all(arms[t][i]["response"].get("done_reason") == "stop"
                                      and arms[t][i]["ssl_chars"] > 0 for t in arms for i in ids)
        comparison = {"paired_n": len(ids), "valid": valid, "exact": agreement(pairs, False),
                      "relaxed": agreement(pairs, True), "type_count_relative_drift": drift}
        comparison["relations"] = {name: relation_summary([pair[index] for pair in pairs])
                                   for index, name in enumerate(("on", "off"))}
        on_relations = comparison["relations"]["on"]["relations"]
        on_coverage = comparison["relations"]["on"]["covered_memories"]
        comparison["off_on_relation_count_ratio"] = (
            comparison["relations"]["off"]["relations"] / on_relations if on_relations else None)
        comparison["off_on_coverage_ratio"] = (
            comparison["relations"]["off"]["covered_memories"] / on_coverage if on_coverage else None)
        comparison["passes"] = valid and comparison["relaxed"]["f1"] >= 0.90 and all(
            d is not None and d <= 0.10 for d in drift.values())
        report["comparisons"][str(limit)] = comparison
    control = {r["id"]: r for p in (root / "control-responses").glob("*.json")
               if "-error-" not in p.name for r in [json.loads(p.read_text())]}
    primary = {r["id"]: r for r in rows if r["think"] and r["num_predict"] == 8192}
    ids = sorted(primary.keys() & control.keys())
    pairs = [(parse(args.parser, primary[i]["response"]["message"].get("content", "")),
              parse(args.parser, control[i]["response"]["message"].get("content", ""))) for i in ids]
    report["self_agreement"] = {"paired_n": len(ids), "exact": agreement(pairs, False),
                                "relaxed": agreement(pairs, True),
                                "valid": len(ids) == 100 and all(
                                    arm[i]["response"].get("done_reason") == "stop" and arm[i]["ssl_chars"] > 0
                                    for arm in (primary, control) for i in ids),
                                "relations": {name: relation_summary([p[index] for p in pairs])
                                              for index, name in enumerate(("on", "control"))}}
    control_counts = Counter()
    for row in control.values():
        control_counts.update(row["ssl_lines"])
    report["arms"]["control-True-8192"] = {
        "n": len(control), "ssl_lines": dict(control_counts),
        **{f"median_{key}": statistics.median(r[key] for r in control.values()) if control else None
           for key in ("latency_seconds", "generated_tokens_including_thinking", "thinking_chars", "ssl_chars")}}
    report["control_response_hashes"] = {p.name: sha(p) for p in sorted((root / "control-responses").glob("*.json"))}
    report["decision_status"] = "Keep thinking on until the control and 30-item fact review are complete. Relation counts alone do not establish fact retention."
    report["response_hashes"] = {p.name: sha(p) for p in sorted((root / "responses").glob("*.json"))}
    save(root / f"report-{time.time_ns()}.json", report)
    print(json.dumps({k: v for k, v in report.items() if not k.endswith("response_hashes")}))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("freeze", "run", "analyze"))
    parser.add_argument("--pairs", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--repo", type=Path, default=Path(__file__).resolve().parents[2])
    parser.add_argument("--host", default="http://dandygpun01fl:11434")
    parser.add_argument("--workers", type=int, choices=(1, 2), default=2)
    parser.add_argument("--parser", type=Path)
    parser.add_argument("--control", action="store_true", help="repeat thinking on at 8192 in a separate immutable arm")
    args = parser.parse_args()
    manifest = freeze(args)
    if args.action == "run":
        run(args, manifest)
    elif args.action == "analyze":
        if not args.parser:
            parser.error("analyze requires --parser")
        analyze(args, manifest)
    else:
        print(json.dumps(manifest))


if __name__ == "__main__":
    main()
