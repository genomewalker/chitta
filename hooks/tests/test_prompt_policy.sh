#!/usr/bin/env bash
# Admission and fusion are tested through the daemon-only hook transport.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
export CHITTA_HOOK_NOW=1789516800000
bash "$ROOT/hooks/tests/test_prompt_core_lanes.sh"
