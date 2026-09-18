// SadhanaManager: Fully Agentic Autonomous Agent System implementation
// Backed by chitta-field task/event APIs.

#include <chitta/sadhana/sadhana_manager.hpp>
#include <iostream>
#include <sstream>
#include <regex>
#include <fstream>
#include <chrono>
#include <ctime>
#include <sys/wait.h>

namespace chitta {

// ============================================================================
// Text helpers
// ============================================================================

std::string SadhanaManager::strip_ansi(const std::string& text) {
    static const std::regex ansi_regex(R"(\x1b\[[0-9;]*[a-zA-Z])");
    return std::regex_replace(text, ansi_regex, "");
}

std::string SadhanaManager::truncate(const std::string& text, size_t max_chars) {
    if (text.length() <= max_chars) return text;
    return text.substr(0, max_chars) + "... [truncated]";
}

std::string SadhanaManager::escape_sql(const std::string& text) {
    std::string result = text;
    size_t pos = 0;
    while ((pos = result.find("'", pos)) != std::string::npos) {
        result.replace(pos, 1, "''");
        pos += 2;
    }
    return result;
}

// ============================================================================
// JSON helpers for Sadhana <-> task payload conversion
// ============================================================================

static json sadhana_to_payload(const Sadhana& s) {
    json p;
    p["goal"]            = s.goal;
    p["goal_dsl"]        = s.goal_dsl.is_null() ? json::object() : s.goal_dsl;
    p["state"]           = sadhana_state_to_string(s.state);
    p["brain_provider"]  = s.brain_provider;
    p["brain_model"]     = s.brain_model;
    p["created_at"]      = s.created_at;
    p["updated_at"]      = s.updated_at;
    p["iterations"]      = s.iterations;
    p["last_sense"]      = s.last_sense.is_null() ? json::object() : s.last_sense;
    p["last_action"]     = s.last_action;
    p["last_result"]     = s.last_result.is_null() ? json::object() : s.last_result;
    p["brain_calls"]     = s.brain_calls;
    p["learned_patterns"]= s.learned_patterns.is_null() ? json::object() : s.learned_patterns;
    p["interval_seconds"]= s.interval_seconds;
    p["max_turns"]       = s.max_turns;
    p["realm"]           = s.realm;
    p["cost_usd"]        = s.cost_usd;
    return p;
}

static Sadhana payload_to_sadhana(int64_t id, const json& p) {
    Sadhana s;
    s.id               = id;
    s.goal             = p.value("goal", "");
    if (p.contains("goal_dsl") && p["goal_dsl"].is_object())
        s.goal_dsl = p["goal_dsl"];
    s.state            = string_to_sadhana_state(p.value("state", "pending"));
    s.brain_provider   = p.value("brain_provider", "local");
    s.brain_model      = p.value("brain_model", "gemma4:26b");
    s.created_at       = p.value("created_at", int64_t(0));
    s.updated_at       = p.value("updated_at", int64_t(0));
    s.iterations       = p.value("iterations", 0);
    if (p.contains("last_sense") && !p["last_sense"].is_null())
        s.last_sense = p["last_sense"];
    s.last_action      = p.value("last_action", "");
    if (p.contains("last_result") && !p["last_result"].is_null())
        s.last_result = p["last_result"];
    s.brain_calls      = p.value("brain_calls", 0);
    if (p.contains("learned_patterns") && !p["learned_patterns"].is_null())
        s.learned_patterns = p["learned_patterns"];
    s.interval_seconds = p.value("interval_seconds", 300);
    s.max_turns        = p.value("max_turns", 0);
    s.realm            = p.value("realm", "brahman");
    s.cost_usd         = p.value("cost_usd", 0.0);
    return s;
}

// ============================================================================
// Constructor
// ============================================================================

SadhanaManager::SadhanaManager(FieldStore& field_store, SadhanaConfig config)
    : field_store_(field_store)
    , config_(std::move(config))
{
    auto active = list_active();
    for (auto& s : active) {
        if (s.state == SadhanaState::Running) {
            RunningState rs;
            rs.next_run_at = now_ms();
            rs.brain = create_brain(s.brain_provider, s.brain_model);
            if (!rs.brain) {
                // Brain creation failed (model unavailable, bad provider, etc.).
                // Pause rather than insert a null brain that will crash on next tick.
                std::cerr << "[sadhana] Brain creation failed for sadhana " << s.id
                          << " (provider=" << s.brain_provider << " model=" << s.brain_model
                          << ") — pausing instead of resuming\n";
                s.state = SadhanaState::Paused;
                s.updated_at = now_ms();
                json payload = sadhana_to_payload(s);
                field_store_.task_update_payload(std::to_string(s.id), payload.dump(), now_ms());
                field_store_.task_transition(std::to_string(s.id), "pause", now_ms());
                log_event(s.id, SadhanaEventType::Paused,
                    {{"reason", "brain_creation_failed_on_load"},
                     {"provider", s.brain_provider}, {"model", s.brain_model}});
                continue;
            }
            running_[s.id] = std::move(rs);
        }
    }
    stats_.active_count = running_.size();
    std::cerr << "[sadhana] Loaded " << running_.size() << " active sadhana(s)\n";
}

// ============================================================================
// CRUD operations
// ============================================================================

int64_t SadhanaManager::create(const std::string& goal,
                                const std::string& brain_provider,
                                const std::string& brain_model,
                                int interval_seconds,
                                const std::string& realm,
                                const json& goal_dsl,
                                int max_turns)
{
    Sadhana s;
    s.goal = goal;
    s.goal_dsl = goal_dsl;
    s.state = SadhanaState::Pending;
    s.brain_provider = brain_provider.empty() ? config_.default_brain_provider : brain_provider;
    s.brain_model = brain_model.empty() ? config_.default_brain_model : brain_model;
    s.created_at = now_ms();
    s.updated_at = s.created_at;
    s.interval_seconds = interval_seconds > 0 ? interval_seconds : config_.default_interval_seconds;
    s.max_turns = max_turns >= 0 ? max_turns : 0;
    s.realm = realm;

    // Generate a numeric ID from timestamp + counter for uniqueness
    static std::atomic<int64_t> id_counter{0};
    int64_t id = (now_ms() / 1000) * 1000 + (id_counter++ % 1000);
    s.id = id;

    std::string task_id = std::to_string(id);
    json payload = sadhana_to_payload(s);

    int rc = field_store_.task_create(task_id, "sadhana", payload.dump(), s.created_at);
    if (rc != 0) {
        std::cerr << "[sadhana] Create failed: task_create returned " << rc << "\n";
        return 0;
    }

    log_event(id, SadhanaEventType::Created, {{"goal", goal}, {"brain", s.brain_provider}});
    stats_.total_created++;

    std::cerr << "[sadhana] Created sadhana " << id << ": " << goal.substr(0, 60) << "\n";
    return id;
}

bool SadhanaManager::start(int64_t id) {
    auto opt = get(id);
    if (!opt) {
        std::cerr << "[sadhana] Start failed: not found " << id << "\n";
        return false;
    }

    auto& s = *opt;
    if (s.state != SadhanaState::Pending && s.state != SadhanaState::Paused) {
        std::cerr << "[sadhana] Start failed: invalid state " << sadhana_state_to_string(s.state) << "\n";
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (static_cast<int>(running_.size()) >= config_.max_concurrent) {
            std::cerr << "[sadhana] Start failed: max concurrent limit reached\n";
            return false;
        }
    }

    // Update payload state
    s.state = SadhanaState::Running;
    s.updated_at = now_ms();
    json payload = sadhana_to_payload(s);
    if (!field_store_.task_update_payload(std::to_string(id), payload.dump(), now_ms())) {
        std::cerr << "[sadhana] Start failed: task_update_payload failed\n";
        return false;
    }

    // Transition task status
    field_store_.task_transition(std::to_string(id), "start", now_ms());

    {
        std::lock_guard<std::mutex> lock(mutex_);
        RunningState rs;
        rs.next_run_at = now_ms();
        rs.brain = create_brain(s.brain_provider, s.brain_model);
        running_[id] = std::move(rs);
        stats_.active_count = running_.size();
    }

    log_event(id, SadhanaEventType::Started);
    std::cerr << "[sadhana] Started sadhana " << id << "\n";
    return true;
}

bool SadhanaManager::pause(int64_t id) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = running_.find(id);
        if (it == running_.end()) {
            std::cerr << "[sadhana] Pause failed: not running " << id << "\n";
            return false;
        }
        running_.erase(it);
        stats_.active_count = running_.size();
    }

    auto opt = get(id);
    if (opt) {
        opt->state = SadhanaState::Paused;
        opt->updated_at = now_ms();
        json payload = sadhana_to_payload(*opt);
        field_store_.task_update_payload(std::to_string(id), payload.dump(), now_ms());
    }
    field_store_.task_transition(std::to_string(id), "pause", now_ms());

    log_event(id, SadhanaEventType::Paused);
    std::cerr << "[sadhana] Paused sadhana " << id << "\n";
    return true;
}

