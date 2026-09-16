#!/usr/bin/env bash
# Markdown extraction is tested without a daemon or store.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
TS_SOURCE=$(sed -n 's/^FETCHCONTENT_SOURCE_DIR_TREE-SITTER:PATH=//p' "$ROOT/chitta/build/CMakeCache.txt" 2>/dev/null || true)
TS_SOURCE="${TS_SOURCE:-$ROOT/chitta/build/_deps/tree-sitter-src}"
if [[ ! -f "$TS_SOURCE/lib/include/tree_sitter/api.h" ]] &&
   ! echo '#include <tree_sitter/api.h>' | "${CXX:-c++}" -x c++ -E - >/dev/null 2>&1; then
    echo "skip: tree-sitter headers not available (build the daemon once, or install tree-sitter)"
    exit 0
fi
cat > "$T/test.cpp" <<'CPP'
#include "chitta/code_intel.hpp"
#include <cassert>
int main(int argc, char** argv) {
    assert(argc == 2);
    const std::string path = argv[1];
    auto write = [&](const std::string& text) { std::ofstream f(path); f << text; };
    write("intro\n# Setup\nUse CHITTA_PATH here.\n```sh\n# not a heading\n```\n## Hook\nfile-changed-hook.sh\n## Hook\nsecond\n# Other\n## Hook\nthird\n");
    auto chunks = chitta::CodeIntel::extract_markdown(path);
    assert(chunks.size() == 6);
    assert(chunks[0].name == "(preamble)");
    assert(chunks[1].name == "Setup");
    assert(chunks[1].signature.find("# not a heading") != std::string::npos);
    assert(chunks[1].line_start == 2 && chunks[1].line_end == 6);
    assert(chunks[2].name == "Setup / Hook");
    assert(chunks[3].name == "Setup / Hook [2]");
    assert(chunks[5].name == "Other / Hook");
    const auto identity = chunks[2].name;
    write("new preamble\nintro\n# Setup\nnew text\n## Hook\nchanged body\n");
    chunks = chitta::CodeIntel::extract_markdown(path);
    assert(chunks[2].name == identity);
    assert(chunks[2].signature.find("changed body") != std::string::npos);
    write("Title\n=====\nbody\nChild\n-----\nbody\n~~~\n# fenced\n~~~\n");
    chunks = chitta::CodeIntel::extract_markdown(path);
    assert(chunks.size() == 2);
    assert(chunks[1].name == "Title / Child");
    write("");
    assert(chitta::CodeIntel::extract_markdown(path).empty());
    std::filesystem::remove(path);
    assert(chitta::CodeIntel::extract_markdown(path).empty());
}
CPP
"${CXX:-c++}" -std=c++17 -I"$ROOT/chitta/include" -I"$TS_SOURCE/lib/include" "$T/test.cpp" -o "$T/test"
"$T/test" "$T/document.md"
echo "ok: Markdown headings, fences, duplicate identity, edits and deletions"
