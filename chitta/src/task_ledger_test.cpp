#include <chitta/task_ledger.hpp>
#include <cassert>
#include <iostream>
#include <atomic>
#include <thread>

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
    // Simultaneous claims must publish one binding and lease, with unique revisions.
    chitta::TaskLedger concurrent;
    std::vector<json> journal; // deliberately protected only by the ledger transaction
    auto append = [&](const json& batch) {
        std::this_thread::yield();
        journal.push_back(batch);
    };
    concurrent.run("thread_create", {{"id", "race"}, {"title", "race"}}, append);
    std::atomic<int> ready{0}, winners{0};
    std::atomic<bool> go{false};
    std::vector<std::thread> clients;
    for (int i = 0; i < 24; ++i) clients.emplace_back([&, i] {
        ++ready;
        while (!go.load()) std::this_thread::yield();
        const auto sid = "s" + std::to_string(i);
        concurrent.run("session_bind", {{"session_id", sid}}, append);
        auto claim = concurrent.run("lease_claim", {{"thread_id", "race"}, {"session_id", sid}}, append);
        if (claim.at("claimed").get<bool>()) ++winners;
        for (int j = 0; j < 20; ++j) {
            concurrent.run("thread_create", {{"id", sid + "/" + std::to_string(j)}, {"title", "parallel"}}, append);
            auto leases = concurrent.run("lease_list", {{"active_only", false}}, append);
            assert(leases.at("rows").size() == 1);
        }
    });
    while (ready.load() != 24) std::this_thread::yield();
    go = true;
    for (auto& client : clients) client.join();
    assert(winners == 1);
    chitta::TaskLedger replayed;
    uint64_t revision = 0;
    for (const auto& batch : journal) {
        assert(batch.at("revision").get<uint64_t>() == ++revision);
        replayed.replay(batch);
    }
    for (const auto* op : {"counts", "lease_list", "session_list"}) {
        assert(concurrent.run(op, json::object(), append) == replayed.run(op, json::object(), append));
    }
    std::cout << "ledger: atomic publish, invalid import rejection, idempotence, indexes, lease "
                 "batch replay passed\n";
}
