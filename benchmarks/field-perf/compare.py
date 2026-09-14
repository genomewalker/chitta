"""Report strict parity and within-arm variation without hiding adaptive differences."""
import argparse
import json
import sys
from pathlib import Path


def differences(left, right, path=""):
    if isinstance(left, dict) and isinstance(right, dict):
        result = [path + "/" + key for key in sorted(left.keys() ^ right.keys())]
        for key in sorted(left.keys() & right.keys()):
            result.extend(differences(left[key], right[key], path + "/" + key))
        return result
    if isinstance(left, list) and isinstance(right, list):
        result = [] if len(left) == len(right) else [path + "/length"]
        for index, (old, new) in enumerate(zip(left, right)):
            result.extend(differences(old, new, path + f"/{index}"))
        return result
    return [] if left == right else [path]


parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("before", type=Path)
parser.add_argument("after", type=Path)
parser.add_argument("--output", type=Path)
args = parser.parse_args()
before, after = (json.loads(p.read_text()) for p in (args.before, args.after))
assert before["family"] == after["family"], "different replica families"
assert before["query"] == after["query"] and before["realm"] == after["realm"]
report = {"schema_differences": differences(before["contracts"], after["contracts"]),
          "methods": {}, "top_level_structured_keys_equal": {}}
for name, old in before["contracts"].items():
    new = after["contracts"][name]
    report["top_level_structured_keys_equal"][name] = (
        old["result"]["structured"].keys() == new["result"]["structured"].keys())
for name, old in before["methods"].items():
    new = after["methods"][name]
    pairs = list(zip(old["ordered_ids"], new["ordered_ids"]))
    matching = sum(a == b for a, b in pairs)
    report["methods"][name] = {
        "matching": matching, "samples": len(pairs),
        "before_variants": len(set(map(tuple, old["ordered_ids"]))),
        "after_variants": len(set(map(tuple, new["ordered_ids"])))}
    print(f"{name}: ordered IDs equal {matching}/{len(pairs)}; "
          f"within-arm variants {report['methods'][name]['before_variants']}/"
          f"{report['methods'][name]['after_variants']}")
print("Schema differences:", report["schema_differences"])
report["strict_equal"] = not report["schema_differences"] and all(
    item["matching"] == item["samples"] for item in report["methods"].values())
if args.output:
    args.output.write_text(json.dumps(report, indent=2) + "\n")
sys.exit(0 if report["strict_equal"] else 1)
