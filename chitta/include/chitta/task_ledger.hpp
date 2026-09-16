#pragma once
#include <nlohmann/json.hpp>
#include <algorithm>
#include <chrono>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>
#include <fnmatch.h>

namespace chitta {
// Owns synchronization independently of RPC dispatch and queue/background callers.
// Lock order: transaction mutex, then store locks inside the persistence callback.
// Persist must not reenter this ledger. Reads return copies under the same mutex.
// Persist a whole mutation before publishing any rows: lease claims never expose
// a binding without its lease, or a close without its lease deletions.
class TaskLedger {
public:
    using json    = nlohmann::json;
    using Persist = std::function<void(const json&)>;
    struct Table {
        std::string key;
        std::map<std::string, json> rows;
        std::map<std::string, std::map<std::string, std::set<std::string>>> indexes;
        Table(std::string k, std::initializer_list<std::string> fields) : key(std::move(k)) {
            for (const auto& f : fields)
                indexes[f];
        }
        void put(const std::string& id, const json& row) {
            auto old = rows.find(id);
            for (auto& [field, index] : indexes) {
                if (old != rows.end()) {
                    auto value = old->second.at(field).dump();
                    auto it    = index.find(value);
                    if (it != index.end() && (it->second.erase(id), it->second.empty()))
                        index.erase(it);
                }
                if (!row.is_null()) index[row.at(field).dump()].insert(id);
            }
            if (row.is_null())
                rows.erase(id);
            else
                rows[id] = row;
        }
        json get(const std::string& id) const {
            auto it = rows.find(id);
            return it == rows.end() ? json(nullptr) : it->second;
        }
        std::vector<json> select(const json& filters) const {
            const std::set<std::string>* candidates = nullptr;
            for (const auto& [field, value] : filters.items()) {
                auto idx = indexes.find(field);
                if (idx == indexes.end()) continue;
                auto found = idx->second.find(value.dump());
                if (found == idx->second.end()) return {};
                if (!candidates || found->second.size() < candidates->size())
                    candidates = &found->second;
            }
            std::vector<json> out;
            auto add = [&](const json& row) {
                for (const auto& [f, v] : filters.items())
                    if (row.at(f) != v) return;
                out.push_back(row);
            };
            if (candidates)
                for (const auto& id : *candidates)
                    add(rows.at(id));
            else
                for (const auto& [id, row] : rows)
                    add(row);
            return out;
        }
    };
private:
    std::mutex transaction_mutex_;
    std::map<std::string, Table> tables{
        {"threads", {"thread_id", {"realm", "status"}}},
        {"thread_sessions", {"session_id", {"thread_id", "project_dir", "status"}}},
        {"thread_leases", {"thread_id", {"session_id"}}},
        {"inbox", {"item_id", {"delivery_state", "target_realm"}}},
        {"artifacts", {"artifact_id", {"task_id", "thread_id"}}}};
    void validate_row(const std::string& table, const std::string& id, const json& row) const {
        if (row.is_null()) return;
        static const std::map<std::string, std::set<std::string>> keys = {
            {"threads",
             {"thread_id", "title", "realm", "status", "topic_fingerprint", "created_at",
              "last_active_at", "sealed_at", "parent_thread_id", "metadata_json"}},
            {"thread_sessions",
             {"session_id", "thread_id", "client", "project_dir", "transcript_path", "status",
              "started_at", "last_active_at", "ended_at", "metadata_json"}},
            {"thread_leases",
             {"thread_id", "session_id", "generation", "acquired_at", "last_heartbeat_at",
              "expires_at"}},
            {"inbox",
             {"item_id", "task_id", "thread_id", "event_type", "digest", "payload_json",
              "target_realm", "created_at", "delivery_state", "delivered_at", "acked_at"}},
            {"artifacts",
             {"artifact_id", "task_id", "thread_id", "path", "kind", "mtime", "size", "md5",
              "created_at", "parent_artifact_id", "metadata_json"}}};
        if (!row.is_object() || row.size() != keys.at(table).size())
            throw std::invalid_argument("invalid ledger row keys");
        static const std::set<std::string> numeric = {
            "created_at",        "last_active_at", "sealed_at",
            "started_at",        "ended_at",       "acquired_at",
            "last_heartbeat_at", "expires_at",     "delivered_at",
            "acked_at",          "mtime",          "size",
            "generation"};
        static const std::set<std::string> nullable = {
            "thread_id",         "topic_fingerprint", "sealed_at", "ended_at", "parent_thread_id",
            "delivered_at",      "acked_at",          "mtime",     "size",     "md5",
            "parent_artifact_id"};
        for (const auto& key : keys.at(table)) {
            if (!row.contains(key)) throw std::invalid_argument("missing ledger field: " + key);
            const auto& value = row.at(key);
            if (nullable.count(key) && value.is_null()) continue;
            if (numeric.count(key) ? !value.is_number() : !value.is_string())
                throw std::invalid_argument("invalid ledger field type: " + key);
        }
        if (row.at(tables.at(table).key) != id)
            throw std::invalid_argument("ledger primary key mismatch");
        if (row.contains("status")) {
            const auto status = row.at("status").get<std::string>();
            const std::set<std::string> allowed =
                table == "threads"
                    ? std::set<std::string>{"active", "sealed", "dormant"}
                    : std::set<std::string>{"active", "ended", "interrupted", "completed"};
            if (!allowed.count(status)) throw std::invalid_argument("invalid ledger status");
        }
        if (row.contains("delivery_state") &&
            !std::set<std::string>{"pending", "delivered", "acked", "suppressed"}.count(
                row.at("delivery_state")))
            throw std::invalid_argument("invalid inbox state");
    }
public:
    static bool is_read(const std::string& op) {
        static const std::set<std::string> reads = {
            "thread_get", "thread_list",   "session_get",      "session_list", "lease_list",
            "inbox_list", "artifact_list", "artifact_lineage", "counts",
            // Hook assembly returns queue payloads/cards without mutating state.
            // Classify explicitly: unknown operations (including hook_turn) must
            // retain the write dispatch and durable sync boundary.
            "hook_handoff_prepare", "hook_handoff_context", "hook_task_context",
            "hook_session_context", "hook_stop_checkpoint", "hook_stop_progress"};
        return reads.count(op) != 0;
    }
private:
    uint64_t revision = 0;
    void replay_locked(const json& batch) {
        for (const auto& c : batch.at("changes"))
            tables.at(c.at("table").get<std::string>()).put(c.at("id"), c.at("row"));
        revision = std::max(revision, batch.at("revision").get<uint64_t>());
    }
public:
    void replay(const json& batch) {
        std::lock_guard<std::mutex> lock(transaction_mutex_);
        replay_locked(batch);
    }
    static double now() {
        return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch())
            .count();
    }
    json run(const std::string& op, const json& a, const Persist& persist) {
        std::lock_guard<std::mutex> lock(transaction_mutex_);
        const double ts = now();
        json changes    = json::array();
        auto change     = [&](const std::string& table, const std::string& id, const json& row) {
            changes.push_back({{"table", table}, {"id", id}, {"row", row}});
        };
        auto finish = [&](json result) {
            if (!changes.empty()) {
                json batch = {{"revision", revision + 1}, {"changes", changes}};
                for (const auto& c : changes)
                    validate_row(c["table"], c["id"], c["row"]);
                persist(batch);
                replay_locked(batch);
            }
            return result;
        };
        auto idarg   = [&](const char* name) { return a.at(name).get<std::string>(); };
        auto require = [&](const std::string& table, const json& id) {
            if (!id.is_null() && tables.at(table).get(id.get<std::string>()).is_null())
                throw std::invalid_argument("unknown " + table + " reference");
        };
        auto valid = [](const std::string& value, std::initializer_list<const char*> allowed) {
            for (auto s : allowed)
                if (value == s) return;
            throw std::invalid_argument("invalid ledger state");
        };
        auto touch = [&](const std::string& sid, bool close, const std::string& status) {
            auto row = tables.at("thread_sessions").get(sid);
            if (!row.is_null()) {
                row["last_active_at"] = ts;
                row["status"]         = status;
                row["ended_at"]       = close ? json(ts) : json(nullptr);
                change("thread_sessions", sid, row);
            }
            for (auto lease : tables.at("thread_leases").select({{"session_id", sid}})) {
                if (close)
                    change("thread_leases", lease["thread_id"], nullptr);
                else {
                    lease["last_heartbeat_at"] = ts;
                    lease["expires_at"]        = ts + 900;
                    change("thread_leases", lease["thread_id"], lease);
                }
            }
            return !row.is_null();
        };
        if (op == "thread_create") {
            auto id = idarg("id");
            require("threads", a.value("parent", json(nullptr)));
            if (!tables.at("threads").get(id).is_null()) return id;
            change("threads", id,
                   {{"thread_id", id},
                    {"title", a.at("title")},
                    {"realm", a.value("realm", "")},
                    {"status", "active"},
                    {"topic_fingerprint", a.value("fingerprint", json(nullptr))},
                    {"created_at", ts},
                    {"last_active_at", ts},
                    {"sealed_at", nullptr},
                    {"parent_thread_id", a.value("parent", json(nullptr))},
                    {"metadata_json", "{}"}});
            return finish(id);
        }
        if (op == "thread_get" || op == "session_get")
            return tables.at(op == "thread_get" ? "threads" : "thread_sessions")
                .get(idarg(op == "thread_get" ? "thread_id" : "session_id"));
        if (op == "thread_update" || op == "thread_seal") {
            auto id  = idarg("thread_id");
            auto row = tables.at("threads").get(id);
            if (row.is_null()) return false;
            if (op == "thread_seal") {
                row["status"]         = "sealed";
                row["sealed_at"]      = ts;
                row["last_active_at"] = ts;
            } else {
                bool any = false;
                for (auto key : {"title", "realm", "status", "topic_fingerprint", "last_active_at",
                                 "metadata_json", "parent_thread_id"})
                    if (a.at("fields").contains(key)) {
                        row[key] = a["fields"][key];
                        any      = true;
                    }
                if (!any) return false;
            }
            valid(row.at("status"), {"active", "sealed", "dormant"});
            require("threads", row.at("parent_thread_id"));
            change("threads", id, row);
            return finish(true);
        }
        if (op == "session_bind") {
            auto id = idarg("session_id");
            if (id.empty()) return false;
            auto row = tables.at("thread_sessions").get(id);
            if (row.is_null())
                row = {{"session_id", id},     {"thread_id", nullptr},  {"client", ""},
                       {"project_dir", ""},    {"transcript_path", ""}, {"status", "active"},
                       {"started_at", ts},     {"last_active_at", ts},  {"ended_at", nullptr},
                       {"metadata_json", "{}"}};
            for (auto key : {"client", "project_dir", "transcript_path"})
                if (!a.value(key, "").empty()) row[key] = a[key];
            if (a.contains("thread_id") && !a["thread_id"].is_null()) {
                require("threads", a["thread_id"]);
                row["thread_id"] = a["thread_id"];
            }
            auto metadata = json::parse(row["metadata_json"].get<std::string>(), nullptr, false);
            if (!metadata.is_object()) metadata = json::object();
            if (a.contains("metadata") && a["metadata"].is_object()) metadata.update(a["metadata"]);
            row["metadata_json"] = metadata.dump();
            row["status"]        = a.value("status", "active");
            valid(row["status"], {"active", "ended", "interrupted", "completed"});
            row["last_active_at"] = ts;
            row["ended_at"]       = nullptr;
            change("thread_sessions", id, row);
            return finish(true);
        }
        if (op == "session_touch" || op == "session_close") {
            auto status = a.value("status", "ended");
            if (status != "ended" && status != "interrupted" && status != "completed")
                status = "ended";
            return finish(touch(idarg("session_id"), op == "session_close",
                                op == "session_close" ? status : "active"));
        }
        if (op == "lease_claim") {
            auto tid = idarg("thread_id"), sid = idarg("session_id");
            require("threads", tid);
            require("thread_sessions", sid);
            auto old = tables.at("thread_leases").get(tid);
            if (!old.is_null() && old["session_id"] != sid &&
                old["expires_at"].get<double>() > ts && !a.value("force", false))
                return {{"claimed", false},
                        {"reason", "live_owner"},
                        {"thread_id", tid},
                        {"owner_session_id", old["session_id"]},
                        {"expires_at", old["expires_at"]},
                        {"generation", old["generation"]}};
            bool same      = !old.is_null() && old["session_id"] == sid;
            int gen        = old.is_null() ? 1 : old["generation"].get<int>() + (same ? 0 : 1);
            double expires = ts + std::max(30, a.value("ttl", 900));
            change("thread_leases", tid,
                   {{"thread_id", tid},
                    {"session_id", sid},
                    {"generation", gen},
                    {"acquired_at", same ? old["acquired_at"] : json(ts)},
                    {"last_heartbeat_at", ts},
                    {"expires_at", expires}});
            auto row              = tables.at("thread_sessions").get(sid);
            row["thread_id"]      = tid;
            row["status"]         = "active";
            row["last_active_at"] = ts;
            change("thread_sessions", sid, row);
            return finish({{"claimed", true},
                           {"thread_id", tid},
                           {"session_id", sid},
                           {"expires_at", expires},
                           {"generation", gen}});
        }
        if (op == "lease_release") {
            for (const auto& row :
                 tables.at("thread_leases").select({{"session_id", idarg("session_id")}}))
                if (!a.contains("thread_id") || a["thread_id"].is_null() || a["thread_id"] == "" ||
                    row["thread_id"] == a["thread_id"])
                    change("thread_leases", row["thread_id"], nullptr);
            return finish(!changes.empty());
        }
        if (op == "inbox_push") {
            auto id = idarg("id");
            require("threads", a.value("thread_id", json(nullptr)));
            if (!tables.at("inbox").get(id).is_null()) return id;
            change("inbox", id,
                   {{"item_id", id},
                    {"task_id", a.value("task_id", "")},
                    {"thread_id", a.value("thread_id", json(nullptr))},
                    {"event_type", a.at("event_type")},
                    {"digest", a.value("digest", "")},
                    {"payload_json", a.value("payload", json::object()).is_null()
                                         ? "{}"
                                         : a.value("payload", json::object()).dump()},
                    {"target_realm", a.value("target_realm", "")},
                    {"created_at", ts},
                    {"delivery_state", "pending"},
                    {"delivered_at", nullptr},
                    {"acked_at", nullptr}});
            return finish(id);
        }
        if (op == "inbox_ack") {
            auto id  = idarg("item_id");
            auto row = tables.at("inbox").get(id);
            if (row.is_null()) return false;
            auto state = a.value("new_state", "acked");
            valid(state, {"pending", "delivered", "acked", "suppressed"});
            row["delivery_state"]                               = state;
            row[state == "acked" ? "acked_at" : "delivered_at"] = ts;
            change("inbox", id, row);
            return finish(true);
        }
        if (op == "artifact_register") {
            auto id = idarg("id");
            require("threads", a.value("thread_id", json(nullptr)));
            require("artifacts", a.value("parent_artifact_id", json(nullptr)));
            if (!tables.at("artifacts").get(id).is_null()) return id;
            json row = {{"artifact_id", id},
                        {"task_id", a.value("task_id", "")},
                        {"path", a.at("path")},
                        {"kind", a.value("kind", "file")},
                        {"thread_id", a.value("thread_id", json(nullptr))},
                        {"parent_artifact_id", a.value("parent_artifact_id", json(nullptr))},
                        {"mtime", a.value("mtime", json(nullptr))},
                        {"size", a.value("size", json(nullptr))},
                        {"md5", a.value("md5", json(nullptr))},
                        {"created_at", ts},
                        {"metadata_json", "{}"}};
            change("artifacts", id, row);
            return finish(id);
        }
        if (op == "artifact_link") {
            auto id  = idarg("child_id");
            auto row = tables.at("artifacts").get(id);
            if (row.is_null()) return false;
            require("artifacts", a.at("parent_id"));
            row["parent_artifact_id"] = a["parent_id"];
            change("artifacts", id, row);
            return finish(true);
        }
        if (op == "artifact_lineage") {
            json rows = json::array();
            auto id   = idarg("artifact_id");
            std::set<std::string> seen;
            // Each RPC page is bounded. The client follows next_id and keeps a
            // cross-page cycle guard so legacy unlimited lineage stays compatible.
            while (!id.empty() && seen.insert(id).second && rows.size() < 100) {
                auto row = tables.at("artifacts").get(id);
                if (row.is_null()) {
                    id.clear();
                    break;
                }
                rows.push_back(row);
                id = row["parent_artifact_id"].is_null()
                         ? ""
                         : row["parent_artifact_id"].get<std::string>();
            }
            return {{"rows", rows}, {"next_id", id}};
        }
        if (op == "import") {
            auto table = idarg("table");
            auto& t    = tables.at(table);
            auto row   = a.at("row");
            auto id    = row.at(t.key).get<std::string>();
            if (!t.get(id).is_null()) return false; // Never roll back newer daemon state on rerun.
            // Imports preserve nullable fields/metadata/timestamps exactly. Foreign
            // references may arrive later in the same source database.
            change(table, id, row);
            return finish(true);
        }
        if (op == "counts") {
            json out = json::object();
            for (auto& [name, t] : tables)
                out[name] = t.rows.size();
            return out;
        }
        const std::map<std::string, std::string> lists = {{"thread_list", "threads"},
                                                          {"session_list", "thread_sessions"},
                                                          {"lease_list", "thread_leases"},
                                                          {"inbox_list", "inbox"},
                                                          {"artifact_list", "artifacts"}};
        auto entry                                     = lists.find(op);
        if (entry == lists.end()) throw std::invalid_argument("unknown ledger op: " + op);
        auto& table  = tables.at(entry->second);
        json filters = json::object();
        for (const auto& [field, index] : table.indexes)
            if (a.contains(field) && !a[field].is_null() &&
                !(op == "artifact_list" && a[field] == ""))
                filters[field] = a[field];
        if (op == "inbox_list") {
            filters.erase("target_realm");
            filters["delivery_state"] = a.value("state", "pending");
        }
        std::vector<json> rows;
        if (!(op == "inbox_list" && a.contains("target_realm") && !a["target_realm"].is_null()))
            rows = table.select(filters);
        if (op == "inbox_list" && a.contains("target_realm") && !a["target_realm"].is_null()) {
            filters["target_realm"] = a["target_realm"];
            rows                    = table.select(filters);
            if (a["target_realm"] != "") {
                filters["target_realm"] = "";
                auto global             = table.select(filters);
                rows.insert(rows.end(), global.begin(), global.end());
            }
        }
        rows.erase(std::remove_if(rows.begin(), rows.end(),
                                  [&](const json& row) {
                                      if (op == "lease_list" && a.value("active_only", true) &&
                                          row["expires_at"].get<double>() <= ts)
                                          return true;
                                      if (op == "inbox_list" && a.contains("target_realm") &&
                                          !a["target_realm"].is_null() &&
                                          row["target_realm"] != "" &&
                                          row["target_realm"] != a["target_realm"])
                                          return true;
                                      if (op == "artifact_list" && a.contains("path_glob") &&
                                          !a["path_glob"].is_null() && a["path_glob"] != "")
                                          return fnmatch(a["path_glob"].get<std::string>().c_str(),
                                                         row["path"].get<std::string>().c_str(),
                                                         FNM_NOESCAPE) != 0;
                                      return false;
                                  }),
                   rows.end());
        std::string order = (op == "thread_list" || op == "session_list") ? "last_active_at"
                            : op == "lease_list"                          ? "last_heartbeat_at"
                                                                          : "created_at";
        auto less         = [&](const json& x, const json& y) {
            return x[order] != y[order] ? x[order] > y[order] : x[table.key] < y[table.key];
        };
        if (a.contains("after") && !a["after"].is_null()) {
            auto after = a["after"];
            rows.erase(std::remove_if(rows.begin(), rows.end(),
                                      [&](const json& row) { return !less(after, row); }),
                       rows.end());
        }
        size_t n =
            std::min(rows.size(), static_cast<size_t>(std::clamp(a.value("limit", 100), 1, 100)));
        std::partial_sort(rows.begin(), rows.begin() + n, rows.end(), less);
        bool more = rows.size() > n;
        rows.resize(n);
        json cursor = nullptr;
        if (more && n) cursor = {{order, rows.back()[order]}, {table.key, rows.back()[table.key]}};
        return {{"rows", rows}, {"after", cursor}};
    }
};
} // namespace chitta
