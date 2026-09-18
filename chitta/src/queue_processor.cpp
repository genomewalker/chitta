#include <chitta/queue_processor.hpp>
#include <chitta/maintenance_jitter.hpp>
#include <chitta/rpc/field_handler.hpp>
#include <chitta/rpc/sandbox.hpp>
#include <chitta/vak.hpp>
#include <chitta/code_intel.hpp>
#include <chitta/llm_http.hpp>
#include <filesystem>
#include <iostream>
#include <chrono>
#include <thread>
#include <unordered_set>
#include <stdexcept>
#include <fcntl.h>
#include <unistd.h>

extern std::atomic<bool> daemon_running;
extern std::atomic<bool> verbose_mode;

using json = nlohmann::json;

namespace chitta {

QueueProcessor::QueueProcessor(FieldStore& field_store,
                               VakYantra* yantra,
                               const DistillConfig& distill_config,
                               FieldRpcHandler& handler,
                               const std::string& queue_path,
                               const std::string& failed_queue_path,
                               std::atomic<size_t>& queue_count,
                               std::atomic<size_t>& queue_distill_count,
                               std::atomic<size_t>& queue_fail_count)
    : field_store_(field_store)
    , yantra_(yantra)
    , distill_config_(distill_config)
    , handler_(handler)
    , queue_path_(queue_path)
    , failed_queue_path_(failed_queue_path)
    , queue_count_(queue_count)
    , queue_distill_count_(queue_distill_count)
    , queue_fail_count_(queue_fail_count)
    , slow_path_(queue_path + ".slow")
    , attempts_path_(queue_path + ".slow.attempts")
{}

// A triplet is worth far more to the analogy lane when it names the memory it
// came from: chitta-field's analogy_snapshot() keys the structural signature
// index on `source_memory_id`, and a fact carrying none contributes no
// hypervector. The `connect` and `store_claim` queue schemas have no id field,
// but callers routinely put one in the payload, and both tools are also used
// with a memory id AS a node (the convention native_distiller.cpp and
// ingester.cpp already follow). Returns the first id that both parses and
// resolves to a live memory, or 0 — which is exactly what add_triplet's
// default 5th argument already means (unattributed).
static uint64_t triplet_source_memory_id(FieldStore& fs, const json& args) {
    auto as_id = [](const json& v) -> uint64_t {
        if (v.is_number_unsigned()) return v.get<uint64_t>();
        if (v.is_string()) {
            const std::string& s = v.get_ref<const std::string&>();
            if (!s.empty() && s.find_first_not_of("0123456789") == std::string::npos) {
                try { return std::stoull(s); } catch (...) {}
            }
        }
        return 0;
    };
    // Explicit attribution wins and is trusted without a lookup: the caller
    // named the memory this fact came from, and that memory need not itself be
    // one of the triplet's nodes.
    for (const char* k : {"source_memory_id", "memory_id", "memory-id"}) {
        auto it = args.find(k);
        if (it != args.end()) {
            uint64_t id = as_id(*it);
            if (id) return id;
        }
    }
    // Otherwise a node may itself BE a memory. Only accept one that resolves to
    // stored content, so an entity that merely looks numeric ("2024") is never
    // misattributed. Subject first — it is the side both existing conventions
    // use for the source memory.
    for (const char* k : {"subject", "object"}) {
        auto it = args.find(k);
        uint64_t id = (it != args.end()) ? as_id(*it) : 0;
        if (id && !fs.get_content(id).empty()) return id;
    }
    return 0;
}

// POISON-PILL: durable per-session distill attempt counter. Bump persists the new
// count before the distill runs, so a crash mid-distill leaves it incremented and
// the next restart sees the elevated count. ceiling: rename is atomic but not
// fsync'd, so a hard power-loss between write and the crash may lose one increment
// (worst case: one extra retry). Single slow thread → no concurrent access.
int QueueProcessor::bump_distill_attempt(const std::string& session_id) {
    json m = json::object();
    { std::ifstream in(attempts_path_); if (in.good()) { try { in >> m; } catch (...) { m = json::object(); } } }
    int n = m.value(session_id, 0) + 1;
    m[session_id] = n;
    std::string tmp = attempts_path_ + ".tmp";
    { std::ofstream out(tmp, std::ios::trunc); out << m.dump(); }
    std::rename(tmp.c_str(), attempts_path_.c_str());
    return n;
}

void QueueProcessor::clear_distill_attempt(const std::string& session_id) {
    json m = json::object();
    { std::ifstream in(attempts_path_); if (in.good()) { try { in >> m; } catch (...) { return; } } }
    if (!m.contains(session_id)) return;
    m.erase(session_id);
    std::string tmp = attempts_path_ + ".tmp";
    { std::ofstream out(tmp, std::ios::trunc); out << m.dump(); }
    std::rename(tmp.c_str(), attempts_path_.c_str());
}

namespace {
// One ledger per lane, owned by its single consumer. Retain the completed
// batch until the next claim so a stale .processing replay after snapshot/WAL
// compaction is still recognized. Rotation retains only IDs in the new batch.
struct QueueAckError : std::runtime_error { using std::runtime_error::runtime_error; };

void ack_sync_path(const std::string& path) {
    int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) throw QueueAckError("open for sync: " + path);
    int rc = ::fsync(fd);
    ::close(fd);
    if (rc != 0) throw QueueAckError("fsync: " + path);
}

void ack_replace(const std::string& path, const std::string& contents) {
    const auto tmp = path + ".tmp";
    {
        std::ofstream out(tmp, std::ios::trunc);
        out << contents;
        out.flush();
        if (!out) throw QueueAckError("write: " + tmp);
    }
    ack_sync_path(tmp);
    if (std::rename(tmp.c_str(), path.c_str()) != 0)
        throw QueueAckError("rename: " + path);
    auto parent = std::filesystem::path(path).parent_path();
    ack_sync_path(parent.empty() ? "." : parent.string());
}

std::string queue_ack_id(const std::string& line) {
    const auto item = json::parse(line, nullptr, false);
    if (!item.is_object()) return {};
    auto it = item.find("ack_id");
    return it != item.end() && it->is_string() ? it->get<std::string>() : "";
}

class AppliedAcks {
    std::string path_;
    std::unordered_set<std::string> ids_;
public:
    explicit AppliedAcks(const std::string& queue) : path_(queue + ".applied-acks") {
        std::error_code ec;
        const bool exists = std::filesystem::exists(path_, ec);
        if (ec) throw QueueAckError("stat applied ack ledger " + path_ + ": " + ec.message());
        if (!exists) return;
        std::ifstream in(path_);
        try {
            json ids;
            in >> ids;
            for (const auto& id : ids.get<std::vector<std::string>>()) ids_.insert(id);
        } catch (const std::exception& e) {
            throw QueueAckError("unreadable applied ack ledger " + path_ + ": " + e.what());
        }
    }
    bool contains(const std::string& id) const { return !id.empty() && ids_.count(id); }
    void persist() { ack_replace(path_, json(ids_).dump() + "\n"); }
    void rotate(const std::vector<std::string>& lines) {
        std::unordered_set<std::string> retained;
        for (const auto& line : lines) {
            auto id = queue_ack_id(line);
            if (contains(id)) retained.insert(id);
        }
        if (ids_ == retained) return;
        ids_ = std::move(retained);
        persist();
    }
    void record(const std::string& id, FieldStore& store) {
        if (id.empty() || contains(id)) return;
        // The C++ convenience sync() discards the FFI status. Never publish a
        // durable ack for a failed WAL sync. Failure leaves .processing intact.
        if (cf_sync(store.handle()) != 0) throw QueueAckError("store sync before ack " + id);
        ids_.insert(id);
        persist();
    }
};
} // namespace

