# Source in tests with ROOT and T set; compile production policy into a fixture CLI.
export LEDGER_TEST_BIN="$T/ledger-response"
"${CXX:-g++}" -std=c++17 -O2 -I"$ROOT/chitta/include" \
    "$ROOT/hooks/tests/ledger-response.cpp" -lcrypto -o "$LEDGER_TEST_BIN" || exit 1
