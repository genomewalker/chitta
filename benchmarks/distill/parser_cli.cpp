// Standalone bridge: compile with ssl_parser.cpp; consumes SSL on stdin.
#include "chitta/ssl_parser.hpp"
#include <nlohmann/json.hpp>
#include <iostream>
#include <iterator>

int main() {
    const std::string input{std::istreambuf_iterator<char>(std::cin), {}};
    auto result = chitta::SSLParser{}.parse(input);
    nlohmann::json triples = nlohmann::json::array();
    for (const auto& t : result.triplets)
        triples.push_back({t.subject, t.predicate, t.object});
    std::cout << nlohmann::json{{"triplets", triples},
                              {"learning_count", result.learnings.size()}}.dump() << '\n';
}