// Crash recovery for one queue file: if its .processing sidecar exists,
// re-queue only the UNPROCESSED suffix. The batch loops checkpoint their
// progress to a .ckpt sidecar after each field_store_.sync(), so every line
// before the watermark is durably applied — re-appending it would duplicate
// data. With no checkpoint, durable ack IDs still filter the replay.
void QueueProcessor::recover_processing(const std::string& queue_file) {
    std::string processing_path = queue_file + ".processing";
    if (!std::ifstream(processing_path).good()) return;
    std::string ckpt_path = processing_path + ".ckpt";
    size_t skip = 0;
    {
        std::ifstream ck(ckpt_path);
        if (ck.good()) ck >> skip;  // stays 0 on parse failure
    }
    // Append the unprocessed suffix to any existing queue file. Count
    // non-empty lines only — the batch loop's watermark indexes the same
    // filtered sequence.
    AppliedAcks applied(queue_file);
    size_t requeued = 0, acknowledged = 0;
    {
        std::ifstream src(processing_path);
        std::ofstream dst(queue_file, std::ios::app);
        std::string line;
        size_t idx = 0;
        while (std::getline(src, line)) {
            if (line.empty()) continue;
            if (idx++ < skip) continue;
            if (applied.contains(queue_ack_id(line))) { ++acknowledged; continue; }
            dst << line << "\n";
            ++requeued;
        }
        dst.flush();
        if (!src.eof() || !dst) throw QueueAckError("requeue failed: " + processing_path);
    }
    ack_sync_path(queue_file);
    auto parent = std::filesystem::path(queue_file).parent_path();
    ack_sync_path(parent.empty() ? "." : parent.string());
    std::remove(processing_path.c_str());
    std::remove(ckpt_path.c_str());
    std::cerr << "[queue] crash recovery: re-queued " << requeued
              << " items from " << processing_path
              << " (skipped " << skip << " checkpointed, " << acknowledged << " applied ack_ids)\n";
}

void QueueProcessor::start() {
    const auto parent = std::filesystem::path(queue_path_).parent_path();
    if (!parent.empty()) std::filesystem::create_directories(parent);
    recover_processing(queue_path_);
    recover_processing(slow_path_);
    thread_ = std::thread([this]() { run(); });
    slow_thread_ = std::thread([this]() { run_slow(); });
}

// Persist the batch watermark: number of items of the current .processing
// batch that are durably applied (call only after field_store_.sync()).
// Write-then-rename so a crash mid-write can't leave a truncated count.
void QueueProcessor::write_checkpoint(const std::string& ckpt_path, size_t processed) {
    std::string tmp = ckpt_path + ".tmp";
    {
        std::ofstream out(tmp, std::ios::trunc);
        out << processed << "\n";
    }
    std::rename(tmp.c_str(), ckpt_path.c_str());
}

void QueueProcessor::stop() {
    if (thread_.joinable()) thread_.join();
    if (slow_thread_.joinable()) slow_thread_.join();
}

float QueueProcessor::category_to_confidence(const std::string& category) {
    if (category == "correction") return 0.95f;
    if (category == "belief")     return 0.90f;
    if (category == "preference") return 0.90f;
    if (category == "solution")   return 0.90f;
    if (category == "milestone")  return 0.90f;
    if (category == "decision")   return 0.85f;
    if (category == "failure")    return 0.85f;
    if (category == "gotcha")     return 0.85f;
    if (category == "episode")    return 0.70f;
    return 0.80f;  // wisdom, pattern, insight, belief, etc.
}

std::vector<float> QueueProcessor::embed_text(const std::string& text) {
    if (!yantra_) return {};
    try {
        return yantra_->transform(text).nu.data;
    } catch (...) {}
    return {};
}

std::string QueueProcessor::category_to_kind(const std::string& cat) {
    if (cat == "episode")    return "episode";
    if (cat == "belief")     return "belief";
    if (cat == "correction") return "correction";
    if (cat == "preference") return "preference";
    if (cat == "event")      return "milestone";
    return "wisdom";
}

