#!/usr/bin/env python3
"""Select and enumerate the exact snapshot family ChittaField::open prefers."""

# Selection mirrors manifest.rs Manifest::load()/validated_snapshot_path().
# Required .pld validation mirrors snapshot.rs load_payload_sidecar() and the
# stripped-content refusal in field.rs ChittaField::open().

import json
import pathlib
import re
import struct
import sys

U64_MAX = (1 << 64) - 1
U32_MAX = (1 << 32) - 1
U16_MAX = (1 << 16) - 1
SNAPSHOT_MAGIC_FIRST = 0xF01157417E000003
SNAPSHOT_MAGIC_V11 = 0xF01157417E00000B
SNAPSHOT_MAGIC_CURRENT = 0xF01157417E000017
PLD_MAGIC = 0x504C440000000001


def uint(value, maximum):
    return type(value) is int and 0 <= value <= maximum


def valid_ref(ref):
    return (isinstance(ref, dict) and isinstance(ref.get("name"), str)
            and uint(ref.get("size_bytes"), U64_MAX))


def valid_family(family):
    if not isinstance(family, dict) or not valid_ref(family.get("snapshot")):
        return False
    sidecars = family.get("sidecars")
    if not isinstance(sidecars, list) or not all(valid_ref(ref) for ref in sidecars):
        return False
    if not uint(family.get("snapshot_seqno"), U64_MAX):
        return False
    covered = family.get("covered", {})
    return isinstance(covered, dict) and all(
        isinstance(key, str) and uint(value, U64_MAX)
        for key, value in covered.items())


def valid_manifest(manifest):
    required = {"store_uuid": str, "embedding_model": str, "clean_shutdown": bool}
    if not isinstance(manifest, dict) or any(
            not isinstance(manifest.get(key), kind) for key, kind in required.items()):
        return False
    if (manifest.get("magic") != "CHITTA_FIELD_MANIFEST_V1"
            or not uint(manifest.get("format_version"), U32_MAX)
            or manifest["format_version"] != 1):
        return False
    for key, maximum in (("generation", U64_MAX), ("embedding_dim", U16_MAX),
                         ("next_memory_id", U64_MAX), ("next_artifact_id", U64_MAX),
                         ("last_seqno", U64_MAX)):
        if not uint(manifest.get(key), maximum):
            return False
    segments = manifest.get("segments")
    if not isinstance(segments, list):
        return False
    for segment in segments:
        if not isinstance(segment, dict) or not isinstance(segment.get("path"), str):
            return False
        if not all(uint(segment.get(key), U64_MAX)
                   for key in ("first_seqno", "last_seqno", "size_bytes")):
            return False
    checkpoint = manifest.get("checkpoints")
    if checkpoint is not None and not valid_family(checkpoint):
        return False
    families = manifest.get("families", {})
    return isinstance(families, dict) and all(
        isinstance(instance, str) and valid_family(family)
        for instance, family in families.items())


def ref_failure(root, ref):
    name, expected = ref["name"], ref["size_bytes"]
    if not name or pathlib.Path(name).name != name:
        return f"unsafe file reference {name!r}"
    path = root / name
    try:
        actual = path.stat().st_size
    except OSError:
        return f"missing {name}"
    if not path.is_file():
        return f"not a regular file: {name}"
    if actual != expected:
        return f"size mismatch for {name}: expected {expected}, got {actual}"
    return None


def snapshot_header(path):
    try:
        with path.open("rb") as handle:
            header = handle.read(16)
    except OSError as exc:
        raise ValueError(str(exc)) from exc
    if len(header) != 16:
        raise ValueError("snapshot too short")
    magic, seqno = struct.unpack("<QQ", header)
    if not SNAPSHOT_MAGIC_FIRST <= magic <= SNAPSHOT_MAGIC_CURRENT:
        raise ValueError(f"invalid full snapshot magic 0x{magic:016x}")
    return magic, seqno


def pld_error(path):
    try:
        size = path.stat().st_size
        with path.open("rb") as handle:
            header = handle.read(16)
            if len(header) != 16:
                return "file is shorter than its 16-byte header"
            magic, count = struct.unpack("<QQ", header)
            if magic != PLD_MAGIC:
                return f"wrong magic 0x{magic:016x}"
            offset = 16
            for record in range(count):
                item = handle.read(12)
                if len(item) != 12:
                    return f"truncated record header at entry {record}"
                _, length = struct.unpack("<QI", item)
                offset += 12 + length
                if offset > size:
                    return f"truncated content at entry {record}"
                handle.seek(length, 1)
    except OSError as exc:
        return str(exc)
    return None


def load_slots(root):
    slots, errors = [], []
    for path in (root / "MANIFEST.1", root / "MANIFEST.2"):
        if not path.is_file():
            continue
        try:
            manifest = json.loads(path.read_text())
        except (OSError, json.JSONDecodeError) as exc:
            errors.append(f"{path.name}: {exc}")
            continue
        if not valid_manifest(manifest):
            errors.append(f"{path.name}: invalid manifest schema, magic, or format")
            continue
        slots.append((path, manifest))
    if not slots:
        detail = "; ".join(errors) or "no MANIFEST.1 or MANIFEST.2 found"
        raise SystemExit(f"no usable manifest: {detail}")
    return slots


