#include "chitta/code_intel.hpp"
#include <algorithm>
#include <cassert>
#include <iostream>

int main(int argc, char** argv) {
    assert(argc == 2);
    namespace fs = std::filesystem;
    chitta::CodeIntel intel;
    std::vector<fs::path> fixtures;
    for (const auto& item : fs::recursive_directory_iterator(argv[1]))
        if (item.path().filename() == "expected.json") fixtures.push_back(item.path());
    std::sort(fixtures.begin(), fixtures.end());
    for (const auto& fixture : fixtures) {
        std::ifstream input(fixture);
        nlohmann::json expected; input >> expected;
        const auto path = fixture.parent_path() / expected["file"].get<std::string>();
        const auto language = expected["language"].get<std::string>();
        assert(intel.detect_language(path.string()) == language);
        for (const auto& alias : expected.value("aliases", nlohmann::json::array()))
            assert(intel.detect_language(alias.get<std::string>()) == language);
        const auto result = intel.extract_file_full(path.string());
        for (const auto& [name, kind] : expected["symbols"].items()) {
            auto found = std::find_if(result.symbols.begin(), result.symbols.end(), [&](const auto& s) { return s.name == name; });
            if (found == result.symbols.end()) { std::cerr << language << ": missing symbol " << name << '\n'; return 1; }
            assert(found->kind == kind.get<std::string>() && !found->signature.empty() && found->line_start > 0 && found->line_end >= found->line_start);
        }
        const auto signatures = expected.value("signatures", nlohmann::json::object());
        for (const auto& [name, signature] : signatures.items()) {
            auto found = std::find_if(result.symbols.begin(), result.symbols.end(), [&](const auto& s) { return s.name == name; });
            assert(found != result.symbols.end() && found->signature.starts_with(signature.get<std::string>()));
        }
        for (const auto& name : expected.value("absent", nlohmann::json::array()))
            assert(std::none_of(result.symbols.begin(), result.symbols.end(), [&](const auto& s) { return s.name == name.get<std::string>(); }));
        for (const auto& name : expected.value("calls", nlohmann::json::array())) {
            if (std::none_of(result.callsites.begin(), result.callsites.end(), [&](const auto& c) { return c.callee_leaf == name.get<std::string>(); })) {
                std::cerr << language << ": missing call " << name << '\n'; return 1;
            }
        }
        for (const auto& name : expected.value("imports", nlohmann::json::array())) {
            if (std::none_of(result.imports.begin(), result.imports.end(), [&](const auto& i) { return i.import_path == name.get<std::string>(); })) {
                std::cerr << language << ": missing import " << name << '\n'; return 1;
            }
        }
        for (const auto& name : expected.value("inherits", nlohmann::json::array()))
            assert(std::any_of(result.type_relationships.begin(), result.type_relationships.end(), [&](const auto& i) { return i.base_name == name.get<std::string>(); }));
        std::ifstream source_file(path);
        std::string source(std::istreambuf_iterator<char>(source_file), {});
        auto* parser = ts_parser_new();
        assert(ts_parser_set_language(parser, intel.extended_grammar(language)));
        auto* tree = ts_parser_parse_string(parser, nullptr, source.data(), source.size());
        if (ts_node_has_error(ts_tree_root_node(tree))) { std::cerr << language << ": fixture parse error\n"; return 1; }
        ts_tree_delete(tree);
        const auto start = std::chrono::steady_clock::now();
        for (int n = 0; n < 200; ++n) {
            tree = ts_parser_parse_string(parser, nullptr, source.data(), source.size());
            assert(tree); ts_tree_delete(tree);
        }
        const auto us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count();
        ts_parser_delete(parser);
        std::cout << nlohmann::json({{"language", language}, {"symbols", result.symbols.size()},
            {"calls", result.callsites.size()}, {"imports", result.imports.size()}, {"inherits", result.type_relationships.size()},
            {"references", result.references.size()}, {"parse_us_per_KB", us / 200 / (source.size() / 1024.0)}}).dump() << '\n';
    }
    // The production hooks are a second shell fixture: real function definitions
    // must be indexed, not just a carefully selected miniature.
    auto root = fs::path(argv[1]).parent_path().parent_path().parent_path().parent_path();
    if (fs::exists(root / "hooks/lib.sh")) {
        auto hooks = intel.extract_file_full((root / "hooks/lib.sh").string());
        assert(hooks.symbols.size() > 10);
        assert(!hooks.callsites.empty());
    }
}
