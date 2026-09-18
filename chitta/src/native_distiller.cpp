#include "../include/chitta/native_distiller.hpp"
#include "../include/chitta/ssl_gloss.hpp"
#include "../include/chitta/ssl_prompt.hpp"
#include "../include/chitta/value_fact_extractor.hpp"
#include <sstream>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>

namespace chitta {

// ── Constructor ──────────────────────────────────────────────────────────────

NativeDistiller::NativeDistiller(FieldStore& field, EmbedFn embedder,
                                 const NativeDistillConfig& config)
    : field_store_(&field), embedder_(std::move(embedder)), config_(config) {}

// ── Helpers ──────────────────────────────────────────────────────────────────

float NativeDistiller::category_to_confidence(const std::string& category) {
    if (category == "correction") return 0.95f;
    if (category == "belief")     return 0.90f;
    if (category == "preference") return 0.90f;
    if (category == "solution")   return 0.90f;
    if (category == "decision")   return 0.85f;
    if (category == "failure")    return 0.85f;
    if (category == "gotcha")     return 0.85f;
    if (category == "pattern")    return 0.80f;
    return 0.80f;  // wisdom, etc.
}

float NativeDistiller::category_to_decay(const std::string& category) {
    // Corrections, beliefs, preferences are long-lived; general wisdom decays faster
    if (category == "correction") return 0.0001f;
    if (category == "preference") return 0.0001f;
    if (category == "belief")     return 0.0001f;
    if (category == "solution")   return 0.001f;
    if (category == "decision")   return 0.001f;
    return 0.005f;
}

// SSL category → storage kind. Most categories collapse to "wisdom"; only the
// four first-class kinds get distinct storage so soul_context can surface them.
static std::string distill_category_to_kind(const std::string& cat) {
    if (cat == "correction") return "correction";
    if (cat == "preference") return "preference";
    if (cat == "belief")     return "belief";
    if (cat == "event")      return "milestone";
    return "wisdom";
}

void NativeDistiller::log(const std::string& msg) {
    if (log_callback_) {
        log_callback_(msg);
    } else if (config_.verbose) {
        std::cerr << msg << "\n";
    }
}

// ── LLM HTTP call ───────────────────────────────────────────────────────────

std::string NativeDistiller::call_llm(const std::string& prompt) {
    return call_llm_http(
        config_.endpoint, config_.model, prompt,
        "You are a knowledge distiller. Extract learnings in SSL v0.3 format. "
        "Output ONLY SSL-formatted learnings with A:v,a affect annotations.",
        config_.timeout_secs, 0.3f, config_.max_tokens,
        [this](const std::string& msg) { log(msg); }, std::nullopt, "teacher", true);
}

// ── precompute_dedup (lock-free: embed + recall, no writes) ──────────────────

void NativeDistiller::precompute_dedup(PreparedDistillation& prep) {
    prep.learning_preps.clear();
    prep.learning_preps.reserve(prep.ssl_result.learnings.size());
    for (const auto& learning : prep.ssl_result.learnings) {
        LearningPrep lp;
        std::string full_text = learning.title + "\n" + learning.content;
        if (embedder_) {
            lp.embedding = embedder_(chitta::ssl::retrieval_text(full_text));
        }
        if (!lp.embedding.empty() && config_.dedup_threshold > 0.0f) {
            auto check_hits = [&](const std::vector<FieldRecallHit>& hits) {
                for (const auto& hit : hits) {
                    if (hit.semantic_score >= config_.dedup_threshold) {
                        lp.is_dup         = true;
                        lp.dup_mem_id     = hit.memory_id;
                        lp.dup_confidence = hit.confidence;
                        return true;
                    }
                }
                return false;
            };
            // Primary: realm-filtered search.
            if (!check_hits(field_store_->recall(lp.embedding, 5, prep.realm))) {
                // Fallback: cross-realm search catches duplicates from other realms
                // (e.g. dream-sweep distilling with brahman when memories live in cc-soul).
                if (!prep.realm.empty())
                    check_hits(field_store_->recall(lp.embedding, 5, ""));
            }
        }
        prep.learning_preps.push_back(std::move(lp));
    }
}

// ── store_learnings (writes only — uses precomputed dedup from LearningPrep) ─

void NativeDistiller::store_learnings(
    const SSLParser::Result& ssl_result,
    const std::string& realm,
    uint64_t episode_mem_id,
    const std::vector<LearningPrep>& learning_preps,
    DistillResult& result
) {
    for (size_t i = 0; i < ssl_result.learnings.size(); ++i) {
        const auto& learning = ssl_result.learnings[i];
        const LearningPrep& lp = (i < learning_preps.size())
                                    ? learning_preps[i] : LearningPrep{};
        float confidence = category_to_confidence(learning.category);
        float decay      = category_to_decay(learning.category);
        std::string full_text = learning.title + "\n" + learning.content;

        if (lp.is_dup) {
            field_store_->strengthen(lp.dup_mem_id, 0.05f);
            if (lp.dup_confidence < 0.75f) {
                float target = category_to_confidence(learning.category);
                field_store_->update_confidence(lp.dup_mem_id, target - lp.dup_confidence);
                log("[distill]   promoted confidence: " + learning.category +
                    " " + std::to_string(lp.dup_confidence) + "→" + std::to_string(target));
            }
            result.learnings_deduped++;
            log("[distill]   ~dup " + learning.category + ": " +
                learning.title.substr(0, 50) + " (strengthened existing)");
            continue;
        }

        uint64_t mem_id = 0;
        try {
            mem_id = field_store_->remember(distill_category_to_kind(learning.category),
                                            realm, full_text,
                                            lp.embedding, confidence, decay);
        } catch (...) {
            continue;
        }

        if (mem_id == 0) continue;

        result.learnings_stored++;
        log("[distill]   +" + learning.category + ": " +
            learning.title.substr(0, 60) + "...");

        // Apply affect dimensions (v0.3: from A:v,a on any type)
        if (learning.affect_valence != 0.0f || learning.affect_arousal != 0.0f) {
            field_store_->set_affect(mem_id, learning.affect_valence, learning.affect_arousal);
            log("[distill]     affect: v=" + std::to_string(learning.affect_valence) +
                " a=" + std::to_string(learning.affect_arousal));
        }

        // Apply structural flags (v0.3: F:FLAG)
        for (const auto& flag : learning.flags) {
            field_store_->add_triplet(std::to_string(mem_id), "has_flag", flag, 1.0f, mem_id);
            log("[distill]     flag: " + flag);
        }

        // Apply cross-references (v0.3: →@ref)
        for (const auto& ref : learning.refs) {
            field_store_->add_triplet(std::to_string(mem_id), "references", ref, 1.0f, mem_id);
            log("[distill]     ref: →@" + ref);
        }

        // Link to episode memory via DerivedFrom edge (edge_type=0)
        if (episode_mem_id > 0) {
            field_store_->add_edge(mem_id, episode_mem_id, 0, 1.0f);
            field_store_->add_triplet(std::to_string(mem_id),
                                      "derived_from",
                                      std::to_string(episode_mem_id), 1.0f, mem_id);
        }

        // Code citations
        for (const auto& cite : learning.citations) {
            std::string cite_target = cite.file;
            if (cite.line > 0) {
                cite_target += ":" + std::to_string(cite.line);
            }
            field_store_->add_triplet(std::to_string(mem_id), "cites", cite_target, 1.0f, mem_id);
            result.citations_linked++;
            log("[distill]     cite: " + cite_target);

            // Bridge to symbol at that file:line if available
            if (cite.line > 0) {
                auto syms = field_store_->symbols_in_file(cite.file);
                for (const auto& sym : syms) {
                    if (sym.line_start <= static_cast<uint32_t>(cite.line) &&
                        static_cast<uint32_t>(cite.line) <= sym.line_end) {
                        std::string sym_ref = "symbol:" + std::to_string(sym.symbol_id);
                        field_store_->add_triplet(std::to_string(mem_id), "cites", sym_ref, 1.0f, mem_id);
                        log("[distill]     → symbol: " +
                            std::string(reinterpret_cast<const char*>(sym.name)));
                        break;
                    }
                }
            }
        }
    }

    // Store triplets
    for (const auto& triplet : ssl_result.triplets) {
        // Session-level facts belong to the episode memory: without a source id
        // the analogy signature index (analogy.rs) can't see them at all.
        field_store_->add_triplet(triplet.subject, triplet.predicate, triplet.object,
                                  1.0f, episode_mem_id);
        log("[distill]   triplet: " + triplet.subject + "→" +
            triplet.predicate + "→" + triplet.object);
    }
}

// ── value-facts (deterministic pass — extract, dedup, write) ─────────────────

bool NativeDistiller::value_facts_enabled() {
    // Default ON (net-positive: +0.907 coverage, 0.000 degradation). Off only when
    // CHITTA_VALUE_FACTS is explicitly 0/false/no/off.
    const char* v = std::getenv("CHITTA_VALUE_FACTS");
    if (!v || !*v) return true;
    return !(std::strcmp(v, "0") == 0 || std::strcmp(v, "false") == 0 ||
             std::strcmp(v, "no") == 0 || std::strcmp(v, "off") == 0);
}

// Phase 1c (lock-free): extract + embed + dedup-recall value-facts. NO writes, NO
// lock. Populates prep.value_fact_preps for the write-only commit pass. The dedup
// here is a TOCTOU snapshot (same discipline as precompute_dedup for learnings): a
// concurrent writer could insert a matching fact between this recall and the commit
// remember(), producing a rare duplicate that the next distill's semantic dedup
// absorbs. The slow lane is single-threaded so distills never overlap each other;
// the only concurrent writers are fast-lane remembers, and a stray near-dup is
// strengthened rather than corrupting the store.
void NativeDistiller::precompute_value_facts(PreparedDistillation& prep) {
    prep.value_fact_preps.clear();
    if (!value_facts_enabled() || prep.conversation.empty()) return;

    // BOUND: cap the bytes handed to the deterministic extractor so a giant
    // conversation can't make even the lock-free pass unbounded.
    const std::string* conv = &prep.conversation;
    std::string capped;
    if (config_.value_fact_max_bytes > 0 &&
        prep.conversation.size() > config_.value_fact_max_bytes) {
        // Keep the tail — most recent turns carry the freshest scalars.
        capped = prep.conversation.substr(prep.conversation.size() - config_.value_fact_max_bytes);
        conv = &capped;
        log("[distill]   value-facts: capped input " +
            std::to_string(prep.conversation.size()) + "→" +
            std::to_string(config_.value_fact_max_bytes) + " bytes");
    }

    auto facts = extract_value_facts(*conv);
    if (facts.empty()) return;
    log("[distill]   value-facts: " + std::to_string(facts.size()) + " candidates");

    prep.value_fact_preps.reserve(facts.size());
    for (auto& fact : facts) {
        ValueFactPrep vp;
        if (embedder_) vp.embedding = embedder_(chitta::ssl::retrieval_text(fact.content));

        // DEDUP: if a strict holder already covers this identifier+value, mark skip.
        // The content leads with "<identifier> <value>", so semantic recall on it
        // matches an existing memory that already states the pair.
        if (!vp.embedding.empty() && config_.dedup_threshold > 0.0f) {
            auto check = [&](const std::vector<FieldRecallHit>& hits) {
                for (const auto& hit : hits)
                    if (hit.semantic_score >= config_.dedup_threshold) { vp.is_dup = true; return true; }
                return false;
            };
            if (!check(field_store_->recall(vp.embedding, 5, prep.realm)) && !prep.realm.empty())
                check(field_store_->recall(vp.embedding, 5, ""));
        }

        vp.fact = std::move(fact);
        prep.value_fact_preps.push_back(std::move(vp));
    }
}

// Phase 2b (write lock): write precomputed value-facts. Writes only — no embed, no
// recall — so the RPC lock is held for microseconds per fact, not the seconds/minutes
// the embed+recall used to cost under the lock.
void NativeDistiller::store_value_facts(
    const std::vector<ValueFactPrep>& value_fact_preps,
    const std::string& realm,
    uint64_t episode_mem_id,
    DistillResult& result
) {
    // Operational scalars: high confidence, slow decay (like OPERATIONAL/belief).
    constexpr float kConfidence = 0.85f;
    constexpr float kDecay      = 0.001f;

    for (const auto& vp : value_fact_preps) {
        if (vp.is_dup) {
            result.value_facts_deduped++;
            continue;
        }

        uint64_t mem_id = 0;
        try {
            mem_id = field_store_->remember("operational", realm, vp.fact.content,
                                            vp.embedding, kConfidence, kDecay);
        } catch (...) { continue; }
        if (mem_id == 0) continue;

        result.value_facts_stored++;
        log("[distill]   +value-fact: " + vp.fact.identifier + " = " + vp.fact.value);

        if (episode_mem_id > 0) {
            field_store_->add_edge(mem_id, episode_mem_id, 0, 1.0f);
            field_store_->add_triplet(std::to_string(mem_id), "derived_from",
                                      std::to_string(episode_mem_id), 1.0f, mem_id);
        }
    }
}

// ── distill_session ──────────────────────────────────────────────────────────

PreparedDistillation NativeDistiller::prepare_distillation(
    const std::string& session_id,
    const std::string& transcript_path,
    const std::string& realm,
    int64_t skip_lines,
    bool queue_triggered
) {
    PreparedDistillation prep;
    prep.session_id = session_id;
    prep.realm = realm;
    // 1. Parse transcript (BOUND: cap lines per pass; remainder resumes next distill
    // via the progress event's last_line — a 108MB transcript is chunked, never a
    // single unbounded parse+distill).
    TranscriptParseOptions parse_opts;
    parse_opts.skip_lines = skip_lines;
    parse_opts.max_lines  = config_.max_lines_per_pass;
    auto turns = parser_.parse(transcript_path, parse_opts, &prep.last_line);

    if (turns.empty()) {
        prep.error = parser_.last_error().empty() ? "No turns found" : parser_.last_error();
        return prep;
    }
    if (parser_.hit_line_cap()) {
        log("[distill] bounded pass: line cap " + std::to_string(config_.max_lines_per_pass) +
            " hit at line " + std::to_string(prep.last_line) +
            " (remainder resumes on next distill)");
    }

    // 2. Check minimum turns
    int effective_min_turns = queue_triggered ? 1 : config_.min_turns;
    if (static_cast<int>(turns.size()) < effective_min_turns) {
        prep.error = "Insufficient turns: " + std::to_string(turns.size()) +
                     " < " + std::to_string(effective_min_turns);
        return prep;
    }

    log("[distill] Session " + session_id + ": " + std::to_string(turns.size()) + " turns");

    // 3. Build conversation with smart truncation
    TruncationParams trunc;
    if (config_.max_context_chars > 0) {
        trunc.max_chars  = config_.max_context_chars;
        trunc.head_chars = config_.max_context_chars / 4;
        trunc.tail_chars = (config_.max_context_chars * 3) / 4;
    } else {
        trunc.max_chars = std::numeric_limits<size_t>::max();
    }
    auto conversation = TranscriptParser::build_conversation(turns, trunc);
    prep.conversation = conversation;  // reused by the deterministic value-fact pass
    // 4. Build SSL prompt
    auto prompt = ssl::build_prompt(conversation);

    // 5. Capture turn range; pre-build episode string + embedding (no field_store writes)
    prep.start_turn = turns.empty() ? 0 : turns.front().turn_index;
    prep.end_turn   = turns.empty() ? 0 : turns.back().turn_index;
    std::ostringstream ep_content;
    ep_content << "[episode] session=" << session_id
               << " turns=" << prep.start_turn << "-" << prep.end_turn
               << " realm=" << realm;
    prep.ep_content = ep_content.str();
    if (embedder_) {
        prep.ep_embedding = embedder_(prep.ep_content);
    }

    // 6. Call LLM — the slow part, zero field_store access
    log("[distill] Calling " + config_.model + " via HTTP...");
    std::string llm_output = call_llm(prompt);
    if (llm_output.empty()) {
        prep.error = "No result from LLM";
        return prep;
    }

    // 7. Parse SSL output
    log("[distill] Processing SSL results...");
    prep.ssl_result = ssl_parser_.parse(llm_output);

    // 8. Precompute embeddings + dedup recalls (no writes, safe outside lock)
    precompute_dedup(prep);

    // 9. Precompute value-facts (extract + embed + dedup recall) — also lock-free.
    // Moved out of commit_distillation so the write lock never covers this heavy pass.
    precompute_value_facts(prep);

    prep.valid = true;
    return prep;
}

DistillResult NativeDistiller::commit_distillation(const PreparedDistillation& prep) {
    DistillResult result;
    if (!prep.valid) {
        result.error = prep.error;
        return result;
    }

    // Create episode record
    int64_t episode_id = 0;
    try {
        episode_id = static_cast<int64_t>(
            field_store_->remember("episode", prep.realm, prep.ep_content,
                                   prep.ep_embedding, 1.0f, 0.0f));
    } catch (...) {}

    if (episode_id > 0) {
        result.episode_id = episode_id;
        log("[distill]   Episode: " + std::to_string(episode_id) +
            " (turns " + std::to_string(prep.start_turn) + "-" +
            std::to_string(prep.end_turn) + ")");
    }

    // Store learnings and triplets (writes only — dedup precomputed in prepare phase)
    store_learnings(prep.ssl_result, prep.realm,
                    static_cast<uint64_t>(episode_id), prep.learning_preps, result);
    result.triplets_created = static_cast<int>(prep.ssl_result.triplets.size());

    // Value-fact write-back: precomputed (extract+embed+dedup) in prepare_distillation,
    // so this is writes-only under the lock. Gated by CHITTA_VALUE_FACTS at precompute.
    store_value_facts(prep.value_fact_preps, prep.realm,
                      static_cast<uint64_t>(episode_id), result);

    log("[distill] Session " + prep.session_id + ": Done (+" +
        std::to_string(result.learnings_stored) + " new, " +
        std::to_string(result.learnings_deduped) + " deduped, " +
        std::to_string(result.triplets_created) + " triplets, " +
        std::to_string(result.citations_linked) + " citations, +" +
        std::to_string(result.value_facts_stored) + " value-facts/" +
        std::to_string(result.value_facts_deduped) + " dedup)");

    result.last_line = prep.last_line;
    result.success = true;
    return result;
}

DistillResult NativeDistiller::distill_session(
    const std::string& session_id,
    const std::string& transcript_path,
    const std::string& realm,
    int64_t skip_lines,
    bool queue_triggered
) {
    auto prep = prepare_distillation(session_id, transcript_path, realm,
                                     skip_lines, queue_triggered);
    if (!prep.valid) {
        DistillResult r;
        r.error = prep.error;
        return r;
    }
    return commit_distillation(prep);
}

} // namespace chitta