void QueueProcessor::write_failed_item(const std::string& line, const std::exception& e) {
    try {
        json entry;
        entry["error"] = e.what();
        entry["retry_count"] = 0;
        entry["timestamp"] = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        auto parsed = json::parse(line, nullptr, false);
        if (!parsed.is_discarded()) {
            entry["tool"] = parsed.value("tool", "unknown");
            entry["args"] = parsed.value("args", json::object());
        } else {
            entry["raw"] = line.substr(0, 300);
        }
        if (!sandbox::append_line_atomic(
                failed_queue_path_,
                entry.dump(-1, ' ', false, json::error_handler_t::replace))) {
            std::cerr << "[queue] FAILED to write dead-letter entry to "
                      << failed_queue_path_
                      << " (original error: " << e.what() << ")\n";
        }
        queue_fail_count_++;
    } catch (const std::exception& inner) {
        std::cerr << "[queue] write_failed_item threw: " << inner.what()
                  << " (original error: " << e.what() << ")\n";
    } catch (...) {
        std::cerr << "[queue] write_failed_item threw unknown exception"
                  << " (original error: " << e.what() << ")\n";
    }
}

namespace {
// Dispatch owns only references to the current item and its prepared inputs.
// Lock acquisition, profiling, slow-lane flushes and checkpoints stay in run().
struct QueueToolDispatch {
    FieldStore& field_store_;
    FieldRpcHandler& handler_;
    std::atomic<size_t>& queue_count_;
    std::atomic<size_t>& queue_fail_count_;
    json& args;
    const std::string& tool;
    const std::string& line;
    std::vector<float>& correction_emb;
    std::vector<ExtractedSymbol>& file_syms;
    std::vector<std::vector<float>>& file_sym_embs;
    std::vector<std::string>& pending_slow;
    float (*category_to_confidence)(const std::string&);
    std::string (*category_to_kind)(const std::string&);

    bool observe() {
        std::string category = args.value("category", "wisdom");
        std::string title = args.value("title", "");
        std::string content = args.value("content", "");
        if (!content.empty()) {
            std::string source   = args.value("source", "hook_regex");
            std::string evidence = args.value("evidence", "");

            // ── Source trust policy (contract enforcement) ─────
            auto policy = source_policy(source);
            float confidence = args.contains("confidence")
                ? args.value("confidence", 0.50f)
                : category_to_confidence(category);
            float decay = policy.allow_durable ? 0.005f : 0.02f;

            // Clamp confidence to policy bounds
            float orig_confidence = confidence;
            confidence = std::min(confidence, policy.max_confidence);
            confidence = std::max(confidence, policy.min_confidence);
            if (orig_confidence != confidence) {
                std::cerr << "[contract] confidence clamped for source=" << source
                          << ": " << orig_confidence << "→" << confidence << "\n";
            }
            // Reject: distillation below floor is a bug, not a policy violation
            if (source == "distillation" && confidence < policy.min_confidence) {
                std::cerr << "[contract] REJECT: distillation event confidence="
                          << confidence << " below floor=" << policy.min_confidence
                          << " — check distillation pipeline\n";
                queue_fail_count_++;
                return true;  // skip this queue item
            }
            // Callers routinely pass a title that is already content's first line;
            // prefixing it again stores the same text twice and doubles its weight
            // in both the lexical lane and the embedding.
            std::string full_text =
                (title.empty() || content.rfind(title, 0) == 0)
                    ? content
                    : title + "\n" + content;
            std::string realm = args.value("realm", "brahman");
            json anchor = nullptr, previous = json::array();
            if (source.rfind("hook", 0) == 0) {
                anchor = RepositoryIndex::source_anchor(full_text, args.value("anchor", json()));
                if (!anchor.is_null()) {
                    full_text += "\n[anchor] " + anchor.dump();
                    previous = field_store_.source_anchors({{"anchor", anchor}, {"realm", realm}});
                }
            }
            // Pass empty embedding — backfill thread embeds asynchronously.
            auto new_id = field_store_.remember(category_to_kind(category), realm,
                                      full_text, {},
                                      confidence, decay);
            // Set initial epistemic status and memory status based on source
            if (new_id > 0) {
                uint8_t es = epistemic_status_for_source(source);
                uint8_t ms = initial_status_for_source(source);
                if (es != 1) field_store_.set_epistemic_status(new_id, es);
                if (ms != 0) field_store_.set_memory_status(new_id, ms);
            }
            // Store provenance as triplets
            if (new_id > 0 && !source.empty()) {
                field_store_.add_triplet(std::to_string(new_id), "source", source);
                if (!evidence.empty())
                    field_store_.add_triplet(std::to_string(new_id), "evidence", evidence);
            }
            // Only versions of this exact source fact supersede each other.
            // Same path with a different heading/symbol/fact remains independent.
            const auto observed = new_id > 0 && !anchor.is_null()
                ? field_store_.source_anchors({{"ids", json::array({std::to_string(new_id)})}, {"realm", realm}})
                : json::array();
            const bool current_anchor = !observed.empty() && observed[0].value("state", "") == "current";
            if (new_id > 0 && current_anchor) field_store_.set_memory_status(new_id, initial_status_for_source(source));
            if (new_id > 0 && current_anchor) for (const auto& old : previous) {
                const auto old_id = old.value("id", "0");
                // Reverting a file can deduplicate to a previously superseded
                // record. Retire its incoming version edge before reversing it.
                if (old_id != std::to_string(new_id))
                    field_store_.forget_triplet(old_id, "supersedes", std::to_string(new_id));
                if (old_id == std::to_string(new_id) || old.value("status", 0) == 1 ||
                    old["anchor"]["content_hash"] == anchor["content_hash"]) continue;
                field_store_.add_triplet(std::to_string(new_id), "supersedes", old_id, 1.0f, new_id);
                field_store_.set_memory_status(std::stoull(old_id), 1);
            }
            queue_count_++;
            // Correction supersession: find semantically similar memories
            // and mark them as superseded via a triplet relation
            if (category == "correction" && new_id > 0) {
                // Hard contradiction policy (CONTRACTS.md §3):
                // A correction supersedes ONLY:
                // 1. If target_id is explicit → supersede that specific memory
                // 2. Otherwise → same realm + cosine > 0.92 (tight threshold)
                //    AND same kind (corrections don't supersede different memory types)
                // force_supersede_ids: explicit list bypasses cosine threshold
                if (args.contains("force_supersede_ids")) {
                    auto fsi = args["force_supersede_ids"];
                    std::vector<std::string> ids;
                    if (fsi.is_array()) {
                        for (auto& v : fsi) ids.push_back(v.get<std::string>());
                    } else if (fsi.is_string()) {
                        std::istringstream ss(fsi.get<std::string>());
                        std::string tok;
                        while (std::getline(ss, tok, ',')) {
                            tok.erase(0, tok.find_first_not_of(' '));
                            tok.erase(tok.find_last_not_of(' ') + 1);
                            if (!tok.empty()) ids.push_back(tok);
                        }
                    }
                    for (const auto& sid : ids) {
                        try {
                            uint64_t tid = std::stoull(sid);
                            field_store_.add_triplet(std::to_string(new_id), "supersedes", sid, 1.0f, new_id);
                            field_store_.weaken(tid, 0.15f);
                            field_store_.set_memory_status(tid, 1);
                        } catch (...) {}
                    }
                }
                // Embedding precomputed before the lock (see top of loop).
                auto& emb = correction_emb;
                std::string target_id_str = args.value("target_id", "");
                if (!target_id_str.empty()) {
                    // Explicit target: targeted supersession
                    try {
                        uint64_t tid = std::stoull(target_id_str);
                        field_store_.add_triplet(std::to_string(new_id), "supersedes", target_id_str, 1.0f, new_id);
                        field_store_.weaken(tid, 0.15f);
                        field_store_.set_memory_status(tid, 1);
                        std::cerr << "[contract] explicit supersession: " << new_id << "→" << target_id_str << "\n";
                    } catch (...) {}
                } else if (!emb.empty()) {
                    // Semantic supersession: strict — same realm, same kind, very high threshold
                    auto hits = field_store_.recall(emb, 5, realm);
                    for (const auto& h : hits) {
                        if (h.memory_id == new_id) continue;
                        if (h.realm != realm) continue;           // same realm only
                        if (h.kind == "correction") continue;     // don't supersede other corrections
                        if (h.score < 0.92f) continue;            // tight: 0.92 not 0.85
                        field_store_.add_triplet(std::to_string(new_id), "supersedes", std::to_string(h.memory_id), 1.0f, new_id);
                        field_store_.weaken(h.memory_id, 0.15f);
                        field_store_.set_memory_status(h.memory_id, 1);
                        std::cerr << "[contract] semantic supersession: " << new_id << "→" << h.memory_id << " (score=" << h.score << ")\n";
                    }
                }
            }
        }
        return false;
    }

