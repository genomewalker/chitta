#include <chitta/task_ledger.hpp>
#include <cassert>
#include <iostream>

int main() {
    using json = nlohmann::json;
    chitta::TaskLedger ledger;
    std::vector<json> events;
    auto persist = [&](const json& batch) { events.push_back(batch); };
    auto run     = [&](const std::string& op, const json& args) {
        return ledger.run(op, args, persist);
    };
    assert(run("thread_create", {{"id", "t"}, {"title", "work"}, {"realm", "a"}}) == "t");
    auto row     = run("thread_get", {{"thread_id", "t"}});
    size_t count = events.size();
    try {
        ledger.run("thread_update", {{"thread_id", "t"}, {"fields", {{"realm", "b"}}}},
                   [](const json&) { throw std::runtime_error("WAL unavailable"); });
        assert(false);
    } catch (const std::runtime_error&) {}
    assert(run("thread_get", {{"thread_id", "t"}}) == row);
    assert(events.size() == count);
    try {
        run("import", {{"table", "threads"}, {"row", {{"thread_id", "invalid"}}}});
        assert(false);
    } catch (const std::invalid_argument&) {}
    assert(events.size() == count);
    assert(run("import", {{"table", "threads"}, {"row", row}}) == false);
    assert(events.size() == count);
    assert(run("thread_update", {{"thread_id", "t"}, {"fields", {{"realm", "b"}}}}) == true);
    assert(run("thread_list", {{"realm", "a"}})["rows"].empty());
    assert(run("thread_list", {{"realm", "b"}})["rows"].size() == 1);
    assert(run("session_bind", {{"session_id", "s"}, {"thread_id", "t"}}) == true);
    assert(run("lease_claim", {{"thread_id", "t"}, {"session_id", "s"}})["claimed"] == true);
    assert(events.back()["changes"].size() == 2);
    assert(run("session_close", {{"session_id", "s"}}) == true);
    assert(events.back()["changes"].size() == 2);
    assert(run("lease_list", {{"active_only", false}})["rows"].empty());
    chitta::TaskLedger recovered;
    for (const auto& event : events)
        recovered.replay(event);
    assert(recovered.run("session_get", {{"session_id", "s"}}, persist) ==
           run("session_get", {{"session_id", "s"}}));
    assert(recovered.run("lease_list", {{"active_only", false}}, persist)["rows"].empty());
    std::cout << "ledger: atomic publish, invalid import rejection, idempotence, indexes, lease "
                 "batch replay passed\n";
}
