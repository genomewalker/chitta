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
    // An R package import must not resolve through Python's module heuristic.
    auto r = root / "analysis.R";
    std::ofstream(r) << "library(a)\nsource(\"a.py\")\n";
    paths.push_back(r.string());
    changed = {r.string()};
    nav.update(root.string(), "fixture", paths, changed, intel.extract_files(changed), false);
    auto imported = nav.query({{"question", "library source"}, {"realm", "fixture"}, {"limit", 40}});
    bool package = false, literal = false;
    for (const auto& edge : imported["edges"]) {
        if (edge["kind"] != "imports") continue;
        if (edge["surface"] == "a") { assert(edge["confidence"] == "EXTRACTED"); package = true; }
        if (edge["surface"] == "a.py") { assert(edge["confidence"] == "INFERRED"); literal = true; }
    }
    assert(package && literal);
    auto jl = root / "reads.jl", consumer = root / "consumer.jl";
    std::ofstream(jl) << "module Reads\nscore(x) = x\nend\n";
    std::ofstream(consumer) << "using Reads\n";
    paths.push_back(jl.string()); paths.push_back(consumer.string());
    changed = {jl.string(), consumer.string()};
    nav.update(root.string(), "fixture", paths, changed, intel.extract_files(changed), false);
    auto modules = nav.query({{"question", "using Reads"}, {"realm", "fixture"}, {"limit", 40}});
    bool module_import = false;
    for (const auto& edge : modules["edges"])
        if (edge["kind"] == "imports" && edge["surface"] == "Reads") {
            assert(edge["confidence"] == "INFERRED"); module_import = true;
        }
    assert(module_import);
    auto stages = root / "stages.nf", flow = root / "flow.nf";
    auto fixture = fs::path(__FILE__).parent_path().parent_path().parent_path() / "hooks/tests/fixtures/codenav/nextflow/navigation.nf";
    std::ifstream nf(fixture);
    std::string nf_source(std::istreambuf_iterator<char>(nf), {});
    assert(nf_source.find("\ndef label") != std::string::npos);
    nf_source.resize(nf_source.find("\ndef label"));
    std::ofstream(stages) << nf_source;
    std::ofstream(flow) << "workflow FLOW {\n ALIGN(Channel.of('sample'))\n COUNT(ALIGN.out)\n}\n";
    paths.push_back(stages.string()); paths.push_back(flow.string());
    changed = {stages.string(), flow.string()};
    nav.update(root.string(), "fixture", paths, changed, intel.extract_files(changed), false);
    auto channel_query = nlohmann::json{{"question", "ALIGN COUNT"}, {"realm", "fixture"}, {"limit", 40}};
    auto channels = nav.query(channel_query);
    bool routed = false;
    for (const auto& edge : channels["edges"])
        if (edge["source"].get<std::string>().ends_with(":ALIGN") && edge["surface"] == "COUNT") {
            assert(edge["confidence"] == "INFERRED" && edge["file"] == flow.string() && edge["line"] == 3);
            routed = true;
        }
    assert(routed);
    chitta::CodeNavigation channel_restart;
    channel_restart.open((root / "navigation.json").string());
    assert(channel_restart.query(channel_query) == channels);
    auto snake = root / "Snakefile";
    auto snake_fixture = fixture.parent_path().parent_path() / "snakemake/navigation.smk";
    fs::copy_file(snake_fixture, snake);
    paths.push_back(snake.string()); changed = {snake.string()};
    nav.update(root.string(), "fixture", paths, changed, intel.extract_files(changed), false);
    auto rule_graph = nav.query({{"question", "align count"}, {"path", snake.string()}, {"limit", 40}});
    bool rule_flow = false;
    for (const auto& edge : rule_graph["edges"])
        if (edge["source"].get<std::string>().ends_with(":align") && edge["surface"] == "count") {
            assert(edge["confidence"] == "INFERRED"); rule_flow = true;
        }
    assert(rule_flow);
    auto perl = root / "Reads.pm";
    fs::copy_file(fixture.parent_path().parent_path() / "perl/navigation.pl", perl);
    paths.push_back(perl.string()); changed = {perl.string()};
    nav.update(root.string(), "fixture", paths, changed, intel.extract_files(changed), false);
    auto packages = nav.query({{"question", "Reads BaseReads"}, {"path", perl.string()}, {"limit", 40}});
    bool package_base = false;
    for (const auto& edge : packages["edges"])
        if (edge["kind"] == "inherits" && edge["surface"] == "BaseReads") {
            assert(edge["source"].get<std::string>().ends_with(":Reads") && edge["line"] == 7);
            package_base = true;
        }
    assert(package_base);
    // Every grammar fixture must produce usable query edges, not just raw AST records.
    std::vector<fs::path> language_specs;
    for (const auto& item : fs::recursive_directory_iterator(fixture.parent_path().parent_path()))
        if (item.path().filename() == "expected.json") language_specs.push_back(item.path());
    std::sort(language_specs.begin(), language_specs.end());
    for (const auto& spec : language_specs) {
        std::ifstream input(spec); nlohmann::json expected; input >> expected;
        auto source = fs::weakly_canonical(spec.parent_path() / expected["file"].get<std::string>());
        std::vector<std::string> sources{source.string()};
        std::unordered_set<std::string> dirty{source.string()};
        chitta::CodeNavigation graph;
        graph.open((root / (expected["language"].get<std::string>() + ".json")).string());
        graph.update(source.parent_path().string(), "grammar", sources, dirty, intel.extract_files(dirty), true);
        auto answer = graph.query({{"path", source.string()}, {"limit", 40}});
        for (const auto& [key, kind] : std::vector<std::pair<std::string, std::string>>{{"calls", "calls"}, {"imports", "imports"}, {"inherits", "inherits"}})
            for (const auto& surface : expected.value(key, nlohmann::json::array())) {
                bool found = false;
                for (const auto& edge : answer["edges"]) found |= edge["kind"] == kind && edge["surface"] == surface;
                if (!found) std::cerr << expected["language"] << " missing query edge " << kind << ':' << surface << '\n';
                assert(found);
            }
        for (const auto& pair : expected.value("channels", nlohmann::json::array())) {
            bool found = false;
            for (const auto& edge : answer["edges"])
                found |= edge["kind"] == "calls" && edge["surface"] == pair[1] && edge["source"].get<std::string>().ends_with(":" + pair[0].get<std::string>());
            assert(found);
        }
    }
    fs::remove_all(root);
    std::cout << "navigation: confidence, ambiguity, scope, paths, restart identity, stale reads and deletion passed\n";
}