    bool strengthen() {
        std::string id_str = args.value("id", "");
        float amount = static_cast<float>(args.value("amount", 0.1));
        if (!id_str.empty()) {
            try {
                uint64_t cf_id = std::stoull(id_str);
                field_store_.strengthen(cf_id, amount);
                queue_count_++;
            } catch (...) {}
        }
        return false;
    }

    bool connect() {
        std::string subj = args.value("subject", "");
        std::string pred = args.value("predicate", "");
        std::string obj = args.value("object", "");
        if (!subj.empty() && !pred.empty() && !obj.empty()) {
            field_store_.add_triplet(subj, pred, obj, 1.0f,
                                     triplet_source_memory_id(field_store_, args));
            queue_count_++;
        }
        return false;
    }

    bool curiosity_note_gap() {
        std::string gap = args.value("gap", "");
        if (!gap.empty()) {
            std::string content = "[curiosity] " + gap;
            // Empty embedding — backfill thread handles it.
            field_store_.remember("episode", "brahman", content,
                                  {}, 0.7f, 0.0f);
            queue_count_++;
        }
        return false;
    }

    bool store_policy() {
        std::string policy_type = args.value("type", "");
        std::string content = args.value("content", "");
        float confidence = args.value("confidence", 0.5f);
        if (!policy_type.empty() && !content.empty()) {
            std::string full = "[policy:" + policy_type + "] " + content;
            // Empty embedding — backfill thread handles it.
            field_store_.remember("wisdom", "brahman", full,
                                  {}, confidence, 0.0f);
            queue_count_++;
        }
        return false;
    }

    bool store_claim() {
        std::string subject = args.value("subject", "");
        std::string predicate = args.value("predicate", "");
        std::string object_norm = args.value("object", "");
        if (!subject.empty() && !predicate.empty() && !object_norm.empty()) {
            field_store_.add_triplet(subject, predicate, object_norm, 1.0f,
                                     triplet_source_memory_id(field_store_, args));
            queue_count_++;
        }
        return false;
    }

    bool learn_outcome() {
        std::string id_str = args.contains("memory-id")
            ? args.value("memory-id", "")
            : args.value("memory_id", "");
        std::string outcome = args.value("outcome", "");
        std::string context = args.value("context", "");
        if (!id_str.empty() && !outcome.empty()) {
            try {
                uint64_t cf_id = std::stoull(id_str);
                json payload = {{"outcome", outcome}, {"context", context}};
                field_store_.emit_event("analytics", "outcome",
                                        id_str, payload.dump());
                if (outcome == "positive") field_store_.strengthen(cf_id, 0.1f);
                else if (outcome == "negative") field_store_.weaken(cf_id, 0.15f);
                queue_count_++;
            } catch (...) {}
        }
        return false;
    }

    bool session_lifecycle() {
        auto result = handler_.dispatch_session(tool, args);
        if (result.is_error) throw std::runtime_error("queued session/ledger operation failed: " + result.text);
        queue_count_++;
        return false;
    }

    bool transcript_register() {
        std::string session_id = args.value("session_id", "");
        std::string path = args.value("transcript_path", "");
        if (!path.empty()) {
            args["transcript_id"] = session_id;
            std::cerr << "[queue] transcript_register: session=" << session_id << " path=" << path << "\n";
            field_store_.emit_event("transcript", "register",
                                    session_id, args.dump());
            // Span lane: pick up this transcript's verbatim atoms
            // immediately (incremental via per-file byte watermark).
            field_store_.span_ingest(path);
            queue_count_++;
        }
        return false;
    }

    bool ledger_save() {
        std::string session_id = args.value("session_id", "");
        std::string project = args.value("project", "default");
        if (!session_id.empty()) {
            std::string key = session_id + ":" + project;
            field_store_.emit_event("ledger", "save", key, args.dump());
            queue_count_++;
        }
        return false;
    }

