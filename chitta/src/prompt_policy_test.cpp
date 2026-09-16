#include <chitta/prompt_policy.hpp>
#include <cassert>
#include <iostream>

int main(int argc, char** argv) {
    using chitta::prompt_policy::admit;
    using chitta::prompt_policy::json;
    if (argc == 2 && std::string(argv[1]) == "--stdin") {
        json state;
        std::cin >> state;
        std::cout << admit(state).dump() << '\n';
        return 0;
    }
    assert(chitta::prompt_policy::hash("") == "d41d8cd98f00b204");
    assert(chitta::prompt_policy::hash("abc") == "900150983cd24fb0");
    assert(chitta::prompt_policy::prefix("α→beta", 3) == "α→b");
    json state = {{"query", "apricot orchard"}, {"query_tokens", "apricot\norchard"},
        {"distinct_tokens", "apricot\norchard"}, {"session_id", "fixture"}, {"c2_pct", "81"},
        {"memories", "#1 [90%] [wisdom] apricot semantic\n"
                     "#2 [80%] [wisdom] apricot second\n"
                     "[kw]#3 [40%] [wisdom] apricot keyword\n"
                     "[corr]#4 [80%] [wisdom] apricot correction\n"}};
    const auto known = admit(state);
    assert(known["c2_tag"] == "KNOWN");
    assert(known["count"] == 3);
    assert(known["fused_block"] == "[corr]#4 [80%] [wisdom] apricot correction\n"
                                  "[kw]#3 [40%] [wisdom] apricot keyword\n"
                                  "[sem]#1 [90%] [wisdom] apricot semantic\n");
    assert(known["dropped"]["cap"] == 1);
    state["c2_pct"] = "80";
    const auto unknown = admit(state);
    assert(unknown["c2_tag"] == "UNKNOWN" && unknown["count"] == 1);
    assert(unknown["dropped"]["unk"] == 3);
    state["small_realm"] = true;
    assert(admit(state)["count"] == 3);
    state["seen_hashes"] = unknown["hashes"][0].get<std::string>();
    assert(admit(state)["dropped"]["dup"] == 1);
    state["query"] = "is not apricot";
    assert(admit(state)["count"] == 0);
    assert(admit(state)["terse_negation"] == true);
    state["query"] = "apricot";
    state["memories"] = "#1 [99%] [episode] [thinking block: hidden]\n"
                        "#2 [99%] [wisdom] [thought] hidden\n"
                        "#3 [99%] [wisdom] [cache:break] hidden\n";
    state["c2_pct"] = "";
    assert(admit(state)["dropped"]["meta"] == 3);
    for (const std::string& noise : {
        "[session:test] working→3 turns", "[session:test] completed→8 turns",
        "pre-compact context: apricot", "[pre-compact: apricot]",
        "You are **Codex** in a multi-agent discussion",
        "DONE (Fable abcdef01)", "Smith_et_al_2020_ABC"}) {
        state["memories"] = "#9 [99%] [wisdom] " + noise;
        assert(admit(state)["dropped"]["meta"] == 1);
    }
    std::cout << "prompt_policy_test: passed\n";
}
