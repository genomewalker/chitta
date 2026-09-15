"""Small, fail-closed primitives shared by the learning experiment tools."""

from __future__ import annotations

import contextlib
import hashlib
import json
import os
import socket
import subprocess
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
REALM = "project:cc-soul"
MARKERS = ("embed_1536_v1.migrated", "ssl_gloss_v1.migrated", "lite_encoder.bin")


def require(condition, reason):
    if not condition:
        raise ValueError(reason)


def digest(path):
    h = hashlib.sha256()
    with Path(path).open("rb") as f:
        for block in iter(lambda: f.read(8 * 1024 * 1024), b""):
            h.update(block)
    return h.hexdigest()


def canonical(value):
    return json.dumps(value, sort_keys=True, separators=(",", ":")).encode()


def seal(value):
    return hashlib.sha256(canonical(value)).hexdigest()


def write_json(path, value):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_suffix(path.suffix + ".tmp")
    tmp.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n")
    tmp.replace(path)


def read_json(path):
    return json.loads(Path(path).read_text())


def command(argv, *, cwd=None, env=None, timeout=600, check=True, input=None):
    p = subprocess.run(
        list(map(str, argv)),
        cwd=cwd,
        env=env,
        input=input,
        stdin=subprocess.DEVNULL if input is None else None,
        capture_output=True,
        text=True,
        timeout=timeout,
    )
    if check and p.returncode:
        raise ValueError(f"{argv[0]} failed ({p.returncode}): {p.stderr[-1500:]}")
    return p


def git(repo, *args):
    return command(["git", "-c", "core.hooksPath=/dev/null", "-C", repo, *args]).stdout.strip()


def socket_for(mind, runtime):
    h = 5381
    for byte in str(mind).encode():
        h = (h * 33 + byte) & 0xFFFFFFFF
    return str(Path(runtime) / "chitta" / f"chitta-{h}.sock")


def live_paths():
    home = Path(os.environ["HOME"]).resolve()
    mind = Path(os.environ.get("CHITTA_DB_PATH", home / ".claude/mind"))
    runtime = os.environ.get("XDG_RUNTIME_DIR", f"/run/user/{os.getuid()}")
    sock = Path(os.environ.get("CHITTA_SOCKET_PATH", socket_for(mind, runtime))).resolve()
    return {"home": str(home), "mind": str(mind.resolve()), "socket": str(sock)}


class RPC:
    """Existing tools/call protocol; no CLI auto-start or lossy u64 conversion."""

    READS = {"query_graph", "list_memories_brief", "memory_provenance", "get", "health_check"}

    def __init__(self, path, *, writable=False):
        self.path = str(path)
        self.writable = writable

    def call(self, name, **arguments):
        require(self.writable or name in self.READS, f"read-only RPC rejects {name}")
        request = {
            "jsonrpc": "2.0",
            "id": 1,
            "method": "tools/call",
            "params": {"name": name, "arguments": arguments},
        }
        with socket.socket(socket.AF_UNIX) as sock:
            sock.settimeout(120)
            sock.connect(self.path)
            sock.sendall(canonical(request) + b"\n")
            with sock.makefile("rb") as stream:
                raw = stream.readline(64 * 1024 * 1024)
        try:
            response = json.loads(raw)
        except json.JSONDecodeError as exc:
            raise ValueError(f"invalid JSON from {name} {arguments}: {raw[:350]!r}") from exc
        require("error" not in response, f"RPC {name}: {response.get('error')}")
        result = response["result"]
        message = "\n".join(c.get("text", "") for c in result.get("content", []))
        if result.get("isError"):
            if name == "get" and message.lower().startswith("memory not found"):
                return None
            raise ValueError(f"RPC {name}: {message}")
        return result.get("structured", result.get("structuredContent", {"text": message}))


def family(mind, *, hash_files=True):
    """Never open a store. Use the immutable launcher's exact family selector."""
    field = Path(mind).resolve() / "chitta-field"
    import sys

    selection = command([sys.executable, ROOT / "scripts/eval-replica-select.py", field]).stdout
    files, metadata = {}, {}
    for line in selection.splitlines():
        parts = line.split("\t")
        kind, name = parts[:2]
        if kind in {"FILE", "MANIFEST", "WAL"}:
            rel = f"segments/{name}" if kind == "WAL" else name
            files[rel] = {"size": int(parts[2])}
        else:
            metadata[kind.lower()] = name
    for name in MARKERS:
        if (field / name).is_file():
            files[name] = {"size": (field / name).stat().st_size}
    # Manifests always get hashes, including live read-only classification reports.
    for name, info in files.items():
        if hash_files or name.startswith("MANIFEST."):
            info["sha256"] = digest(field / name)
        require((field / name).stat().st_size == info["size"], f"source changed: {name}")
    again = command([sys.executable, ROOT / "scripts/eval-replica-select.py", field]).stdout
    require(selection == again, "source family changed while hashing")
    for name, info in files.items():
        if name.startswith("MANIFEST."):
            require(digest(field / name) == info["sha256"], "manifest changed while hashing")
    return {
        "mind": str(Path(mind).resolve()),
        "selection": metadata,
        "files": files,
        "sha256": seal(files),
        "fully_hashed": hash_files,
    }


def initial_hashes(repo, sha):
    """Git tree includes modes and symlinks; blob SHA256s cover actual bytes."""
    rows = git(repo, "ls-tree", "-r", "--full-tree", sha)
    result = {}
    for row in rows.splitlines():
        meta, path = row.split("\t", 1)
        mode, kind, oid = meta.split()
        require(kind == "blob", f"submodule must be reconstructed explicitly: {path}")
        p = subprocess.run(
            ["git", "-C", str(repo), "cat-file", "blob", oid], capture_output=True, check=True
        )
        result[path] = {"mode": mode, "sha256": hashlib.sha256(p.stdout).hexdigest()}
    return result


@contextlib.contextmanager
def task_worktree(task, parent):
    """Fetch only the initial commit; no alternates, shared objects or future refs."""
    parent = Path(parent)
    bare, work = parent / "objects.git", parent / "work"
    git(parent, "init", "--quiet", "--bare", str(bare))
    git(
        bare,
        "fetch",
        "--quiet",
        "--depth=1",
        Path(task["repo"]).resolve().as_uri(),
        task["cwd_sha"],
    )
    git(bare, "worktree", "add", "--quiet", "--detach", str(work), "FETCH_HEAD")
    require(git(work, "rev-parse", "HEAD") == task["cwd_sha"], "wrong task checkout")
    require(
        initial_hashes(work, "HEAD") == task["initial_state_hashes"], "initial tree hash mismatch"
    )
    try:
        yield work
    finally:
        git(bare, "worktree", "remove", "--force", str(work))


def now_ms():
    return time.time_ns() // 1_000_000


def tree_identity(root):
    """Pin an explicitly declared runtime dependency tree, including link targets."""
    root = Path(root).resolve()
    result = {}
    for path in sorted(root.rglob("*")):
        rel = str(path.relative_to(root))
        if path.is_symlink():
            result[rel] = {"link": os.readlink(path)}
        elif path.is_file():
            result[rel] = {"sha256": digest(path), "mode": path.stat().st_mode & 0o777}
    return seal(result)


def validate_runtime_roots(roots, protected):
    for name in roots:
        path = Path(name).resolve()
        require(
            path.is_dir() and len(path.parts) >= 3,
            "runtime mount must be a specific dependency directory",
        )
        for forbidden in protected:
            f = Path(forbidden).resolve()
            require(
                not path.is_relative_to(f) and not f.is_relative_to(path),
                f"runtime mount exposes protected data: {name}",
            )
