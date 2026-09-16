#!/bin/bash
set -euo pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
"${CXX:-g++}" -std=c++17 -O2 -pthread -I"$ROOT/chitta/include" \
    "$ROOT/hooks/tests/event-response.cpp" -lcrypto -o "$T/cli"
python3 "$ROOT/hooks/tests/ancillary_native_cases.py" "$ROOT" "$T/cli"