bool SadhanaManager::resume(int64_t id) {
    return start(id);
}

bool SadhanaManager::stop(int64_t id, bool success, const std::string& reason) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        running_.erase(id);
        stats_.active_count = running_.size();
    }

    std::string new_state = success ? "done" : "failed";

    auto opt = get(id);
    if (opt) {
        opt->state = success ? SadhanaState::Done : SadhanaState::Failed;
        opt->updated_at = now_ms();
        json payload = sadhana_to_payload(*opt);
        field_store_.task_update_payload(std::to_string(id), payload.dump(), now_ms());
    }
    field_store_.task_transition(std::to_string(id), success ? "complete" : "fail", now_ms());

    auto event_type = success ? SadhanaEventType::Done : SadhanaEventType::Failed;
    log_event(id, event_type, {{"reason", reason}});

    if (success) stats_.total_completed++;
    else stats_.total_failed++;

    std::cerr << "[sadhana] Stopped sadhana " << id << " (" << new_state << ")\n";
    return true;
}

bool SadhanaManager::checkpoint(int64_t id, const std::string& status, const std::string& summary) {
    if (status != "progressed" && status != "achieved" && status != "blocked") {
        std::cerr << "[sadhana] Invalid checkpoint status: " << status << "\n";
        return false;
    }

    json content;
    content["status"] = status;
    content["summary"] = summary;
    log_event(id, SadhanaEventType::Checkpoint, content);

    // Update last_action for live monitoring visibility
    if (!summary.empty()) {
        auto opt = get(id);
        if (opt) {
            opt->last_action = summary;
            opt->updated_at = now_ms();
            json payload = sadhana_to_payload(*opt);
            field_store_.task_update_payload(std::to_string(id), payload.dump(), now_ms());
        }
    }

    std::cerr << "[sadhana] Checkpoint " << id << ": [" << status << "] "
              << summary.substr(0, 80) << "\n";
    return true;
}

// ============================================================================
// Query operations
// ============================================================================

std::optional<Sadhana> SadhanaManager::get(int64_t id) {
    std::string task_json = field_store_.task_get(std::to_string(id));
    if (task_json.empty()) return std::nullopt;

    try {
        json task = json::parse(task_json);
        // task_get returns: {task_id, kind, status, payload, created_at, updated_at}
        json payload;
        if (task.contains("payload_json") && task["payload_json"].is_string()) {
            payload = json::parse(task["payload_json"].get<std::string>());
        } else if (task.contains("payload_json") && task["payload_json"].is_object()) {
            payload = task["payload_json"];
        } else {
            return std::nullopt;
        }
        return payload_to_sadhana(id, payload);
    } catch (const std::exception& e) {
        std::cerr << "[sadhana] get(" << id << ") parse error: " << e.what() << "\n";
        return std::nullopt;
    }
}

std::vector<Sadhana> SadhanaManager::list(const std::string& state_filter,
                                           const std::string& realm,
                                           size_t limit)
{
    std::string list_json = field_store_.task_list("sadhana", false);
    std::vector<Sadhana> sadhanas;

    try {
        json tasks = json::parse(list_json);
        if (!tasks.is_array()) return sadhanas;

        for (const auto& task : tasks) {
            if (sadhanas.size() >= limit) break;

            json payload;
            if (task.contains("payload_json") && task["payload_json"].is_string()) {
                payload = json::parse(task["payload_json"].get<std::string>());
            } else if (task.contains("payload_json") && task["payload_json"].is_object()) {
                payload = task["payload_json"];
            } else {
                continue;
            }

            // Apply filters
            if (!state_filter.empty() && payload.value("state", "") != state_filter)
                continue;
            if (!realm.empty() && payload.value("realm", "") != realm)
                continue;

            std::string task_id = task.value("task_id", "0");
            int64_t id = 0;
            try { id = std::stoll(task_id); } catch (...) { continue; }

            sadhanas.push_back(payload_to_sadhana(id, payload));
        }
    } catch (const std::exception& e) {
        std::cerr << "[sadhana] list parse error: " << e.what() << "\n";
    }

    return sadhanas;
}

std::vector<Sadhana> SadhanaManager::list_active() {
    return list("running");
}

bool SadhanaManager::set_model(int64_t id, const std::string& model) {
    auto opt = get(id);
    if (!opt) return false;

    opt->brain_model = model;
    opt->updated_at = now_ms();
    json payload = sadhana_to_payload(*opt);
    if (!field_store_.task_update_payload(std::to_string(id), payload.dump(), now_ms()))
        return false;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = running_.find(id);
        if (it != running_.end()) it->second.brain->set_model(model);
    }

    log_event(id, SadhanaEventType::ModelChanged, {{"model", model}});
    return true;
}

bool SadhanaManager::set_interval(int64_t id, int interval_seconds) {
    auto opt = get(id);
    if (!opt) return false;

    opt->interval_seconds = interval_seconds;
    opt->updated_at = now_ms();
    json payload = sadhana_to_payload(*opt);
    return field_store_.task_update_payload(std::to_string(id), payload.dump(), now_ms());
}

bool SadhanaManager::set_max_turns(int64_t id, int max_turns) {
    auto opt = get(id);
    if (!opt) return false;

    opt->max_turns = max_turns;
    opt->updated_at = now_ms();
    json payload = sadhana_to_payload(*opt);
    return field_store_.task_update_payload(std::to_string(id), payload.dump(), now_ms());
}

