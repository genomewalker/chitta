#include "chitta/code_navigation.hpp"
#include "chitta/code_intel.hpp"
#include <cassert>
#include <iostream>
#include <set>

int main() {
    namespace fs = std::filesystem;
    char temp[] = "/tmp/chitta-navigation-XXXXXX";
    assert(mkdtemp(temp));
    fs::path root(temp), a = root / "a.py", b = root / "b.py", c = root / "c.py", d = root / "d.cpp";
    std::ofstream(a) << "import math\nclass Base:\n    pass\nclass Child(Base):\n    pass\ndef target():\n    return 1\ndef caller():\n    return target()\n";
    std::ofstream(a, std::ios::app) << "# " << std::string(157, 'x') << "→ UTF-8 boundary\ndef unicode_doc():\n    pass\n";
    std::ofstream(b) << "def target():\n    return 2\n";
    std::ofstream(c) << "def ambiguous():\n    return target() + foreign_only()\n";
    std::ofstream(d) << "int foreign_only() { return 1; }\nclass Local { public: int onlymethod() { return 1; } };\nint unknown(Other& obj) { return obj.onlymethod(); }\nint Local::outside() { return onlymethod(); }\n";
    chitta::CodeIntel intel;
    std::vector<std::string> paths{a.string(), b.string(), c.string(), d.string()};
    std::unordered_set<std::string> changed(paths.begin(), paths.end());
    auto extraction = intel.extract_files(changed);
    bool signature = false, qualified = false;
    for (const auto& symbol : extraction.symbols) {
        if (symbol.name == "caller") { assert(symbol.signature.find("caller(") != std::string::npos); signature = true; }
        if (symbol.name == "outside") { assert(symbol.parent == "Local"); qualified = true; }
    }
    assert(signature && qualified);
    chitta::CodeNavigation nav;
    nav.open((root / "navigation.json").string());
    nav.update(root.string(), "project:fixture", paths, changed, extraction, true);
    auto query = nlohmann::json{{"question", "caller target Base Child ambiguous foreign_only unknown onlymethod"}, {"realm", "fixture"}, {"limit", 40}};
    auto before = nav.query(query);
    std::set<std::string> kinds;
    bool ambiguous = false, foreign = false, untyped = false;
    for (const auto& edge : before["edges"]) {
        kinds.insert(edge["kind"]);
        assert(edge["confidence"] == "EXTRACTED" || edge["confidence"] == "INFERRED");
        ambiguous |= edge["resolution"] == "AMBIGUOUS";
        if (edge["kind"] == "calls" && edge["surface"] == "foreign_only" && edge["file"] == c.string()) {
            assert(edge["confidence"] == "EXTRACTED"); foreign = true;
        }
        if (edge["kind"] == "calls" && edge["surface"] == "onlymethod" && edge["line"] == 3) {
            assert(edge["confidence"] == "EXTRACTED"); untyped = true;
        }
    }
    assert(kinds.count("calls") && kinds.count("inherits") && kinds.count("references"));
    assert(ambiguous && foreign && untyped);
    assert(nav.path({{"from", "caller"}, {"to", "target"}, {"path", a.string()}})["found"] == true);
    assert(nav.path({{"from", "outside"}, {"to", "onlymethod"}, {"path", d.string()}})["found"] == true);
    assert(nav.path({{"from", "caller"}, {"to", "target"}}).contains("error"));
    auto overview = nav.overview({{"project", "fixture"}});
    assert(overview["files"] == 4 && !overview["communities"].empty());
    auto text = overview["text"].get<std::string>();
    assert(std::count(text.begin(), text.end(), '\n') <= 40);
    chitta::CodeNavigation reopened;
    reopened.open((root / "navigation.json").string());
    assert(reopened.query(query) == before);
    assert(nav.read({{"name", "caller"}, {"path", a.string()}})["code"].get<std::string>().find("return target()") != std::string::npos);
    std::ofstream(a) << "def renamed():\n    return 3\n";
    assert(nav.read({{"name", "caller"}, {"path", a.string()}}).contains("error"));
    assert(nav.query({{"path", a.string()}})["stale"] == true);
    changed = {a.string()};
    nav.update(root.string(), "fixture", paths, changed, intel.extract_files(changed), false);
    assert(nav.read({{"name", "caller"}, {"path", a.string()}}).is_null());
    nav.remove(b.string());
    assert(nav.overview({{"project", "fixture"}})["files"] == 3);
    fs::remove_all(root);
    std::cout << "navigation: confidence, ambiguity, scope, paths, restart identity, stale reads and deletion passed\n";
}