    bool ledger_append() {
        try {
            field_store_.ledger_append(args.dump());
            queue_count_++;
        } catch (const std::exception& e) {
            throw; // re-throw so the outer catch writes to dead-letter
        }
        return false;
    }

    bool narrative_log() {
        std::string session_id = args.value("session_id", "");
        std::string summary = args.value("summary", "");
        if (!session_id.empty() && !summary.empty()) {
            field_store_.emit_event("analytics", "narrative_log",
                                    session_id, args.dump());
            queue_count_++;
        }
        return false;
    }

    bool calibration_record() {
        std::string domain = args.value("domain", "");
        if (!domain.empty()) {
            field_store_.emit_event("analytics", "calibration",
                                    domain, args.dump());
            queue_count_++;
        }
        return false;
    }

    bool habit_observe() {
        std::string trigger = args.value("trigger", "");
        std::string response = args.value("response", "");
        if (!trigger.empty() && !response.empty()) {
            // habit_observe fires on ~every tool call and its analytics
            // event is write-only + unbounded (organ/analytics.rs Vec, no
            // cap), so sample to bound RAM/WAL growth. The RPC-side
            // tool_habit_observe still records every observation as a
            // triplet; this event is redundant telemetry.
            // ceiling: 1/HABIT_SAMPLE lossy; upgrade: cap analytics_registry
            // or add an aggregating reader, then drop the sampling.
            static std::atomic<uint64_t> habit_seq{0};
            constexpr uint64_t HABIT_SAMPLE = 20;
            if (habit_seq.fetch_add(1, std::memory_order_relaxed) % HABIT_SAMPLE == 0) {
                field_store_.emit_event("analytics", "habit",
                                        trigger, args.dump());
                queue_count_++;
            }
        }
        return false;
    }

    bool anticipation_success() {
        int64_t id = args.value("id", (int64_t)0);
        if (id > 0) {
            field_store_.emit_event("analytics", "anticipation_success",
                                    std::to_string(id), "{}");
            queue_count_++;
        }
        return false;
    }

    bool store_turn() {
        std::string session_id = args.value("session_id", "");
        std::string content = args.value("content", "");
        if (!session_id.empty() && !content.empty()) {
            field_store_.emit_event("transcript", "turn",
                                    session_id, args.dump());
            queue_count_++;
        }
        return false;
    }

    bool store_relationship_event() {
        std::string event_type = args.value("event_type", "");
        std::string session_id = args.value("session_id", "");
        if (!event_type.empty()) {
            field_store_.emit_event("relationship", event_type,
                                    session_id, args.dump());
            queue_count_++;
        }
        return false;
    }

    bool log_session_tokens() {
        std::string sid = args.value("session_id", "");
        if (!sid.empty()) {
            field_store_.emit_event("analytics", "session_tokens",
                                    sid, args.dump());
            queue_count_++;
        }
        return false;
    }

    bool log_correction_outcome() {
        std::string sid = args.value("session_id", "");
        if (!sid.empty()) {
            field_store_.emit_event("analytics", "correction_outcome",
                                    sid, args.dump());
            queue_count_++;
        }
        return false;
    }

    bool log_exposure() {
        std::string session_id = args.value("session_id", "");
        if (!session_id.empty()) {
            field_store_.emit_event("analytics", "exposure",
                                    session_id, args.dump());
            queue_count_++;
        }
        return false;
    }

    bool extract_symbols() {
        // Freshness loop: post-edit hooks queue changed files here.
        // Invalidate the file's old symbols, then insert the fresh
        // extraction (parsed + embedded before the lock). A deleted
        // file still gets its stale entries cleared.
        std::string p = args.value("path", "");
        if (!p.empty()) {
            field_store_.remove_symbols_by_file(p);
            for (size_t i = 0; i < file_syms.size(); ++i) {
                const auto& sym = file_syms[i];
                field_store_.upsert_symbol(
                    sym.kind, sym.name,
                    sym.signature.empty() ? sym.name : sym.signature,
                    sym.file_path,
                    static_cast<uint32_t>(sym.line_start),
                    static_cast<uint32_t>(sym.line_end),
                    0,
                    i < file_sym_embs.size() ? file_sym_embs[i]
                                             : std::vector<float>{});
            }
            queue_count_++;
        }
        return false;
    }

    bool distill_trigger() {
        // Re-route to the slow lane (buffered; appended after the
        // next sync so the transcript_register it depends on is
        // durable first). run_slow() executes it.
        pending_slow.push_back(line);
        return false;
    }

    // True preserves the observe rejection's outer-loop continue. Unknown
    // tools remain a no-op, followed by the usual batch accounting.
    bool dispatch() {
        using Member = bool (QueueToolDispatch::*)();
        static const std::unordered_map<std::string, Member> handlers = {
            {"observe", &QueueToolDispatch::observe},
            {"strengthen", &QueueToolDispatch::strengthen},
            {"connect", &QueueToolDispatch::connect},
            {"curiosity_note_gap", &QueueToolDispatch::curiosity_note_gap},
            {"store_policy", &QueueToolDispatch::store_policy},
            {"store_claim", &QueueToolDispatch::store_claim},
            {"learn_outcome", &QueueToolDispatch::learn_outcome},
            {"ledger_op", &QueueToolDispatch::session_lifecycle},
            {"session_register", &QueueToolDispatch::session_lifecycle},
            {"session_heartbeat", &QueueToolDispatch::session_lifecycle},
            {"session_deregister", &QueueToolDispatch::session_lifecycle},
            {"transcript_register", &QueueToolDispatch::transcript_register},
            {"ledger_save", &QueueToolDispatch::ledger_save},
            {"ledger_append", &QueueToolDispatch::ledger_append},
            {"narrative_log", &QueueToolDispatch::narrative_log},
            {"calibration_record", &QueueToolDispatch::calibration_record},
            {"habit_observe", &QueueToolDispatch::habit_observe},
            {"anticipation_success", &QueueToolDispatch::anticipation_success},
            {"store_turn", &QueueToolDispatch::store_turn},
            {"store_relationship_event", &QueueToolDispatch::store_relationship_event},
            {"log_session_tokens", &QueueToolDispatch::log_session_tokens},
            {"log_correction_outcome", &QueueToolDispatch::log_correction_outcome},
            {"log_exposure", &QueueToolDispatch::log_exposure},
            {"extract_symbols", &QueueToolDispatch::extract_symbols},
            {"distill_trigger", &QueueToolDispatch::distill_trigger},
        };
        const auto it = handlers.find(tool);
        return it != handlers.end() && (this->*it->second)();
    }
};
} // namespace