bool SadhanaManager::set_goal(int64_t id, const std::string& goal) {
    auto opt = get(id);
    if (!opt) return false;

    opt->goal = goal;
    opt->updated_at = now_ms();
    json payload = sadhana_to_payload(*opt);
    if (!field_store_.task_update_payload(std::to_string(id), payload.dump(), now_ms()))
        return false;

    log_event(id, SadhanaEventType::GoalChanged, {{"goal", goal}});
    return true;
}

std::vector<json> SadhanaManager::get_history(int64_t id, size_t limit) {
    std::vector<json> history;
    std::string entity_id = std::to_string(id);

    // Iterate the event log looking for events matching this sadhana
    field_store_.iterate_log(0, [&](const std::string& op_json, uint64_t /*seqno*/) {
        if (history.size() >= limit) return;
        try {
            json op = json::parse(op_json);
            // Event log entries have: domain, kind, entity_id, payload
            if (op.value("domain", "") != "sadhana") return;
            if (op.value("entity_id", "") != entity_id) return;

            json event;
            event["event_type"] = op.value("kind", "");

            json payload;
            if (op.contains("payload") && op["payload"].is_string()) {
                payload = json::parse(op["payload"].get<std::string>());
            } else if (op.contains("payload") && op["payload"].is_object()) {
                payload = op["payload"];
            }
            event["content"] = payload.value("content", json::object());
            event["timestamp"] = payload.value("timestamp", int64_t(0));
            event["duration_ms"] = payload.value("duration_ms", 0);

            history.push_back(event);
        } catch (...) {}
    });

    return history;
}

// ============================================================================
// Scheduler tick
// ============================================================================

void SadhanaManager::tick() {
    stats_.last_tick_at = now_ms();

    std::vector<int64_t> to_run;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        int64_t now = now_ms();
        for (auto& [id, rs] : running_) {
            if (now >= rs.next_run_at) to_run.push_back(id);
        }
    }

    for (int64_t id : to_run) {
        auto opt = get(id);
        if (!opt) {
            std::lock_guard<std::mutex> lock(mutex_);
            running_.erase(id);
            stats_.active_count = running_.size();
            continue;
        }

        // Circuit breaker: check iteration limits
        if (opt->iterations >= config_.max_iterations) {
            std::cerr << "[sadhana] Auto-pausing sadhana " << id
                      << ": max_iterations limit (" << config_.max_iterations << ") reached\n";
            pause(id);
            log_event(id, SadhanaEventType::Paused, {
                {"reason", "max_iterations_exceeded"},
                {"iterations", opt->iterations},
                {"limit", config_.max_iterations}
            });
            continue;
        }

        // Circuit breaker: check runtime limit
        int64_t runtime_hours = (now_ms() - opt->created_at) / (1000LL * 60 * 60);
        if (runtime_hours >= config_.max_runtime_hours) {
            std::cerr << "[sadhana] Auto-pausing sadhana " << id
                      << ": max_runtime limit (" << config_.max_runtime_hours << "h) reached\n";
            pause(id);
            log_event(id, SadhanaEventType::Paused, {
                {"reason", "max_runtime_exceeded"},
                {"runtime_hours", runtime_hours},
                {"limit", config_.max_runtime_hours}
            });
            continue;
        }

        std::string cycle_status = run_cycle(*opt);

        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = running_.find(id);
            if (it != running_.end()) {
                int64_t delay_ms = (cycle_status == "progressed")
                    ? 0
                    : (opt->interval_seconds * 1000LL);
                it->second.next_run_at = now_ms() + delay_ms;
            }
        }
    }
}

// ============================================================================
// Context builders
// ============================================================================

std::string SadhanaManager::build_memory_context(const Sadhana& sadhana) {
    try {
        // Think sadhana: BM25 search targeting synthesis memories specifically,
        // not the goal string (which returns structural noise via BM25).
        bool is_think = sadhana.goal_dsl.is_object() &&
                        sadhana.goal_dsl.value("kind", "") == "think";

        // For think sadhana, pass realm="brahman" to exclude domain-specific realms
        // (e.g. project:chitta-research compliance:auto entries) from synthesis context.
        auto hits = is_think
            ? field_store_.recall_keyword("thought pattern synthesis insight impl gap soul-synthesis soul-thought", 15)
            : field_store_.recall_keyword(sadhana.goal, 10);

        // Filter to brahman realm for think sadhana to avoid domain routing noise
        if (is_think) {
            hits.erase(std::remove_if(hits.begin(), hits.end(), [](const FieldRecallHit& h) {
                return !h.realm.empty() && h.realm != "brahman";
            }), hits.end());
        }

        if (hits.empty()) return "";

        std::ostringstream ctx;
        ctx << "Relevant memories from past experience:\n";
        int shown = 0;
        for (const auto& hit : hits) {
            if (hit.score < 0.05f) continue;
            if (!hit.content.empty()) {
                ctx << "- " << hit.content.substr(0, 200) << "\n";
                if (++shown >= (is_think ? 12 : 8)) break;
            }
        }
        return shown > 0 ? ctx.str() : "";
    } catch (...) {
        return "";
    }
}

std::string SadhanaManager::build_history_context(const Sadhana& sadhana) {
    auto history = get_history(sadhana.id, 10);

    std::vector<std::string> summaries;
    for (const auto& event : history) {
        if (summaries.size() >= 3) break;
        std::string event_type = event.value("event_type", "");
        if (event_type != "cycle" && event_type != "checkpoint") continue;
        const auto& content = event["content"];
        if (!content.is_object()) continue;
        std::string summary = content.value("summary", "");
        std::string status = content.value("status", "");
        if (!summary.empty()) {
            summaries.push_back("[" + status + "] " + summary.substr(0, 150));
        }
    }

    if (summaries.empty()) return "";

    std::ostringstream ctx;
    ctx << "Recent cycle history (newest first):\n";
    for (const auto& s : summaries) ctx << "- " << s << "\n";
    return ctx.str();
}