def merged_manifest(slots):
    if len(slots) == 1:
        _, newest = slots[0]
        older = None
    else:
        first, second = slots
        if first[1]["generation"] >= second[1]["generation"]:
            (_, newest), (_, older) = first, second
        else:
            (_, newest), (_, older) = second, first
    families = dict(newest.get("families", {}))
    if older is not None:
        for instance, family in older.get("families", {}).items():
            existing = families.get(instance)
            if existing is None or existing["snapshot_seqno"] < family["snapshot_seqno"]:
                families[instance] = family
    return newest, families


def select(root):
    slots = load_slots(root)
    newest, families = merged_manifest(slots)
    candidates = [families[key] for key in sorted(families)]
    if newest.get("checkpoints") is not None:
        candidates.append(newest["checkpoints"])
    candidates.sort(key=lambda family: family["snapshot_seqno"], reverse=True)
    if not candidates:
        raise SystemExit(f"no usable manifest generation {newest['generation']}: "
                         "no committed checkpoint entries")

    failures, previous_name, selected = [], None, None
    for family in candidates:
        snapshot_name = family["snapshot"]["name"]
        # Vec::dedup_by removes adjacent duplicates after Rust's stable sort.
        if snapshot_name == previous_name:
            continue
        previous_name = snapshot_name
        match = re.fullmatch(r"chitta\.([0-9A-Fa-f]+)\.snapshot", snapshot_name)
        if not match:
            failures.append(f"unsafe snapshot name {snapshot_name!r}")
            continue
        refs = [family["snapshot"], *family["sidecars"]]
        bad = next((failure for ref in refs if (failure := ref_failure(root, ref))), None)
        if bad:
            failures.append(f"{snapshot_name}: {bad}")
            continue
        selected = match.group(1).lower(), family, refs
        break
    if selected is None:
        raise SystemExit("no consistent snapshot family: " + "; ".join(failures[:8]))

    snapshot_id, family, recorded_refs = selected
    snapshot_name = family["snapshot"]["name"]
    try:
        magic, header_seqno = snapshot_header(root / snapshot_name)
    except ValueError as exc:
        raise SystemExit(f"committed snapshot {snapshot_name} is not loadable: {exc}") from exc

    pld_name = f"chitta.{snapshot_id}.pld"
    pld_path = root / pld_name
    if magic >= SNAPSHOT_MAGIC_V11 and not pld_path.is_file():
        raise SystemExit(
            f"committed snapshot family {snapshot_name} selected by manifest generation "
            f"{newest['generation']} is incomplete: required sidecar {pld_name} is missing "
            "(snapshot v11+ stores payload content in .pld)")
    if magic >= SNAPSHOT_MAGIC_V11 and (corrupt := pld_error(pld_path)):
        raise SystemExit(
            f"committed snapshot family {snapshot_name} selected by manifest generation "
            f"{newest['generation']} is incomplete: required sidecar {pld_name} is corrupt: "
            f"{corrupt}")

    files = {ref["name"]: ref["size_bytes"] for ref in recorded_refs}
    optional_suffixes = (
        "emb", "hdc", "bin", "mu", "hnsw", "delta.hnsw", "realm_hnsw",
        "pld", "sup.json", "shdr", "rsf", "lsh", "organs", "turbo", "turbo.meta")
    for suffix in optional_suffixes:
        path = root / f"chitta.{snapshot_id}.{suffix}"
        if path.is_file():
            files[path.name] = path.stat().st_size

    print(f"ID\t{snapshot_id}")
    print(f"SEQNO\t{family['snapshot_seqno']}")
    print(f"HEADER_SEQNO\t{header_seqno}")
    print(f"GENERATION\t{newest['generation']}")
    for path in (root / "MANIFEST.1", root / "MANIFEST.2"):
        if path.is_file():
            print(f"MANIFEST\t{path.name}\t{path.stat().st_size}")
    for name, size in files.items():
        print(f"FILE\t{name}\t{size}")
    for name in (f"cortex.{snapshot_id}.snapshot", f"seen_offsets.{snapshot_id}.json"):
        path = root / name
        if path.is_file():
            print(f"FILE\t{name}\t{path.stat().st_size}")
    segment_dir = root / "segments"
    if segment_dir.is_dir():
        for path in sorted(segment_dir.iterdir()):
            if path.is_file() and re.fullmatch(r"[0-9A-Fa-f]{8}_[0-9]{12}\.seg", path.name):
                print(f"WAL\t{path.name}\t{path.stat().st_size}")


if __name__ == "__main__":
    if len(sys.argv) != 2:
        raise SystemExit(f"usage: {pathlib.Path(sys.argv[0]).name} STORE_DIR")
    select(pathlib.Path(sys.argv[1]))