void QueueProcessor::run() try {
    // Span-lane flush cadence: live-path ingest links in RAM only; persist here,
    // off the memory-write hot path (also flushed on daemon close via cf_close).
    auto last_span_flush = std::chrono::steady_clock::now();
    MaintenanceJitter maintenance_jitter;
    uint64_t span_sequence = 0;
    auto next_span_flush = maintenance_jitter.delay(
        std::chrono::seconds(30), "queue_span_flush", span_sequence++);

    while (daemon_running) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        if (std::chrono::steady_clock::now() - last_span_flush >= next_span_flush) {
            last_span_flush = std::chrono::steady_clock::now();
            next_span_flush = maintenance_jitter.delay(
                std::chrono::seconds(30), "queue_span_flush", span_sequence++);
            if (!handler_.maintenance_should_skip("queue_span_flush"))
                field_store_.span_flush();
        }

        // Atomically claim queue file via rename (prevents data loss from concurrent writes)
        std::string processing_path = queue_path_ + ".processing";
        if (std::rename(queue_path_.c_str(), processing_path.c_str()) != 0) continue;

        // Read the claimed file
        std::vector<std::string> lines;
        {
            std::ifstream in(processing_path);
            std::string line;
            while (std::getline(in, line)) {
                if (!line.empty()) lines.push_back(line);
            }
        }

        AppliedAcks applied(queue_path_);
        if (lines.empty()) {
            std::remove(processing_path.c_str());
            continue;
        }
        applied.rotate(lines);
        batch_remaining_ = lines.size();

        // Checkpoint cadence: sync + watermark every N items so a crash (or
        // the shutdown-timeout force-exit) reprocesses at most N items instead
        // of the whole batch. The watermark only advances past durably synced
        // work, so recovery never skips an unsynced write.
        std::string ckpt_path = processing_path + ".ckpt";
        constexpr size_t kCkptInterval = 200;
        size_t processed = 0;

        // Ledger-coalesce: intermediate ledger_save events for the same
        // session:project are dead writes — ledger_load reads only the latest
        // (get_latest_event). Keep the LAST occurrence per key in this batch,
        // skip the rest (~21% of queue traffic).
        std::vector<bool> coalesce_skip(lines.size(), false);
        {
            std::unordered_map<std::string, size_t> last_idx;
            std::vector<std::pair<std::string, size_t>> saves;
            for (size_t li = 0; li < lines.size(); ++li) {
                if (lines[li].find("\"ledger_save\"") == std::string::npos) continue;
                auto j = json::parse(lines[li], nullptr, false);
                if (j.is_discarded() || j.value("tool", "") != "ledger_save") continue;
                auto a = j.value("args", json::object());
                std::string sid = a.value("session_id", "");
                if (sid.empty()) continue;
                std::string key = sid + ":" + a.value("project", "default");
                saves.emplace_back(key, li);
                last_idx[key] = li;
            }
            size_t coalesced = 0;
            for (const auto& [key, li] : saves)
                if (last_idx[key] != li) { coalesce_skip[li] = true; ++coalesced; }
            if (coalesced > 0)
                std::cerr << "[queue] ledger-coalesce: skipped " << coalesced
                          << " superseded ledger_save items\n";
        }

        // distill_trigger items are re-routed to the slow lane so a 10-90s LLM
        // item can never head-of-line-block µs writes. Buffered here and only
        // appended to the slow queue AFTER field_store_.sync() — guarantees the
        // transcript_register event a distill resolves against is durable
        // before the slow lane can claim the item.
        std::vector<std::string> pending_slow;
        auto flush_slow = [&](bool durable = false) {
            if (pending_slow.empty()) return;
            std::lock_guard<std::mutex> g(slow_mu_);
            for (const auto& sl : pending_slow)
                if (!sandbox::append_line_atomic(slow_path_, sl))
                    throw QueueAckError("slow handoff append failed");
            if (durable) {
                ack_sync_path(slow_path_);
                auto parent = std::filesystem::path(slow_path_).parent_path();
                ack_sync_path(parent.empty() ? "." : parent.string());
            }
            pending_slow.clear();
        };

        // Process each queued request — all writes go to chitta-field
        for (size_t li = 0; li < lines.size(); ++li) {
            const auto& line = lines[li];
            if (!daemon_running) break;
            if (coalesce_skip[li]) {
                // This is a terminal supersession decision. Retain its ack so
                // recovery cannot resurrect an older save after filtering the
                // already-applied replacement out of the same batch.
                applied.record(queue_ack_id(line), field_store_);
                batch_remaining_--;
                ++processed;
                continue;  // superseded ledger_save; nothing to sync
            }

            try {
                const auto ack_id = queue_ack_id(line);
                if (applied.contains(ack_id)) {
                    batch_remaining_--;
                    ++processed;
                    continue;
                }
                auto j = json::parse(line);
                std::string tool = j.value("tool", "");
                auto args = j.value("args", json::object());

                // Precompute the correction-supersession embedding BEFORE taking
                // the exclusive lock. embed_text() is a model forward pass (tens–
                // hundreds of ms) that touches no shared C++/Rust state, so running
                // it under rpc_mutex_ blocks every reader/recall RPC needlessly.
                // Only the rare semantic-correction path (no explicit target_id /
                // force_supersede_ids) needs it; compute here, consume inside lock.
                std::vector<float> correction_emb;
                if (tool == "observe" && args.value("category", "wisdom") == "correction"
                    && args.value("target_id", "").empty()
                    && !args.contains("force_supersede_ids")) {
                    std::string ct = args.value("title", "");
                    std::string cc = args.value("content", "");
                    if (!cc.empty())
                        correction_emb = embed_text(ct.empty() ? cc : ct + "\n" + cc);
                }

                // Precompute symbol extraction + embeddings for extract_symbols
                // BEFORE the lock — tree-sitter parsing and per-symbol embed
                // forward passes touch no shared state (same rationale as
                // correction_emb above).
                std::vector<ExtractedSymbol> file_syms;
                std::vector<std::vector<float>> file_sym_embs;
                if (tool == "extract_symbols") {
                    std::string p = args.value("path", "");
                    if (!p.empty() && std::filesystem::exists(p)) {
                        CodeIntel intel;
                        file_syms = intel.extract_file(p);
                        file_sym_embs.reserve(file_syms.size());
                        for (const auto& sym : file_syms) {
                            std::string text = sym.kind + " " + sym.name;
                            if (!sym.signature.empty()) text += " " + sym.signature;
                            file_sym_embs.push_back(embed_text(text));
                        }
                    }
                }

                // Recall-priority gate: each queued write below holds the EXCLUSIVE
                // rpc_mutex (observe/transcript_register ~300ms each), and a burst of
                // them stacks into multi-second recall stalls — the dominant recall-
                // starvation source under load (many sessions → observe/transcript
                // floods). Yield to live recalls before taking the lock. BOUNDED (≤1.5s):
                // after the cap the item proceeds regardless, so the queue still drains
                // and the WAL can't back up indefinitely — recall stall per item is then
                // capped at one ~300ms hold. Only the exclusive path is gated.
                if (tool != "distill_trigger")
                    for (int _g = 0; _g < 30 && daemon_running
                                     && handler_.recall_pressured(); ++_g)
                        std::this_thread::sleep_for(std::chrono::milliseconds(50));

                // FieldStore is always ready (synchronous init).
                // distill_trigger spawns an LLM call (10–30s); it must NOT hold
                // the unique rpc_mutex_ during that work, or every reader RPC
                // (health_check, msg_inbox, …) stalls. run_distillation takes
                // &handler_ and acquires the lock itself for the brief writes.
                std::unique_lock<std::shared_mutex> _lk;
                if (tool != "distill_trigger" && FieldRpcHandler::global_lock_enabled()) {
                    _lk = handler_.acquire_lock();
                }
                // Lock-hold profiler: log if this queued write holds the exclusive
                // rpc_mutex_ (blocking all recall) longer than CHITTA_LOCKPROF_MS. The
                // guard fires at end-of-iteration, covering every `continue` path.
                auto _lp_h0 = std::chrono::steady_clock::now();
                struct LockProfGuard {
                    const std::string& tool;
                    std::chrono::steady_clock::time_point t0;
                    bool held;
                    long held_ms = -1;
                    ~LockProfGuard() {
                        long thr = FieldRpcHandler::lockprof_threshold_ms();
                        if (!held || thr <= 0) return;
                        long ms = held_ms >= 0 ? held_ms : FieldRpcHandler::ms_since(t0);
                        if (ms >= thr)
                            std::cerr << "[lockprof] EXCLUSIVE queue:" << tool << " held=" << ms
                                      << "ms (blocks all readers/recall while held)\n";
                    }
                } _lp_guard{tool, _lp_h0, _lk.owns_lock()};

                QueueToolDispatch dispatch{
                    field_store_, handler_, queue_count_, queue_fail_count_, args,
                    tool, line, correction_emb, file_syms, file_sym_embs, pending_slow,
                    &QueueProcessor::category_to_confidence, &QueueProcessor::category_to_kind};
                if (dispatch.dispatch() && ack_id.empty()) continue;
                if (_lk.owns_lock()) {
                    _lp_guard.held_ms = FieldRpcHandler::ms_since(_lp_guard.t0);
                    _lk.unlock();
                }
                if (!ack_id.empty()) {
                    // A fast-lane distill ack means durable handoff, not completion.
                    if (tool == "distill_trigger") {
                        if (cf_sync(field_store_.handle()) != 0)
                            throw QueueAckError("store sync before slow handoff");
                        flush_slow(true);
                    }
                    applied.record(ack_id, field_store_);
                }
            } catch (const QueueAckError&) {
                throw;
            } catch (const std::exception& e) {
                // Always log — never silently drop a learning event
                std::cerr << "[queue] FAILED: " << e.what() << "\n";
                // Write to dead-letter file for inspection/retry
                write_failed_item(line, e);
            }
            batch_remaining_--;
            ++processed;
            if (processed % kCkptInterval == 0) {
                field_store_.sync();
                flush_slow();  // after sync: transcript events durable, safe to expose distills
                write_checkpoint(ckpt_path, processed);
            }
        }

        // Durable fdatasync of this batch's WAL appends, OFF the rpc_mutex (put_memory only
        // flush_buf()s under the lock now). One fsync per queue batch, not per item.
        field_store_.sync();
        flush_slow();

        if (processed < lines.size()) {
            // Graceful shutdown mid-batch: leave .processing + final watermark
            // for recovery. Previously this path fell through to the remove()
            // below and silently dropped the unprocessed tail.
            write_checkpoint(ckpt_path, processed);
            std::cerr << "[queue] shutdown mid-batch: " << processed << "/"
                      << lines.size() << " done, remainder recovered on restart\n";
            break;
        }

        // Remove after successful processing — crash before this leaves .processing for recovery
        std::remove(processing_path.c_str());
        std::remove(ckpt_path.c_str());

        if (verbose_mode) {
            std::cerr << "[queue] Processed " << lines.size() << " items, total=" << queue_count_ << "\n";
        }
    }
} catch (const QueueAckError& e) {
    std::cerr << "[queue] paused with processing file retained: " << e.what() << "\n";
}