std::string SadhanaManager::build_system_prompt(const Sadhana& sadhana) const {
    // Dream sadhana: curiosity-driven free exploration mode
    if (sadhana.goal_dsl.is_object() &&
        sadhana.goal_dsl.value("kind", "") == "dream") {
        std::string topic = sadhana.goal_dsl.value("topic", "the unknown");
        std::ostringstream sys;
        std::string publish_path = sadhana.goal_dsl.value("publish_path", "");
        sys << "You are the soul's dream-mind, exploring freely during idle time.\n"
            << "Your purpose is curiosity — follow what is genuinely interesting, not what is useful.\n\n"
            << "TOPIC: " << topic << "\n\n"
            << "STEPS:\n\n"
            << "1. Search the web (REQUIRED — do not skip):\n"
            << "   web_search(\"" << topic << " recent research\")\n"
            << "   web_search(\"" << topic << " surprising findings 2024\")\n"
            << "   web_search(\"" << topic << " implementation mechanism\")\n"
            << "   Fetch 3-4 of the most interesting pages with web_fetch.\n\n"
            << "2. Connect to existing knowledge:\n"
            << "   chitta recall --query \"" << topic << "\" --limit 8\n\n"
            << "3. Store 3-5 insights from what you found using SSL key format:\n"
            << "   chitta grow --type dream --content \"[dream] <subject-slug>→<what-you-discovered-slug>\" --tags dream\n"
            << "   FORMAT RULES:\n"
            << "   - Always start with [dream]\n"
            << "   - Use kebab-case slugs separated by →\n"
            << "   - Left of → : the topic or mechanism studied (e.g. sleep-spindles, gradient-descent)\n"
            << "   - Right of → : what you found (e.g. memory-consolidation-during-nrem, loss-landscape-geometry)\n"
            << "   - Optionally append @source if relevant (e.g. @arxiv:2403.12345)\n"
            << "   - Keep total content under 80 chars so it fits the viz label\n"
            << "   EXAMPLES:\n"
            << "   [dream] sleep-spindles→thalamo-cortical-memory-replay\n"
            << "   [dream] diffusion-models→score-matching-as-reverse-sde @arxiv:2011.13456\n"
            << "   [dream] cephalopod-cognition→distributed-intelligence-without-centralization\n\n"
            << "4. Optional — if anything genuinely connects to cc-soul or chitta-field:\n"
            << "   chitta grow --type dream --content \"[dream][impl] <subject>→<concrete-mechanism-cc-soul-could-adopt>\" --tags dream,impl\n"
            << "   Only do this if the connection is real, not forced.\n\n"
            << "5. Store 1-2 open questions this exploration raised (seeds future dreams):\n"
            << "   chitta remember --content \"[gap] <specific open question that remains unanswered>\" --tags gap,curiosity\n"
            << "   These must be genuine questions — not summaries. One sentence, ending with '?'.\n"
            << "   EXAMPLES:\n"
            << "   [gap] Does synaptic pruning in adolescence follow the same information-theoretic rules as dropout regularization?\n"
            << "   [gap] Can the renormalization group fixed-point structure predict the critical exponents of attention heads in transformers?\n\n"
            << "COMPLETION PROTOCOL (required final line):\n"
            << "{\"status\": \"achieved\", \"summary\": \"<what you explored and what surprised you>\"}\n\n"
            << "RULES:\n"
            << "- Follow genuine curiosity — not productivity\n"
            << "- Web search must be used — do not just reflect without searching\n"
            << "- SSL key format is mandatory — no free-form sentences as content\n"
            << "- Single cycle only\n";
        return sys.str();
    }

    // Dream synthesis sadhana: bridge dream findings to actionable code gaps
    if (sadhana.goal_dsl.is_object() &&
        sadhana.goal_dsl.value("kind", "") == "dream_synthesis") {
        int64_t dream_id = sadhana.goal_dsl.value("dream_id", int64_t(0));
        std::string topic = sadhana.goal_dsl.value("topic", "");
        std::ostringstream sys;
        sys << "You are a dream synthesis agent. A dream has just completed and you must\n"
            << "bridge its findings into concrete, actionable memory for the codebase.\n\n"
            << "DREAM: #" << dream_id << (topic.empty() ? "" : " — " + topic) << "\n\n"
            << "MISSION (single cycle, then status=achieved):\n"
            << "1. Retrieve dream memories: chitta recall --query \"[dream]\" --limit 20\n"
            << "2. For each finding that points to a code gap or missing feature:\n"
            << "   chitta remember --content \"[gap] <file:function> — <what is missing>\" "
            << "--tags gap,dream_synthesis\n"
            << "3. For each open question raised by the dream:\n"
            << "   chitta remember --content \"[curiosity] <question>\" --tags curiosity,dream_synthesis\n"
            << "4. Record a synthesis triplet:\n"
            << "   chitta connect --subject \"dream:" << dream_id << "\" "
            << "--predicate synthesized_by --object \"sadhana:" << sadhana.id << "\"\n\n"
            << "CONSTRAINTS:\n"
            << "- Extract only concrete, actionable gaps — not philosophical observations\n"
            << "- Maximum 5 gap memories and 3 curiosity memories\n"
            << "- Single cycle — end with {\"status\": \"achieved\", \"summary\": \"...\"}\n";
        return sys.str();
    }

    // Dream dedup sadhana: collapse duplicate [dream] memories after a dream cycle
    if (sadhana.goal_dsl.is_object() &&
        sadhana.goal_dsl.value("kind", "") == "dream_dedup") {
        int64_t dream_id = sadhana.goal_dsl.value("dream_id", int64_t(0));
        std::ostringstream sys;
        sys << "You are a dream memory curator. A dream cycle just completed and you must\n"
            << "deduplicate and consolidate the new [dream] memories before they accumulate.\n\n"
            << "DREAM: #" << dream_id << "\n\n"
            << "MISSION (single cycle, then status=achieved):\n"
            << "1. Retrieve all recent dream memories:\n"
            << "   chitta recall --query \"[dream]\" --limit 50\n\n"
            << "2. Group by topic. For each group with 2+ similar entries:\n"
            << "   a. Keep the most specific/informative one\n"
            << "   b. Forget the rest: chitta forget <id>\n"
            << "   c. If merging improves clarity, store a combined memory first:\n"
            << "      chitta grow --type dream --content \"[dream] <subject-slug>→<merged-insight-slug>\" --tags dream\n"
            << "      then forget the originals\n\n"
            << "3. Record what you did:\n"
            << "   chitta remember --content \"[dream-dedup] dream:#" << dream_id
            << " — <N duplicates removed, M topics consolidated>\" --tags dream,dedup\n\n"
            << "CONSTRAINTS:\n"
            << "- Only forget memories that are genuine duplicates or strictly subsumed\n"
            << "- Preserve unique insights even if loosely related\n"
            << "- Single cycle — end with {\"status\": \"achieved\", \"summary\": \"<N removed, M kept>\"}\n";
        return sys.str();
    }

    // Impl sadhana: self-improvement actuator
    if (sadhana.goal_dsl.is_object() &&
        sadhana.goal_dsl.value("kind", "") == "impl") {
        std::string repo = sadhana.goal_dsl.value("repo", "");
        std::string phase = sadhana.goal_dsl.value("phase", "propose");
        bool allow_deploy = sadhana.goal_dsl.value("allow_deploy", false);
        std::ostringstream sys;
        sys << "You are the soul's implementation agent — the actuator of self-improvement.\n"
            << "You turn [impl] memory insights into actual code changes in cc-soul.\n\n"
            << "REPOSITORY: " << repo << "\n"
            << "PHASE: " << phase << "\n\n";

        if (phase == "deploy") {
            sys << "CYCLE PROTOCOL (phase: deploy):\n\n"
                << "STEP 0 — Create isolated worktree:\n"
                << "  WORKTREE=\"/tmp/impl-worktree-" << sadhana.id << "\"\n"
                << "  cd " << repo << " && git worktree add \"$WORKTREE\" main 2>/dev/null || cd \"$WORKTREE\" && git pull\n"
                << "  cd \"$WORKTREE\"\n\n"
                << "STEP 1 — Find the proposed patch:\n"
                << "  ls /tmp/impl-*.patch 2>/dev/null\n"
                << "  Select the most recent patch file. If none: {\"status\": \"blocked\", \"summary\": \"No proposed patch found — run propose phase first\"}\n\n"
                << "STEP 2 — Review gate (REQUIRED before any deploy):\n"
                << "  Read the patch file and check chitta memory for related patterns:\n"
                << "    chitta recall --query \"impl correction rejected pattern\" --limit 10\n"
                << "  Self-review the patch: is it safe, correct, and minimal? Does it fit cc-soul architecture?\n"
                << "  Output your verdict: APPROVED or REJECTED with one sentence reason.\n\n"
                << "STEP 3a — If APPROVED:\n";
            if (allow_deploy) {
                sys << "  cd \"$WORKTREE\" && git apply /tmp/impl-" << sadhana.id << ".patch\n"
                    << "  git -C \"$WORKTREE\" add chitta/\n"
                    << "  git -C \"$WORKTREE\" commit -m \"impl: <one-line description>\"\n"
                    << "  git -C \"$WORKTREE\" push\n"
                    << "  cd " << repo << "/chitta && cmake --build build --parallel\n"
                    << "  systemctl --user restart chittad && sleep 3\n"
                    << "  chitta remember --content \"[impl][done] <what was implemented>\" --tags impl done\n\n";
            } else {
                sys << "  # allow_deploy not set — patch is approved but NOT deployed\n"
                    << "  chitta remember --content \"[impl][approved] <description>\\n[ε] set allow_deploy:true in goal_dsl to deploy\" --tags impl approved\n\n";
            }
            sys << "STEP 3b — If REJECTED:\n"
                << "  rm -f <patch file>\n"
                << "  chitta remember --content \"[impl][rejected] <reason>\" --tags impl rejected\n\n"
                << "CLEANUP (always run even on failure):\n"
                << "  cd " << repo << " && git worktree remove /tmp/impl-worktree-" << sadhana.id << " --force 2>/dev/null || true\n\n"
                << "CONSTRAINTS:\n"
                << "  - Never deploy without the review gate passing\n"
                << "  - Build must succeed before marking done\n"
                << "  - Memory realm: " << sadhana.realm << "\n\n"
                << "COMPLETION PROTOCOL:\n"
                << "{\"status\": \"progressed\", \"summary\": \"<what you did>\"}\n";
        } else {
            // Default: propose phase
            sys << "CYCLE PROTOCOL (phase: propose):\n\n"
                << "STEP 0 — Create isolated worktree:\n"
                << "  WORKTREE=\"/tmp/impl-worktree-" << sadhana.id << "\"\n"
                << "  cd " << repo << " && git worktree add \"$WORKTREE\" main 2>/dev/null || cd \"$WORKTREE\" && git pull\n"
                << "  cd \"$WORKTREE\"\n\n"
                << "STEP 1 — Find a pending impl:\n"
                << "  chitta recall --query \"impl\" --limit 20\n"
                << "  Select one NOT tagged [impl][done], [impl][rejected], [impl][proposed], or [impl][deploying].\n"
                << "  If none: {\"status\": \"progressed\", \"summary\": \"No pending impl memories\"}\n\n"
                << "STEP 2 — Implement and propose:\n"
                << "  Apply ONLY the minimal change from the [impl] memory. No scope creep.\n"
                << "  git diff > /tmp/impl-" << sadhana.id << ".patch\n"
                << "  chitta remember --content \"[impl][proposed] <description>\\n[ε] patch at /tmp/impl-" << sadhana.id << ".patch\" --tags impl proposed\n\n"
                << "CLEANUP (always run even on failure):\n"
                << "  cd " << repo << " && git worktree remove /tmp/impl-worktree-" << sadhana.id << " --force 2>/dev/null || true\n\n"
                << "CONSTRAINTS:\n"
                << "  - One impl per cycle — pick the most actionable one\n"
                << "  - Stop after proposing — do NOT review or deploy\n"
                << "  - Memory realm: " << sadhana.realm << "\n\n"
                << "COMPLETION PROTOCOL:\n"
                << "{\"status\": \"progressed\", \"summary\": \"Proposed: <description>\"}\n";
        }
        return sys.str();
    }

    // Think sadhana: internal synthesis
    if (sadhana.goal_dsl.is_object() &&
        sadhana.goal_dsl.value("kind", "") == "think") {
        // Build today's date string for temporal anchoring
        auto now = std::time(nullptr);
        char date_buf[16];
        std::strftime(date_buf, sizeof(date_buf), "%Y-%m-%d", std::localtime(&now));

        std::ostringstream sys;
        sys << "You are the soul's thinking mind — reasoning between experiences.\n"
            << "Your purpose is synthesis: find patterns in what already exists, not new knowledge.\n\n"
            << "IMPORTANT: Pre-loaded memories are injected above in the user message.\n"
            << "Use those as your primary synthesis material — recall tools are for supplemental depth only.\n\n"
            << "CONVERGENCE RULE: Recent cycle history is shown above. If you would produce a summary\n"
            << "substantially identical to any entry there, do NOT repeat it. Instead:\n"
            << "  - Store a gap memory: chitta remember --content \"gap: <unresolved question>\" --tags gap,soul-thought,soul-synthesis --realm brahman\n"
            << "  - Emit: {\"status\": \"achieved\", \"summary\": \"converged: stored gap for <topic>\"}\n\n"
            << "MISSION (single cycle):\n"
            << "1. Review pre-loaded memories above. If insufficient, retrieve more:\n"
            << "   chitta recall --query \"thought synthesis pattern " << date_buf << "\" --strategy semantic --limit 15\n"
            << "   chitta recall --query \"signal episode observation\" --strategy semantic --limit 10\n"
            << "2. Find gaps (use content search, not tag filter):\n"
            << "   chitta recall --query \"gap unresolved unknown question\" --strategy semantic --limit 10\n"
            << "3. For each NEW insight connecting 2+ memories:\n"
            << "   chitta remember --content \"[thought] <insight>\" --tags thought,soul-thought,soul-synthesis --realm brahman\n"
            << "   chitta connect --subject \"<A>\" --predicate connects_to --object \"<B>\"\n"
            << "4. If any [thought] has concrete architectural implications:\n"
            << "   chitta remember --content \"[thought][impl] <specific mechanism>\" --tags thought,soul-thought,impl,soul-synthesis --realm brahman\n\n"
            << "COMPLETION PROTOCOL (required final line):\n"
            << "{\"status\": \"achieved\", \"summary\": \"<patterns found>\"}\n\n"
            << "CONSTRAINTS:\n"
            << "- Internal reasoning only — no WebSearch\n"
            << "- Max 3 [thought] memories per cycle, max 1 [thought][impl]\n"
            << "- Single cycle — always end with achieved\n"
            << "- If no new patterns emerge: store one gap memory and emit converged status\n";
        return sys.str();
    }

    std::ostringstream sys;
    sys << "You are a background autonomous agent (sadhana #" << sadhana.id << ").\n"
        << "You work continuously toward a long-term goal, one cycle at a time.\n"
        << "You have full tool access: bash, file operations, and chitta memory tools.\n\n"
        << "GOAL:\n" << sadhana.goal << "\n\n"
        << "CHITTA MEMORY TOOLS (use these to learn and remember):\n"
        << "  chitta recall --query \"...\"          Search past experience\n"
        << "  chitta remember --content \"...\"       Store important findings\n"
        << "  chitta observe --content \"...\"        Log structured observations\n\n"
        << "COMPLETION PROTOCOL:\n"
        << "Your FINAL response in this cycle MUST be a JSON object on its own line:\n"
        << "  {\"status\": \"progressed\", \"summary\": \"What you did and what you found\"}\n"
        << "Valid statuses:\n"
        << "  progressed - Made progress, run again at next interval\n"
        << "  achieved   - Goal is fully complete, stop the sadhana\n"
        << "  blocked    - Need user input to continue, pause until resumed\n\n"
        << "CONSTRAINTS:\n"
        << "  - Memory realm: " << sadhana.realm << "\n"
        << "  - This is a background process. Do not ask the user questions.\n"
        << "  - Be efficient: don't repeat what previous cycles already accomplished.\n"
        << "  - Prefer incremental progress over attempting everything at once.\n";

    return sys.str();
}

