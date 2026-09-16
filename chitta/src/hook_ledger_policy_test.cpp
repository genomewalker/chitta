#include <chitta/hook_ledger_policy.hpp>
#include <cassert>
#include <iostream>

int main() {
    using namespace chitta::hook_ledger;
    json input{{"session_id", "s"}, {"project_dir", "/fixture/project"}, {"branch", "feature"},
        {"next_action", "Next action: run the fixture"}, {"blocker", ""},
        {"artifact_paths", {"b", "a", "b"}}};
    auto cap = capsule(input, json::object(), json::object(), 100);
    assert(cap["verified"] && cap["artifact_paths"] == json({"a", "b"}));
    json rows = {{{"metadata_json", json({{"handoff", cap}}).dump()}}};
    assert(handoff_card(rows, "/fixture/project", "feature") ==
        "[handoff]\nNext action: Next action: run the fixture\nBranch: feature\nArtifacts: a, b\n"
        "Blocker: none recorded\nSource: visible_plan (session s)\n[/handoff]");
    assert(handoff_card(rows, "/fixture/project", "another").empty());
    input["next_action"] = "";
    const auto cleared = capsule(input, json(), json(), 101);
    rows.push_back({{"metadata_json", json({{"handoff", cleared}}).dump()}});
    assert(handoff_card(rows, "/fixture/project", "feature").empty());
    json session{{"thread_id", "t"}};
    json thread{{"title", "Never invent a plan from the title"}, {"metadata_json", "{}"}};
    assert(!capsule(input, session, thread, 102)["verified"].get<bool>());
    thread["metadata_json"] = "{\"next_steps\":[\"inspect the failing assertion\"]}";
    const auto inherited = capsule(input, session, thread, 103);
    assert(inherited["next_action"] == "inspect the failing assertion");
    assert(inherited["source"]["thread_id"] == "t" && inherited["source"]["kind"] == "ledger_thread");
    assert(task_card({{{"event_type", "completed"}, {"digest", "fixture done"}}},
        {{{"title", "follow up"}, {"thread_id", "123456789"}}}, "project:test") ==
        "\n━━━ inbox (project:test) ━━━\n✓ fixture done\n\n━━━ active threads ━━━\n  ⟳  follow up [12345678]");
    assert(task_card({{{"event_type", "other"}, {"digest", "embedded\nnewline\n\n"}}},
        {{{"title", "Thread α\nsecond line"}, {"thread_id", "12345678-long"}}}, "project:test") ==
        "\n━━━ inbox (project:test) ━━━\n• embedded\nnewline\n\n━━━ active threads ━━━\n  ⟳  Thread α\nsecond line [12345678]");
    json ledger{{"session_id", "s"}, {"mood", "working"}, {"active_files", {"a.cpp"}},
        {"decisions", {"retain timeout fallback"}}, {"snapshot", "A detailed context snapshot for the fixture"}};
    assert(ledger_card(ledger, "startup", 100)["card"] == "[ledger] s (working)\n");
    assert(ledger_card(ledger, "compact", 100)["card"] ==
        "\n[session-restored]\nFiles in context:\n  - a.cpp\n\nDecisions made:\n  - retain timeout fallback\n"
        "\nLast context:\nA detailed context snapshot for the fixture\n\n[/session-restored]\n\n");
    ledger["mood"] = "in_progress";
    ledger["updated_at"] = "1970-01-01T00:00:00Z";
    assert(ledger_card(ledger, "clear", 100)["post_clear"]);
    assert(!ledger_card(ledger, "clear", 15000)["post_clear"].get<bool>());
    std::cout << "hook_ledger_policy_test: passed\n";
}