// Execute one distill_trigger. Resolves the transcript via the durable event
// log only (the fast lane's sync-before-append ordering guarantees the
// register event is visible). run_distillation acquires the rpc lock itself
// for its brief writes — this thread never holds it across the LLM call.
void QueueProcessor::process_distill(const json& args, const std::string& endpoint) {
    if (!distill_config_.enabled) {
        if (verbose_mode)
            std::cerr << "[queue] distill_trigger ignored (distillation disabled)\n";
        return;
    }
    std::string session_id = args.value("session_id", "");
    if (session_id.empty()) return;

    std::string transcript_path;
    std::string realm = "brahman";
    auto reg = field_store_.get_latest_event("transcript", "register", session_id);
    if (reg) {
        try {
            auto r = json::parse(*reg);
            transcript_path = r.value("transcript_path", "");
            realm = r.value("realm", "brahman");
        } catch (...) {}
    }
    if (transcript_path.empty()) {
        std::cerr << "[queue] distill_trigger: no transcript registered for " << session_id << "\n";
        return;
    }
    std::cerr << "[queue] distill_trigger: path=" << transcript_path << " realm=" << realm << "\n";
    // Span lane: catch up on new transcript bytes before distilling
    // (incremental; no-op when unchanged).
    field_store_.span_ingest(transcript_path);
    TranscriptState ts;
    ts.session_id = session_id;
    ts.transcript_path = transcript_path;
    ts.realm = realm;
    ts.last_processed_line = 0;
    auto progress = field_store_.get_latest_event("transcript", "progress", session_id);
    if (progress) {
        try {
            auto p = json::parse(*progress);
            ts.last_processed_line = p.value("last_line", (int64_t)0);
        } catch (...) {}
    }
    DistillConfig cfg = distill_config_;
    cfg.endpoint = endpoint;  // pre-probed; skips per-distill discovery
    if (run_distillation(field_store_, yantra_, ts, cfg, &handler_, true)) {
        queue_distill_count_++;
        std::cerr << "[queue] distill_trigger: success (total=" << queue_distill_count_.load() << ")\n";
    } else {
        std::cerr << "[queue] distill_trigger: run_distillation returned false\n";
    }
}