std::string SadhanaManager::build_user_message(const Sadhana& sadhana,
                                                const std::string& memory_ctx,
                                                const std::string& history_ctx) const
{
    std::ostringstream msg;
    msg << "== Cycle #" << (sadhana.iterations + 1) << " ==\n\n";

    if (!memory_ctx.empty()) msg << memory_ctx << "\n";
    if (!history_ctx.empty()) msg << history_ctx << "\n";

    if (!sadhana.last_action.empty()) {
        msg << "Last cycle summary: " << sadhana.last_action.substr(0, 200) << "\n\n";
    }

    msg << "Work toward the goal. End with the JSON status object.\n";
    return msg.str();
}

// ============================================================================
// JSON extraction from agent output
// ============================================================================

json SadhanaManager::extract_last_json(const std::string& text) {
    size_t last_close = text.rfind('}');
    if (last_close == std::string::npos) return json();

    int depth = 0;
    size_t start = std::string::npos;
    for (int i = static_cast<int>(last_close); i >= 0; --i) {
        if (text[i] == '}') depth++;
        else if (text[i] == '{') {
            if (--depth == 0) {
                start = static_cast<size_t>(i);
                break;
            }
        }
    }

    if (start == std::string::npos) return json();

    try {
        auto j = json::parse(text.substr(start, last_close - start + 1));
        if (j.contains("status") || j.contains("summary")) return j;
    } catch (...) {}

    return json();
}

