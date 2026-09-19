#!/usr/bin/env python3
"""FFI declaration drift check: every cf_* declared in field_store.hpp must
exist in chitta-field's ffi.rs with the SAME parameter count. C linkage never
fails the link on signature mismatch — arity drift is silent UB. Exits 1 on
drift. Run before releases (and whenever the hand-rolled extern block in
field_store.hpp changes)."""
import re
import sys
import pathlib

root = pathlib.Path(__file__).resolve().parent.parent
hpp = (root / "chitta/include/chitta/field_store.hpp").read_text()
ffi_paths = [root / "chitta-field/src/ffi.rs", *sorted((root / "chitta-field/src/ffi").glob("*.rs"))]
ffi = "\n".join(path.read_text() for path in ffi_paths if path.exists())


def arity(params: str) -> int:
    # rustfmt leaves a trailing comma in multiline signatures.
    p = params.strip().rstrip(",").strip()
    if not p or p == "void":
        return 0
    depth, n = 0, 1
    for c in p:
        if c in "<([":
            depth += 1
        elif c in ">)]":
            depth -= 1
        elif c == "," and depth == 0:
            n += 1
    return n


rust = {
    m.group(1): arity(m.group(2))
    for m in re.finditer(
        r'pub (?:unsafe )?extern "C" fn (cf_\w+)\((.*?)\)\s*(?:->|\{)', ffi, re.S
    )
}

# Scope to extern "C" blocks only — the class also has private cf_* member
# wrappers (no handle param) that are not part of the ABI.
extern_blocks = []
for m in re.finditer(r'extern "C" \{', hpp):
    depth, j = 1, m.end()
    while depth > 0 and j < len(hpp):
        if hpp[j] == "{":
            depth += 1
        elif hpp[j] == "}":
            depth -= 1
        j += 1
    extern_blocks.append(hpp[m.end():j])
externs = "\n".join(extern_blocks)

bad = 0
for m in re.finditer(r"^\s*(?:[\w:*&<>]+\s+)+\**(cf_\w+)\((.*?)\);", externs, re.M | re.S):
    name, n = m.group(1), arity(m.group(2))
    if name not in rust:
        print(f"DRIFT: {name} declared in field_store.hpp but absent from ffi.rs")
        bad += 1
    elif rust[name] != n:
        print(f"DRIFT: {name} arity mismatch — hpp={n} ffi.rs={rust[name]} (silent UB)")
        bad += 1

print(f"{bad} drift(s); {len(rust)} ffi.rs externs checked against hand decls")
sys.exit(1 if bad else 0)
