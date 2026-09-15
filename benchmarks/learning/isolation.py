#!/usr/bin/env python3
"""Enforced model sandbox. Network connections are limited to HTTPS (TCP 443)."""

from __future__ import annotations

import ctypes
import os
import sys
from pathlib import Path

from common import command, require


def restrict_network():
    """Landlock ABI 4+: no TCP connects to live MCP/daemon ports, no TCP binds."""
    libc = ctypes.CDLL(None, use_errno=True)
    abi = libc.syscall(444, 0, 0, 1)
    require(abi >= 4, "Landlock network ABI >=4 is required for model execution")

    class Ruleset(ctypes.Structure):
        _fields_ = [("fs", ctypes.c_uint64), ("net", ctypes.c_uint64)]

    class Port(ctypes.Structure):
        _fields_ = [("access", ctypes.c_uint64), ("port", ctypes.c_uint64)]

    rules = Ruleset(0, 3)
    fd = libc.syscall(444, ctypes.byref(rules), ctypes.sizeof(rules), 0)
    require(fd >= 0, "cannot create network ruleset")
    try:
        rule = Port(2, 443)
        require(libc.syscall(445, fd, 2, ctypes.byref(rule), 0) == 0, "cannot allow HTTPS")
        require(libc.prctl(38, 1, 0, 0, 0) == 0, "cannot set no_new_privs")
        require(libc.syscall(446, fd, 0) == 0, "cannot enforce network ruleset")
    finally:
        os.close(fd)


def sandbox_command(visible, work, command_argv, runtime_roots=()):
    visible = Path(visible).resolve()
    args = [
        "/usr/bin/bwrap",
        "--die-with-parent",
        "--new-session",
        "--unshare-pid",
        "--unshare-ipc",
        "--unshare-uts",
        "--proc",
        "/proc",
        "--dev",
        "/dev",
        "--tmpfs",
        "/tmp",
    ]
    for path in ("/usr", "/bin", "/lib", "/lib64"):
        if Path(path).exists():
            args += ["--ro-bind", path, path]
    for path in (
        "/etc/ssl",
        "/etc/pki",
        "/etc/resolv.conf",
        "/etc/hosts",
        "/etc/nsswitch.conf",
        "/etc/passwd",
        "/etc/group",
        "/etc/ld.so.cache",
    ):
        if Path(path).exists():
            args += ["--ro-bind", path, path]
    for path in runtime_roots:
        args += ["--ro-bind", path, path]
    args += ["--bind", str(visible), str(visible), "--chdir", str(work), "--"]
    return args + list(map(str, command_argv))


def probe():
    p = command(["/usr/bin/bwrap", "--ro-bind", "/", "/", "--", "/bin/true"], check=False)
    if p.returncode:
        return p.stderr.strip()
    p = command([sys.executable, __file__, "--network-exec", "/bin/true"], check=False)
    return p.stderr.strip() if p.returncode else None


if __name__ == "__main__":
    require(sys.argv[1] == "--network-exec", "expected --network-exec")
    restrict_network()
    os.execvp(sys.argv[2], sys.argv[2:])