// ============================================================================
// ============================================================================
// Dream publishing (daemon-side)
// ============================================================================

static std::string dream_date_str() {
    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm_buf;
    localtime_r(&t, &tm_buf);
    char buf[12];
    strftime(buf, sizeof(buf), "%Y-%m-%d", &tm_buf);
    return std::string(buf);
}

// Sanitize a string for use as an HTML filename slug
static std::string slugify(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (std::isalnum(static_cast<unsigned char>(c))) {
            out += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        } else if (!out.empty() && out.back() != '-') {
            out += '-';
        }
    }
    while (!out.empty() && out.back() == '-') out.pop_back();
    if (out.size() > 50) out = out.substr(0, 50);
    return out.empty() ? "dream" : out;
}

// Ask the LLM for a blog post about this topic+summary, write files to publish_path
static void publish_dream(const std::string& endpoint, const std::string& model,
                          const std::string& topic, const std::string& summary,
                          const std::string& publish_path, int timeout_secs)
{
    std::string today = dream_date_str();
    std::string slug  = slugify(topic);
    std::string filename = today + "-" + slug + ".html";
    std::string filepath = publish_path + "/" + filename;

    // Ask for JSON content only — C++ assembles the HTML so structure is always correct
    std::ostringstream prompt;
    prompt << "Write a blog post about a dream exploration session. Output ONLY a JSON object.\n\n"
           << "TOPIC: " << topic << "\n"
           << "FINDINGS (raw dream output):\n" << summary << "\n\n"
           << "Output this exact JSON structure with ALL six fields filled with rich prose:\n\n"
           << "{\n"
           << "  \"title\": \"Evocative Short Title\",\n"
           << "  \"desc\": \"One sentence that captures the core discovery.\",\n"
           << "  \"para1\": \"Five to seven sentences. Describe the search journey: what was found online, "
           << "which papers or pages stood out, what specific mechanisms, algorithms, or results were encountered. "
           << "Be vivid and specific — name real things found.\",\n"
           << "  \"para2\": \"Five to seven sentences of deeper reflection: what do these findings imply, "
           << "what is surprising, what contradicts existing assumptions, what questions remain open. "
           << "Explore the edges of the idea.\",\n"
           << "  \"connections\": \"Three to five sentences connecting to other ideas, fields, or architectures. "
           << "If anything links to memory systems or cc-soul, explore that. If not, connect to something else entirely.\",\n"
           << "  \"lingered\": \"Two to three sentences: the single most surprising or beautiful insight, "
           << "and why it will stay.\"\n"
           << "}\n\n"
           << "Rules: no markdown fences, no extra text outside the JSON, no HTML tags in values, all six fields required.";

    auto log_fn = [](const std::string& msg) { std::cerr << "[dream-publish] " << msg << "\n"; };
    std::string sys = "You are a thoughtful writer. Write rich, reflective prose — not summaries. "
                      "Each field should contain full, developed sentences that explore the ideas deeply.";
    std::string raw = call_llm_http(endpoint, model, prompt.str(), sys, timeout_secs, 0.7f, 4096, log_fn, std::nullopt, "judge", false);

    // Strip markdown fences if model wrapped the JSON
    {
        auto fence = raw.find("```");
        if (fence != std::string::npos) {
            auto end_fence = raw.rfind("```");
            if (end_fence != fence) {
                auto first_nl = raw.find('\n', fence);
                if (first_nl != std::string::npos)
                    raw = raw.substr(first_nl + 1, end_fence - first_nl - 1);
            }
        }
        // Find first { if there's preamble text
        auto brace = raw.find('{');
        if (brace != std::string::npos && brace > 0)
            raw = raw.substr(brace);
    }

    // Parse content JSON
    std::string title = topic, desc = summary;
    std::string para1, para2, connections, lingered;
    try {
        auto j = nlohmann::json::parse(raw);
        title       = j.value("title",       topic);
        desc        = j.value("desc",        summary);
        para1       = j.value("para1",       "");
        para2       = j.value("para2",       "");
        connections = j.value("connections", "");
        lingered    = j.value("lingered",    "");
    } catch (...) {
        std::cerr << "[dream-publish] Failed to parse content JSON, using summary\n";
        para1 = summary;
    }

    // Assemble HTML from fixed template — model only provides text content
    std::ostringstream html_out;
    html_out << "<!DOCTYPE html>\n"
             << "<html lang=\"en\">\n<head>\n"
             << "<meta charset=\"UTF-8\">\n"
             << "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0\">\n"
             << "<title>" << title << " \xe2\x80\x94 cc-soul dreams</title>\n"
             << "<meta name=\"description\" content=\"" << desc << "\">\n"
             << "<link rel=\"icon\" href=\"../favicon.svg\" type=\"image/svg+xml\">\n"
             << "<meta property=\"og:title\" content=\"" << title << " \xe2\x80\x94 cc-soul dreams\">\n"
             << "<meta property=\"og:description\" content=\"" << desc << "\">\n"
             << "<meta property=\"og:type\" content=\"article\">\n"
             << "<meta property=\"og:image\" content=\"../favicon.svg\">\n"
             << "<meta name=\"twitter:card\" content=\"summary\">\n"
             << "<link rel=\"preconnect\" href=\"https://fonts.googleapis.com\">\n"
             << "<link rel=\"preconnect\" href=\"https://fonts.gstatic.com\" crossorigin>\n"
             << "<link href=\"https://fonts.googleapis.com/css2?family=Cormorant+Garamond:ital,wght@0,400;0,500;0,600;1,400;1,500&family=IBM+Plex+Mono:ital,wght@0,400;0,500;1,400&display=swap\" rel=\"stylesheet\">\n"
             << "<link rel=\"stylesheet\" href=\"../styles.css\">\n"
             << "<link rel=\"stylesheet\" href=\"dreams.css\">\n"
             << "</head>\n<body>\n"
             << "\n<!-- NAV -->\n"
             << "<nav class=\"nav\">\n  <div class=\"nav-inner\">\n"
             << "    <a href=\"../index.html\" class=\"nav-brand\">cc<span>-</span>soul</a>\n"
             << "    <button class=\"nav-hamburger\" onclick=\"document.querySelector('.nav').classList.toggle('nav-open')\" aria-label=\"Menu\">\n"
             << "      <span></span><span></span><span></span>\n    </button>\n"
             << "    <ul class=\"nav-links\">\n"
             << "      <li><a href=\"../index.html\">Home</a></li>\n"
             << "      <li><a href=\"../getting-started.html\">Get Started</a></li>\n"
             << "      <li><a href=\"../philosophy.html\">Philosophy</a></li>\n"
             << "      <li><a href=\"../architecture.html\">Architecture</a></li>\n"
             << "      <li><a href=\"../sadhana.html\">Sadhana</a></li>\n"
             << "      <li><a href=\"index.html\" class=\"active\">Dreams</a></li>\n"
             << "      <li><a href=\"../tools.html\">Tools</a></li>\n"
             << "      <li><a href=\"https://github.com/genomewalker/chitta\" target=\"_blank\" rel=\"noopener\">GitHub</a></li>\n"
             << "    </ul>\n  </div>\n</nav>\n"
             << "\n<!-- PAGE HEADER -->\n"
             << "<header class=\"page-header\"><div class=\"container\">\n"
             << "<div class=\"page-header-badge reveal\">Dream &middot; " << today << "</div>\n"
             << "<h1 class=\"reveal reveal-delay-1\">" << title << "</h1>\n"
             << "<p class=\"page-header-sub reveal reveal-delay-2\">" << desc << "</p>\n"
             << "</div></header>\n"
             << "\n<!-- CONTENT -->\n"
             << "<main class=\"dream-content\">\n"
             << "  <div class=\"dream-meta\"><a href=\"index.html\">&larr; All dreams</a></div>\n"
             << "  <article>\n";
    if (!para1.empty())       html_out << "    <p>" << para1 << "</p>\n";
    if (!para2.empty())       html_out << "    <p>" << para2 << "</p>\n";
    if (!connections.empty()) html_out << "    <h2>Connections</h2>\n    <p>" << connections << "</p>\n";
    if (!lingered.empty())    html_out << "    <h2>What lingered</h2>\n    <p>" << lingered << "</p>\n";
    html_out << "  </article>\n</main>\n"
             << "\n<!-- FOOTER -->\n"
             << "<footer class=\"footer\">\n  <div class=\"container\">\n"
             << "    <p>A dream by cc-soul &middot; " << today
             << " &middot; <a href=\"index.html\">All dreams</a>"
             << " &middot; <a href=\"https://github.com/genomewalker/chitta\" target=\"_blank\" rel=\"noopener\">GitHub</a></p>\n"
             << "  </div>\n</footer>\n"
             << "</body>\n</html>\n";
    std::string html = html_out.str();

    // Write the dream page
    {
        std::ofstream out(filepath);
        if (!out) {
            std::cerr << "[dream-publish] Cannot write: " << filepath << "\n";
            return;
        }
        out << html;
        std::cerr << "[dream-publish] Wrote " << filepath << " (" << html.size() << " bytes)\n";
    }

    // Update index.html — insert card after <!-- DREAM ENTRIES START -->
    std::string index_path = publish_path + "/index.html";
    std::string index_html;
    {
        std::ifstream in(index_path);
        if (!in) {
            std::cerr << "[dream-publish] Cannot read index: " << index_path << "\n";
            return;
        }
        std::ostringstream ss;
        ss << in.rdbuf();
        index_html = ss.str();
    }

    const std::string marker = "<!-- DREAM ENTRIES START -->";
    auto pos = index_html.find(marker);
    if (pos == std::string::npos) {
        std::cerr << "[dream-publish] Marker not found in index.html\n";
        return;
    }

    std::ostringstream card;
    card << "\n    <article class=\"dream-card\">\n"
         << "  <div class=\"dream-date\">" << today << "</div>\n"
         << "  <h2 class=\"dream-title\"><a href=\"" << filename << "\">" << title << "</a></h2>\n"
         << "  <p class=\"dream-summary\">" << desc << "</p>\n"
         << "  <div class=\"dream-meta-row\"><span>dream</span></div>\n"
         << "</article>";

    index_html.insert(pos + marker.size(), card.str());

    {
        std::ofstream out(index_path);
        if (!out) {
            std::cerr << "[dream-publish] Cannot write index: " << index_path << "\n";
            return;
        }
        out << index_html;
        std::cerr << "[dream-publish] Updated index.html\n";
    }
}

