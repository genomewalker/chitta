#include "chitta/code_intel.hpp"
#include <cassert>
#include <cstdlib>
#include <iostream>

int main() {
    namespace fs = std::filesystem;
    char scratch[] = "/tmp/chitta-code-files-XXXXXX";
    assert(mkdtemp(scratch));
    fs::path root = fs::path(scratch) / "repo with ' spaces";
    fs::create_directories(root);
    chitta::CodeIntel::git_output({"init", "-q", root.string()});
    auto write = [&](const auto& name, const auto& body) { std::ofstream(root / name) << body; };
    write("a.cpp", "int alpha() { return 1; }\n");
    write("b.py", "def beta():\n    return 2\n");
    write("ignored.cpp", "int ignored;\n");
    write("private.py", "secret = 1\n");
    write(".gitignore", "ignored.cpp\n");
    write(".chittaignore", "private.py\nuntracked.py\n");
    write("untracked.py", "private = True\n");
    chitta::CodeIntel::git_output({"-C", root.string(), "add", "-f", "ignored.cpp"});
    chitta::CodeIntel::git_output({"-C", root.string(), "add", "a.cpp", "private.py"});
    chitta::CodeIntel intel;
    auto files = intel.collect_source_files(root.string());
    assert(files.size() == 2 && fs::path(files[0]).filename() == "a.cpp");
    assert(intel.collect_source_files((root / "ignored.cpp").string()).empty());
    assert(intel.collect_source_files((root / "private.py").string()).empty());
    assert(intel.collect_source_files((root / "b.py").string()).size() == 1);
    for (int n = 0; n < 510; ++n) write("file" + std::to_string(n) + ".rs", "fn example() {}\n");
    assert(intel.collect_source_files(root.string()).size() == 512);
    assert(intel.collect_source_files(root.string(), {}, 3).size() == 3);
    fs::remove(root / "a.cpp");
    assert(intel.collect_source_files(root.string()).size() == 511);
    fs::remove_all(scratch);
    std::cout << "code files: Git filters, tracked ignores, quoting, deletion, uncapped coverage passed\n";
}
