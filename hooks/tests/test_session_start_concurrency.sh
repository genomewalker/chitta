#!/bin/bash
# Frozen cards and native lifecycle transport boundaries.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
PYTHONDONTWRITEBYTECODE=1 python3 "$ROOT/hooks/tests/session_native_cases.py"
