#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
{
    printf '%s\n' '#include <vector>' '#include <map>' '#include <cmath>' '#include <algorithm>' '#include <cassert>'
    sed -n '/^\/\/ ANCHOR_TIES_BEGIN$/,/^\/\/ ANCHOR_TIES_END$/p' "$ROOT/chitta/src/handlers/field_memory_recall.cpp"
    cat <<'CPP'
struct Row { int id; float score; bool stale; };
int main() {
    std::vector<Row> rows{{1,2,true},{2,1,true},{3,1,false},{4,1,false},{5,0,false}};
    demote_stale_ties(rows, [](auto r){return r.score;}, [](auto r){return r.stale;});
    assert(rows[0].id==1); // A higher-scoring stale hit keeps its rank.
    assert(rows[1].id==3 && rows[2].id==4 && rows[3].id==2);
    assert(rows[4].id==5); // No general unanchored-is-fresher rule.
}
CPP
} > "$T/test.cpp"
"${CXX:-c++}" -std=c++17 "$T/test.cpp" -o "$T/test"
"$T/test"
echo 'ok: stale anchor demotion changes only equal-score ties'
