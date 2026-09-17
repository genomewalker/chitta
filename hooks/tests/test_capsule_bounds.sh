#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
"${CXX:-g++}" -std=c++17 -O1 -pthread -I"$ROOT/chitta/include" \
    "$ROOT/hooks/tests/capsule-bounds.cpp" -lcrypto -o "$T/capsule-bounds"
"$T/capsule-bounds"