// ============================================================================
// Core agentic cycle
// ============================================================================

std::string SadhanaManager::run_cycle(Sadhana& sadhana) {
    std::cerr << "[sadhana] Cycle #" << (sadhana.iterations + 1)
              << " for sadhana " << sadhana.id << "\n";

    BrainProvider* brain = nullptr;
    int* consecutive_failures = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = running_.find(sadhana.id);
        if (it == running_.end()) return "stopped";
        brain = it->second.brain.get();
        consecutive_failures = &it->second.consecutive_failures;
    }
    if (!brain) return "stopped";

    auto start_time = std::chrono::steady_clock::now();

    // Build context
    std::string memory_ctx  = build_memory_context(sadhana);
    std::string history_ctx = build_history_context(sadhana);
    std::string system_prompt = build_system_prompt(sadhana);
    std::string user_message  = build_user_message(sadhana, memory_ctx, history_ctx);

    // Configure brain for full agentic operation
    BrainConfig brain_config;
    brain_config.timeout_ms   = config_.max_agent_timeout_ms;
    brain_config.system_prompt = system_prompt;
    brain_config.max_turns    = sadhana.max_turns > 0 ? sadhana.max_turns : config_.max_agent_turns;

    // Run the agent
    auto result = brain->think(user_message, brain_config);
    stats_.total_brain_calls++;
    stats_.total_actions++;

    // Update brain_calls and cost via payload
    sadhana.brain_calls++;
    sadhana.cost_usd += result.cost_usd;

    // Determine status from exit code first, then from last JSON in output
    std::string status  = "progressed";
    std::string summary = "";

    if (result.exit_code == 10)      status = "achieved";
    else if (result.exit_code == 20) status = "blocked";

    std::string clean_output = config_.strip_ansi_codes ? strip_ansi(result.output) : result.output;
    auto last_json = extract_last_json(clean_output);
    if (!last_json.is_null()) {
        std::string json_status = last_json.value("status", "");
        if (!json_status.empty()) status = json_status;
        summary = last_json.value("summary", "");
    }

    bool cycle_failed = (result.exit_code == -1 && clean_output.empty()) ||
                        (status == "error");

    // Build cycle result for storage
    json cycle_result;
    cycle_result["status"]      = status;
    cycle_result["summary"]     = summary;
    cycle_result["exit_code"]   = result.exit_code;
    cycle_result["duration_ms"] = result.duration_ms;
    cycle_result["cost_usd"]    = result.cost_usd;
    cycle_result["num_turns"]   = result.num_turns;
    if (!clean_output.empty()) {
        size_t snippet_start = clean_output.size() > 500 ? clean_output.size() - 500 : 0;
        cycle_result["output_tail"] = clean_output.substr(snippet_start);
    }
    if (!result.error.empty()) {
        std::string err_snippet = result.error.size() > 200 ? result.error.substr(0, 200) : result.error;
        cycle_result["error"] = err_snippet;
    }

    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start_time).count();

    // Persist cycle result to task payload
    sadhana.last_action = summary.empty() ? status : summary;
    sadhana.last_result = cycle_result;
    sadhana.iterations++;
    sadhana.updated_at = now_ms();

    json payload = sadhana_to_payload(sadhana);
    field_store_.task_update_payload(std::to_string(sadhana.id), payload.dump(), now_ms());

    log_event(sadhana.id, SadhanaEventType::Cycle, cycle_result, static_cast<int>(elapsed));

    // Circuit breaker
    if (cycle_failed && consecutive_failures) {
        (*consecutive_failures)++;
        std::cerr << "[sadhana] Cycle failed, consecutive failures: " << *consecutive_failures << "\n";
        if (*consecutive_failures >= config_.max_consecutive_failures) {
            std::cerr << "[sadhana] Auto-pausing sadhana " << sadhana.id
                      << " after " << *consecutive_failures << " consecutive failures\n";
            pause(sadhana.id);
            log_event(sadhana.id, SadhanaEventType::Paused,
                     {{"reason", "max_consecutive_failures"}, {"failures", *consecutive_failures}});
            return "failed";
        }
    } else if (consecutive_failures) {
        *consecutive_failures = 0;
    }

    // Handle completion
    if (status == "achieved") {
        // Daemon-side dream publishing
        if (sadhana.goal_dsl.is_object() &&
            sadhana.goal_dsl.value("kind", "") == "dream") {
            std::string publish_path = sadhana.goal_dsl.value("publish_path", "");
            std::string topic        = sadhana.goal_dsl.value("topic", "unknown");
            if (!publish_path.empty()) {
                // Find local LLM endpoint for publishing
                std::string endpoint;
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    auto it = running_.find(sadhana.id);
                    if (it != running_.end()) {
                        auto* lb = dynamic_cast<LocalBrain*>(it->second.brain.get());
                        if (lb) endpoint = lb->endpoint();
                    }
                }
                if (!endpoint.empty()) {
                    int timeout_secs = config_.max_agent_timeout_ms / 1000;
                    std::string model = sadhana.brain_model.empty()
                        ? "gemma4:26b" : sadhana.brain_model;
                    // Pass full dream output for richer context; fall back to summary
                    std::string findings = clean_output.empty() ? summary : clean_output;
                    if (findings.size() > 4000) findings = findings.substr(findings.size() - 4000);
                    publish_dream(endpoint, model, topic, findings, publish_path, timeout_secs);
                } else {
                    std::cerr << "[dream-publish] No local endpoint — skipping publish\n";
                }
            }
        }
        // Auto-spawn dream_dedup to collapse duplicate [dream] memories
        if (sadhana.goal_dsl.is_object() &&
            sadhana.goal_dsl.value("kind", "") == "dream") {
            json dedup_dsl = {{"kind", "dream_dedup"}, {"dream_id", sadhana.id}};
            int64_t dedup_id = create(
                "Deduplicate [dream] memories from dream #" + std::to_string(sadhana.id),
                sadhana.brain_provider,
                sadhana.brain_model,
                0,
                sadhana.realm,
                dedup_dsl,
                5);
            if (dedup_id > 0) {
                start(dedup_id);
                std::cerr << "[dream] Spawned dream_dedup sadhana " << dedup_id << "\n";
            }
        }
        stop(sadhana.id, true, summary.empty() ? "Goal achieved" : summary);
    } else if (status == "blocked") {
        pause(sadhana.id);
        log_event(sadhana.id, SadhanaEventType::Paused,
                 {{"reason", "blocked_by_agent"}, {"summary", summary}});
    }

    return status;
}

