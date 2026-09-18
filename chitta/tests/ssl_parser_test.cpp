#include "chitta/ssl_parser.hpp"
#include <cassert>
#include <fstream>
#include <iterator>
#include <iostream>
#include <nlohmann/json.hpp>

int main() {
    chitta::SSLParser parser;
    for (const std::string arrow : {"→", "->", "=>", "$\\rightarrow$", "\\rightarrow", "$ \\rightarrow $"}) {
        for (const std::string type : {"PATTERN", "DECISION", "GOTCHA", "INSIGHT", "BELIEF", "SOLUTION"}) {
            auto r = parser.parse("[" + type + "] [domain] a " + arrow + " b " + arrow + " c A:+0.5,0.4 F:CORE");
            assert(r.learnings.size() == 1);
            assert(r.triplets.size() == 2);
            assert(r.triplets[0].subject == "a" && r.triplets[0].object == "b");
            assert(r.triplets[1].subject == "b" && r.triplets[1].object == "c");
            assert(r.triplets[0].predicate == (type == "GOTCHA" ? "causes" : "leads_to"));
            assert(r.learnings[0].affect_valence == 0.5f);
            assert(r.learnings[0].flags == std::vector<std::string>{"CORE"});
        }
        auto set = parser.parse("[PATTERN] root " + arrow + " {a, b, c}");
        assert(set.triplets.size() == 3);
        assert(set.triplets[0].object == "a" && set.triplets[2].object == "c");
    }
    assert(parser.parse("[PATTERN] a→b @source:99999999999999999999999").triplets.size() == 1);
    auto choice = parser.parse("[DECISION] [db] sqlite→postgres|single-file A:+0.5,0.4 F:CORE");
    assert(choice.triplets.size() == 1 && choice.triplets[0].predicate == "chosen_over");
    assert(choice.triplets[0].subject == "sqlite" && choice.triplets[0].object == "postgres");
    auto native_choice = parser.parse("[DECISION] sqlite>postgres|single-file");
    assert(native_choice.triplets.size() == 1 && native_choice.triplets[0].predicate == "chosen_over");
    auto explicit_triple = parser.parse("[TRIPLET] thing depends_on other @2026-09-18 A:+0.5,0.4 F:CORE");
    assert(explicit_triple.triplets.size() == 1);
    assert(explicit_triple.triplets[0].predicate == "depends_on");
    assert(explicit_triple.triplets[0].object == "other");
    assert(explicit_triple.triplets[0].date_annotation == "2026-09-18");
    auto refs = parser.parse("[PATTERN] subject→object →@cross-ref A:+0.2,0.1\n[ε] echo a -> b");
    assert(refs.triplets.size() == 1 && refs.triplets[0].object == "object");
    assert(refs.learnings[0].refs == std::vector<std::string>{"cross-ref"});
    assert(parser.parse("[PATTERN] isolated A:+0.2,0.1\n[ε] a→b").triplets.empty());
    assert(parser.parse("[PATTERN] a→→b\n[UNKNOWN] c→d").triplets.empty());
    assert(parser.parse("[PATTERN] a→{}").triplets.empty());
    auto continued = parser.parse("[INSIGHT] a→b\nb→c");
    assert(continued.triplets.size() == 2);
    std::ifstream fixture(std::string(SSL_FIXTURES) + "/distill-arrows.ssl");
    assert(fixture.good());
    std::string body{std::istreambuf_iterator<char>(fixture), {}};
    std::ifstream expected_file(std::string(SSL_FIXTURES) + "/distill-arrows.json");
    nlohmann::json expected;
    expected_file >> expected;
    nlohmann::json actual = nlohmann::json::array();
    for (const auto& t : parser.parse(body).triplets)
        actual.push_back({t.subject, t.predicate, t.object});
    assert(actual == expected);
    std::cout << "SSL parser: all arrows, types, sets, annotations, choices, continuations and parity PASS\n";
}