// Slow lane: drains the distill queue (each item is a 10-90s LLM call).
// Mirrors the fast lane's claim/checkpoint/recover protocol but checkpoints
// after EVERY item — losing a watermark here costs a whole re-distill.
// Shares no mutable state with the fast lane; the only coupling is the
// slow queue file (fast appends, this claims) and FieldStore's own locking.
void QueueProcessor::run_slow() try {
    // Cached LLM endpoint, re-verified with one cheap curl before each distill.
    // Full re-discovery only on a probe miss; never `chitta-gpu start` from here.
    std::string endpoint;
    while (daemon_running) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));

        std::string processing_path = slow_path_ + ".processing";
        {
            std::lock_guard<std::mutex> g(slow_mu_);
            if (std::rename(slow_path_.c_str(), processing_path.c_str()) != 0) continue;
        }

        std::vector<std::string> lines;
        {
            std::ifstream in(processing_path);
            std::string line;
            while (std::getline(in, line)) {
                if (!line.empty()) lines.push_back(line);
            }
        }
        AppliedAcks applied(slow_path_);
        if (lines.empty()) {
            std::remove(processing_path.c_str());
            continue;
        }
        applied.rotate(lines);

        std::string ckpt_path = processing_path + ".ckpt";
        size_t processed = 0;
        bool endpoint_down = false;
        for (const auto& line : lines) {
            if (!daemon_running) break;
            try {
                const auto ack_id = queue_ack_id(line);
                if (applied.contains(ack_id)) { ++processed; continue; }
                auto j = json::parse(line);
                if (j.value("tool", "") == "distill_trigger") {
                    // POISON-PILL: bump the durable attempt count BEFORE distilling.
                    // A session that has already crashed the daemon kMaxDistillAttempts
                    // times is dead-lettered instead of re-crashing on every restart.
                    std::string sid = j.value("args", json::object()).value("session_id", "");
                    if (!sid.empty() && bump_distill_attempt(sid) > kMaxDistillAttempts) {
                        std::cerr << "[queue] distill poison-pill: session " << sid
                                  << " exceeded " << kMaxDistillAttempts
                                  << " attempts, dead-lettering\n";
                        write_failed_item(line, std::runtime_error(
                            "distill poison-pill: exceeded " +
                            std::to_string(kMaxDistillAttempts) + " attempts"));
                        clear_distill_attempt(sid);
                        ++processed;
                        field_store_.sync();
                        write_checkpoint(ckpt_path, processed);
                        continue;
                    }
                    // Fail-fast probe: verify the endpoint before run_distillation
                    // burns its 180s timeout (or worse, discovery's 120s
                    // chitta-gpu start) on a dead ollama.
                    if (distill_config_.endpoint.empty())
                        endpoint = discover_gpu_endpoint("teacher", distill_config_.model,
                                                         nullptr, /*allow_start=*/false);
                    if (endpoint.empty()) { endpoint_down = true; break; }
                    process_distill(j.value("args", json::object()), endpoint);
                    // Reached here without crashing → reset the attempt count.
                    if (!sid.empty()) clear_distill_attempt(sid);
                }
                applied.record(ack_id, field_store_);
            } catch (const QueueAckError&) {
                throw;
            } catch (const std::exception& e) {
                std::cerr << "[queue] slow-lane FAILED: " << e.what() << "\n";
                write_failed_item(line, e);
            }
            ++processed;
            field_store_.sync();
            write_checkpoint(ckpt_path, processed);
        }

        if (endpoint_down) {
            // Defer, don't dead-letter: put the unprocessed remainder back on
            // the slow queue for the next cycle, then back off so we don't
            // hammer squeue while the endpoint is down.
            size_t deferred = 0;
            {
                std::lock_guard<std::mutex> g(slow_mu_);
                for (size_t i = processed; i < lines.size(); ++i, ++deferred)
                    sandbox::append_line_atomic(slow_path_, lines[i]);
            }
            std::remove(processing_path.c_str());
            std::remove(ckpt_path.c_str());
            std::cerr << "[queue] distill endpoint down, " << deferred << " deferred\n";
            for (int i = 0; i < 120 && daemon_running; ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
            continue;
        }

        if (processed < lines.size()) {
            write_checkpoint(ckpt_path, processed);
            std::cerr << "[queue] slow-lane shutdown mid-batch: " << processed
                      << "/" << lines.size() << " done, remainder recovered on restart\n";
            break;
        }
        std::remove(processing_path.c_str());
        std::remove(ckpt_path.c_str());
    }
} catch (const QueueAckError& e) {
    std::cerr << "[queue] slow lane paused with processing file retained: " << e.what() << "\n";
}

} // namespace chitta