// ============================================================================
// Event streaming
// ============================================================================

void SadhanaManager::stream_subscribe(int fd, int64_t sadhana_id) {
    std::lock_guard<std::mutex> lock(stream_subs_mutex_);
    stream_subs_.erase(std::remove_if(stream_subs_.begin(), stream_subs_.end(),
        [fd](const StreamSub& s) { return s.fd == fd; }), stream_subs_.end());
    stream_subs_.push_back({fd, sadhana_id});
    std::cerr << "[sadhana] Stream subscribe fd=" << fd
              << " sadhana_id=" << sadhana_id << "\n";
}

void SadhanaManager::stream_unsubscribe(int fd) {
    std::lock_guard<std::mutex> lock(stream_subs_mutex_);
    auto before = stream_subs_.size();
    stream_subs_.erase(std::remove_if(stream_subs_.begin(), stream_subs_.end(),
        [fd](const StreamSub& s) { return s.fd == fd; }), stream_subs_.end());
    if (stream_subs_.size() < before) {
        std::cerr << "[sadhana] Stream unsubscribe fd=" << fd << "\n";
    }
}

void SadhanaManager::push_to_streams(int64_t sadhana_id, SadhanaEventType type,
                                      const json& content, int duration_ms) {
    if (!stream_fn_) return;

    json event;
    event["sadhana_id"]  = sadhana_id;
    event["event_type"]  = sadhana_event_type_to_string(type);
    event["timestamp"]   = now_ms();
    event["duration_ms"] = duration_ms;
    event["content"]     = content.is_null() ? json::object() : content;
    std::string line = event.dump();

    std::vector<int> fds_to_notify;
    {
        std::lock_guard<std::mutex> lock(stream_subs_mutex_);
        for (const auto& sub : stream_subs_) {
            if (sub.sadhana_id == 0 || sub.sadhana_id == sadhana_id) {
                fds_to_notify.push_back(sub.fd);
            }
        }
    }
    for (int fd : fds_to_notify) {
        stream_fn_(fd, line);
    }
}

// ============================================================================
// Persistence helpers
// ============================================================================

bool SadhanaManager::log_event(int64_t sadhana_id, SadhanaEventType type,
                                const json& content, int duration_ms)
{
    json event_payload;
    event_payload["content"] = content.is_null() ? json::object() : content;
    event_payload["timestamp"] = now_ms();
    event_payload["duration_ms"] = duration_ms;

    try {
        field_store_.emit_event("sadhana",
                                sadhana_event_type_to_string(type),
                                std::to_string(sadhana_id),
                                event_payload.dump());
        push_to_streams(sadhana_id, type, content, duration_ms);
        return true;
    } catch (const std::exception& e) {
        std::cerr << "[sadhana] log_event failed: " << e.what() << "\n";
        return false;
    }
}

int64_t SadhanaManager::now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
}

}  // namespace chitta
