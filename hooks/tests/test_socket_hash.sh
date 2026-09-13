#!/bin/bash
# Preserve the socket hash while removing a subprocess per path character.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
source "$ROOT/hooks/lib.sh"
export LC_ALL=C
for value in '' /tmp/mind '/tmp/space and punctuation-_.123' "$(printf '%0300d' 0)"; do
    expected=$(python3 -c 'import sys; h=5381
for c in sys.argv[1].encode(): h=((h << 5) + h + c) & 0xffffffff
print(h)' "$value")
    actual=$(djb2_hash "$value")
    [[ "$actual" == "$expected" ]]
done
echo 'ok: socket hash matches DJB2 including overflow and empty input'
