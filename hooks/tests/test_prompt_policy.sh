#!/usr/bin/env bash
# Compare the complete synthetic admission fixtures through shell and native policy.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BIN="${CHITTA_POLICY_TEST_BIN:-$ROOT/bin/prompt_policy_test}"
[[ -x "$BIN" ]] || { echo 'skip: build prompt_policy_test for native policy parity'; exit 0; }
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
mkdir -p "$T/legacy" "$T/native" "$T/timeout"
export CHITTA_HOOK_NOW=1789516800000
CHITTA_PROMPT_CONTEXT=0 HOOK_PARITY_CAPTURE="$T/legacy" \
    bash "$ROOT/hooks/tests/test_prompt_core_lanes.sh" >"$T/legacy.log" 2>&1 || {
        echo "legacy fixture arm failed"; grep -E "FAIL:|Error|error" "$T/legacy.log" | head -20; exit 1;
    }
CHITTA_PROMPT_CONTEXT=1 STUB_POLICY_BIN="$BIN" HOOK_PARITY_CAPTURE="$T/native" \
    bash "$ROOT/hooks/tests/test_prompt_core_lanes.sh" >"$T/native.log" 2>&1 || {
        echo "native fixture arm failed"; grep -E "FAIL:|Error|error" "$T/native.log" | head -20; exit 1;
    }
diff -ru "$T/legacy" "$T/native"
CHITTA_PROMPT_CONTEXT=1 STUB_POLICY_BIN="$BIN" STUB_POLICY_MODE=timeout \
    HOOK_PARITY_CAPTURE="$T/timeout" bash "$ROOT/hooks/tests/test_prompt_core_lanes.sh" \
    >"$T/timeout.log" 2>&1 || {
        echo "timeout fixture arm failed"; grep -E "FAIL:|Error|error" "$T/timeout.log" | head -20; exit 1;
    }
diff -ru "$T/legacy" "$T/timeout"
printf 'native admission: all synthetic hook outputs byte-identical\n'
