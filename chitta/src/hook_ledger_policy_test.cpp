#include <chitta/hook_ledger_policy.hpp>
#include <cassert>
#include <iostream>

int main() {
    using namespace chitta::hook_ledger;
    json v2{{"repository", "/maps/projects/repo/.git"}, {"session_id", "s"},
        {"stream_id", "stream"}, {"next_action", "run tests"},
        {"gates", {{"quick", {{"status", "pass"}, {"number", 1}}}}}};
    const auto saved = capsule_v2(v2, 9, 100);
    assert(saved["repository"] == "/projects/repo/.git" && saved["revision"] == 9);
    assert(saved.dump().size() <= 4096);
    auto completed = saved;
    completed["state"] = "complete";
    completed["code_head"] = "";
    json stop{{"session_id", "s"}, {"project_dir", "/nonexistent"}};
    const auto kept = prepare_capsule(stop, json::object(), json::object(), completed, 101);
    assert(kept["args"]["expected_revision"] == 9);
    assert(kept["args"]["metadata"]["handoff"]["state"] == "complete");
    assert(kept["args"]["metadata"]["handoff"]["gates"] == saved["gates"]);
    stop["artifact_paths"] = json::array({"modified.cpp"});
    const auto invalid = prepare_capsule(stop, json::object(), json::object(), completed, 102);
    assert(invalid["args"]["metadata"]["handoff"]["state"] == "invalidated");
    assert(invalid["args"]["metadata"]["handoff"]["gates"].empty());
    auto alias = saved;
    alias["repository"] = "/maps/projects/repo/.git";
    assert(capsule_key(alias) == capsule_key(saved));
    alias["stream_id"] = "another";
    assert(capsule_key(alias) != capsule_key(saved));
    for (const auto& state : {"complete", "in_progress", "missing", "invalidated"}) {
        v2["state"] = state;
        assert(capsule_v2(v2, 10, 101)["state"] == state);
    }
    auto rejected = [&](json value) {
        try { capsule_v2(value, 10, 101); return false; }
        catch (const std::exception&) { return true; }
    };
    v2["state"] = "invented";
    assert(rejected(v2));
    v2["state"] = "in_progress";
    v2["dirty_paths"] = std::vector<std::string>(21, "a");
    assert(rejected(v2));
    v2["dirty_paths"] = std::vector<std::string>(20, std::string(128, '\n'));
    assert(rejected(v2)); // Escaping is included in the serialized byte bound.
    v2["dirty_paths"] = json::array();
    v2["jobs"] = {{{"id", "job"}, {"result_path", "/scratch/result"}}};
    assert(!rejected(v2));
    v2["gates"]["quick"]["status"] = "unknown";
    assert(rejected(v2));
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
    const auto progress = stop_progress({{"session_id", "s"}, {"last_user", "Fix it"},
        {"response", "Error fixed; tests passed.\nNext: verify the checkpoint"}, {"has_error", true}});
    assert(progress["writes"].size() == 2);
    assert(progress["writes"][0]["snapshot"] == "Goal: Fix it\\nNext: Next: verify the checkpoint");
    assert(progress["writes"][1]["mood"] == "confident");
    assert(progress["diagnostics"] == "[ledger] error checkpoint triggered\n[ledger] milestone checkpoint triggered\n");
    const auto checkpoint = stop_checkpoint({{"session_id", "s"}, {"realm", "project:test"},
        {"response", "Visible text. [DECISION] Preserve queue acknowledgement.\n[BLOCKER] Waiting\n[GOTCHA] Keep this"},
        {"turn_index", 1}, {"snapshot", {{"files", {"a.cpp"}}, {"tools", {"Read", "Bash", "Edit"}},
            {"counts", {{"assistant", 1}}}}}});
    assert(checkpoint["ledger"]["decisions"] == json::array({"Preserve queue acknowledgement."}));
    assert(checkpoint["ledger"]["blockers"] == json::array({"Waiting"}));
    assert(checkpoint["ledger"]["discoveries"] == json::array({"[GOTCHA] Keep this"}));
    assert(checkpoint["summary"] == "");
    assert(checkpoint["summary_log"] == "[soul] skip session-summary: too few turns (1<3)");
    assert(checkpoint["ledger_log"] == "[ledger] queued: s (working, files=1 decisions=1 todos=0)");
    const auto summary = stop_checkpoint({{"session_id", "s"}, {"response", "Done."},
        {"turn_index", 7}, {"snapshot", {{"tools", {"Read", "Bash", "Edit"}}}}});
    assert(summary["summary"] == "[session:s] confident→4 turns | tools: Read,Bash Edit");
    assert(stop_checkpoint({{"response", "[DECISION] \n[BLOCKER]"}})["ledger"]["decisions"].empty());
    assert(stop_checkpoint({{"response", std::string(999, 'x') + "αβ"}})["ledger"]["snapshot"]
        == std::string(999, 'x') + "�");
    std::cout << "hook_ledger_policy_test: passed\n";
}
