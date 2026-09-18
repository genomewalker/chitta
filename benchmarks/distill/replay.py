"""Compare production SSL parsers on an immutable teacher snapshot."""
import argparse
import json
import subprocess
from pathlib import Path

from think_ablation import save, sha


def main():
    cli = argparse.ArgumentParser(__doc__)
    cli.add_argument("--data", type=Path, required=True)
    cli.add_argument("--before", type=Path, required=True)
    cli.add_argument("--after", type=Path, required=True)
    cli.add_argument("--output", type=Path, required=True)
    args = cli.parse_args()
    labels = json.loads((args.data / "labels.json").read_text())
    manifest = json.loads((args.data / "manifest.json").read_text())
    assert sha(args.data / "labels.json") == manifest["files"]["labels.json"]
    rows = []
    for item in labels:
        body = item["response"]["message"]["content"]
        row = {"id": item["id"]}
        for arm, binary in (("before", args.before), ("after", args.after)):
            result = subprocess.run([str(binary)], input=body, text=True, capture_output=True, check=False)
            row[arm] = json.loads(result.stdout)["triplets"] if result.returncode == 0 else []
            if result.returncode:
                row[arm + "_error"] = result.stderr.strip()
        rows.append(row)
    summary = {}
    for arm in ("before", "after"):
        counts = [len(row[arm]) for row in rows]
        summary[arm] = {"parser_errors": sum(arm + "_error" in row for row in rows), "triplets": sum(counts), "triplets_per_memory": sum(counts) / len(rows),
                        "memories_with_relations": sum(n > 0 for n in counts),
                        "coverage": sum(n > 0 for n in counts) / len(rows)}
    save(args.output, {"manifest_sha256": sha(args.data / "manifest.json"),
                       "before_sha256": sha(args.before), "after_sha256": sha(args.after),
                       "count": len(rows), "summary": summary, "items": rows})
    print(json.dumps(summary))


if __name__ == "__main__":
    main()
