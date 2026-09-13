#pragma once
#include <string>
#include <vector>
#include <optional>
#include <functional>
#include <stdexcept>
#include <cstring>
#include <chrono>
#include <cstdint>
#include <utility>
#include <atomic>
#include <iostream>
#include <cmath>
#include <limits>
#include <nlohmann/json.hpp>
#include "chitta_field.h"

// Session-level recall hit (from cf_recall_session)
struct CfSessionHit {
    float score;
    uint32_t chunk_count;
    float max_chunk_score;
};

// ── FFI lock-direction invariant (load-bearing — do not break) ───────────────
// The FFI is strictly unidirectional: C++ calls INTO Rust; Rust NEVER calls
// back into C++. Cross-language deadlock is impossible only because of this:
// C++-side locks (rpc_mutex_ in field_handler.hpp, queue/trace mutexes in
// thread_pool.hpp) are always OUTERMOST, and Rust's internal parking_lot locks
// are always INNERMOST and released before any cf_* call returns. Adding a
// Rust→C++ callback (or holding a Rust lock across a call that re-enters C++)
// would create a cross-language lock cycle that no tooling here can detect.

// Explicit forward declarations for functions added in a later chitta-field version.
// These ensure the symbols are visible even if chitta_field.h was included earlier
// from a different path that predates these additions.
extern "C" {
int cf_sync(struct CfHandle* h);  // fdatasync WAL; call OFF the rpc_mutex (see field_handler)
int64_t cf_cw_refresh_sweep(struct CfHandle* h, size_t budget);
int cf_backfill_embedding(struct CfHandle* h, uint64_t memory_id,
    const float* embedding_ptr, size_t embedding_len);
int cf_pending_embeddings(struct CfHandle* h,
    uint64_t* out_ids, size_t max_ids, size_t* out_count);
// Deferred batched backfill (Step-1): stage/plan/apply. Hold rpc_mutex around
// stage + apply ONLY; call plan WITHOUT it so the HNSW search runs off the lock.
int cf_backfill_stage(struct CfHandle* h, const uint64_t* ids,
    const float* embeddings, size_t n, size_t embed_dim, size_t* out_plan_count);
int cf_backfill_plan(struct CfHandle* h);
int cf_backfill_apply(struct CfHandle* h, size_t* out_applied);
int cf_persist_delta_hnsw(struct CfHandle* h, size_t* out_persisted);
int cf_purge_orphan_embed_pending(struct CfHandle* h, size_t* out_cleared);
size_t cf_requeue_ghost_embeddings(const struct CfHandle* h);
int cf_force_clear_embed_pending(struct CfHandle* h, const uint64_t* ids, size_t count, size_t* out_cleared);
int cf_embed_coverage(struct CfHandle* h, size_t* out_eligible, size_t* out_embedded);
size_t cf_get_embedding(struct CfHandle* h, uint64_t id, float* out, size_t cap);
int cf_get_retrieval_surface(struct CfHandle* h, uint64_t id, uint8_t* buf, size_t buf_cap, size_t* written);
int cf_set_retrieval_surface(struct CfHandle* h, uint64_t id, const uint8_t* surface, size_t len);
int cf_forget_triplet(struct CfHandle* h,
    const char* subject, const char* predicate, const char* object);
int cf_ack_memory(struct CfHandle* h, uint64_t memory_id);
int cf_nack_memory(struct CfHandle* h, uint64_t memory_id);
int cf_record_outcome(struct CfHandle* h, uint64_t memory_id, int success, float weight,
    float* out_alpha, float* out_beta);
int cf_select_route(struct CfHandle* h, const char* query,
    uint64_t* out_episode_id, uint8_t* out_route);
int cf_route_feedback(struct CfHandle* h, uint64_t episode_id, float reward);
int cf_set_memory_status(struct CfHandle* h, uint64_t memory_id, uint8_t status);
int cf_set_epistemic_status(struct CfHandle* h, uint64_t memory_id, uint8_t epistemic_status);
int cf_set_affect(struct CfHandle* h, uint64_t memory_id, float valence, float arousal);
int64_t cf_compact_wal(struct CfHandle* h);
size_t  cf_wal_segment_count(const struct CfHandle* h);
int     cf_maybe_compact_wal(struct CfHandle* h, size_t threshold);
int64_t cf_prune_episodes(struct CfHandle* h, uint64_t max_age_days, size_t max_count);
uint64_t cf_promote_staged_memories(struct CfHandle* h);
char*    cf_write_gate_stats(const struct CfHandle* h);
uint64_t cf_log_symbol_event(struct CfHandle* h, const char* params_json);
char*    cf_query_symbol_events(const struct CfHandle* h, const char* params_json);
int      cf_mark_memory_invalidated(struct CfHandle* h, uint64_t memory_id, const char* reason);
char*    cf_query_cross_harness_conflicts(const struct CfHandle* h, const char* realm, uint32_t limit, float min_score);
char*    cf_symbol_stale_for_memory(const struct CfHandle* h, uint64_t memory_id);
char*    cf_memory_claim_info(const struct CfHandle* h, uint64_t memory_id, int64_t now_ms);

// Affective recall FFI
int cf_recall_semantic_ctx(struct CfHandle* h,
    const float* query_embedding, size_t embedding_len,
    const char* realm, size_t k,
    float query_valence, float query_arousal,
    CfRecallHit* buf, size_t buf_cap, size_t* written);

// Field-RAG / Modern Hopfield recall FFI
int cf_recall_field(struct CfHandle* h,
    const float* query_embedding, size_t embedding_len,
    const char* query_text, const char* realm, size_t k,
    CfRecallHit* buf, size_t buf_cap, size_t* written);

// Lane-0 atom bridge: anchor memory -> saturating-IDF co-atom partner ids +
// BM25-IDF weights (parallel out arrays, ranked by weight desc). Fused as a
// first-class RRF lane by the multi-lane recall path.
int cf_bridge_lane(struct CfHandle* h,
    uint64_t anchor_memory_id, const char* realm, size_t k,
    uint64_t* out_ids, float* out_weights, size_t cap, size_t* written);

// Hybrid recall: HNSW + BM25 fused via RRF in Rust (real hybrid path).
// start_ms/end_ms: optional authored_at_ms window gate (0/0 = disabled).
int cf_recall_with_fallback(struct CfHandle* h,
    const float* query_embedding, size_t embedding_len,
    const char* query_text, const char* realm, size_t k,
    int64_t start_ms, int64_t end_ms,
    CfRecallHit* buf, size_t buf_cap, size_t* written);

// Deterministic temporal-phrase parser ("last week", "in June", "since 2026-05-01").
// Returns JSON {"from_ms","to_ms","stripped"} (free with cf_free_string) or null.
char* cf_parse_time_window(const char* query, int64_t now_ms, int32_t tz_offset_min);

// FEP attractor network FFI
float cf_reconstruction_error(const struct CfHandle* h, uint64_t memory_id);
float cf_memory_surprise(const struct CfHandle* h, uint64_t memory_id);
int cf_search_attractor(const struct CfHandle* h, const float* embedding, size_t dim,
    size_t k, size_t settle_steps, CfRecallHit* buf, size_t buf_cap, size_t* written);
int cf_hopfield_co_retrieval(struct CfHandle* h, const uint64_t* ids, size_t count, int64_t ts_ms);
char* cf_hopfield_stats(const struct CfHandle* h);
int cf_adapt_vigilance(struct CfHandle* h, float avg_error);
int cf_chain_head(const struct CfHandle* h, uint8_t* out);
// One-shot backfill of the artifact (bridge-lane) index from stored payloads.
int cf_backfill_artifact_refs(const struct CfHandle* h, uint64_t* out_mems, uint64_t* out_assoc);
// Compiled store vector-space id (model/dim/text-format). Handle-less; used by the
// self-update gate to detect an incompatible replacement binary before execv.
uint64_t cf_compiled_vector_space_id(void);
// Compiled embedding model id (build.rs EMBED_MODEL_ID); borrowed static, do not free.
const char* cf_embed_model_id(void);
// Prune/down-weight memories by content pattern (prune-memories command). Returns a
// malloc'd JSON array [{id,kind,content}] of matches (free with cf_free_string).
char* cf_prune_memories(struct CfHandle* h, const char* patterns_json, int apply, int action);
void cf_free_string(char* s);
int cf_set_source_session(struct CfHandle* h, uint64_t memory_id, const char* session_id);
int cf_recall_session(struct CfHandle* h,
    const float* query_embedding, size_t embedding_len,
    const char* query_text, const char* realm, size_t k,
    CfSessionHit* hits_buf, size_t hits_cap, size_t* hits_written,
    char** session_ids_json_out);
int cf_recall_spreading(struct CfHandle* h, const char* query, size_t k, const char* realm,
    size_t max_nodes, size_t max_entries_per_entity, uint8_t depth,
    char* out_json, size_t out_json_len);
int cf_recall_hdc(struct CfHandle* h, const char* query, const char* realm,
    size_t k, CfRecallHit* buf, size_t buf_cap, size_t* written);

// CEC: event tape + CDAWG
int   cf_log_event(struct CfHandle* h, const char* tool, const char* entity,
    uint8_t outcome, uint64_t session_id, int64_t ts_ms);
int   cf_recall_last_action(struct CfHandle* h, const char* tool, const char* entity,
    size_t k, CfRecallHit* buf, size_t buf_cap, size_t* written);
char* cf_recall_failure_pattern(struct CfHandle* h, size_t k);
char* cf_recall_causal_antecedent(struct CfHandle* h, const char* tool, const char* entity, size_t k);
char* cf_recall_hdcbind(struct CfHandle* h, const char* known_role, const char* known_val,
                        const char* query_role, size_t k);
char* cf_recall_counterfactual(struct CfHandle* h, const char* tool, const char* entity,
                               uint8_t outcome, size_t k);
char* cf_consolidation_preview(struct CfHandle* h, size_t k);
char* cf_consolidation_pass(struct CfHandle* h);
char* cf_refutation_stats(struct CfHandle* h, size_t k);
char* cf_recall_motif_value(struct CfHandle* h, const char* tool, const char* entity, size_t k);
char* cf_executor_flush(struct CfHandle* h);
char* cf_list_policies(struct CfHandle* h, bool active_only);
int   cf_log_event_ex(struct CfHandle* h, const char* tool, const char* entity,
                      uint8_t outcome, uint64_t session_id, int64_t ts_ms,
                      uint32_t token_cost, uint32_t latency_ms, uint8_t retry_count);
int   cf_log_decision(struct CfHandle* h, const char* chosen_tool, const char* chosen_entity,
                      uint8_t chosen_outcome, const char* rejected_json,
                      float confidence_delta, int64_t ts_ms);
char* cf_recall_true_counterfactual(struct CfHandle* h, const char* tool,
                                    const char* entity, uint8_t outcome, size_t k);
char* cf_hypothesis_probes(struct CfHandle* h, size_t k);
char* cf_turiya_status(const struct CfHandle* h);
char* cf_tape_stats(const struct CfHandle* h);
char* cf_verbalize_rules(const struct CfHandle* h, size_t k);
char* cf_queue_experiments(struct CfHandle* h, size_t k);
char* cf_fep_status(const struct CfHandle* h);
char* cf_routed_recall(const struct CfHandle* h, const char* request_json);
char* cf_witness_memory(const struct CfHandle* h, uint64_t memory_id, const char* witness_kind);
char* cf_reconcile_pass(const struct CfHandle* h);
int cf_force_reindex(const struct CfHandle* h);
char* cf_harvest_scope(const struct CfHandle* h);
char* cf_seed_hdc_geometry(const struct CfHandle* h, const char* json_path);

// Skill registry FFI
int cf_skill_upload(struct CfHandle* h, const char* skill_id, const char* content,
    const char* uploaded_by, const char* tags_json, int64_t ts_ms);
char* cf_skill_read(const struct CfHandle* h, const char* skill_id, uint32_t version);
char* cf_skill_list(const struct CfHandle* h);
char* cf_skill_search(const struct CfHandle* h, const char* query, size_t limit);
int cf_skill_deprecate(struct CfHandle* h, const char* skill_id);

// Agent registry FFI
int cf_agent_upsert(struct CfHandle* h, const char* agent_id,
    const char* display_name, const char* description, int64_t ts_ms);
int cf_agent_record_activity(struct CfHandle* h, const char* agent_id, int64_t ts_ms);
int cf_agent_record_session(struct CfHandle* h, const char* agent_id, int64_t ts_ms);
char* cf_agent_get(const struct CfHandle* h, const char* agent_id);
char* cf_agent_list(const struct CfHandle* h);
int cf_agent_disable(struct CfHandle* h, const char* agent_id);

// Constraint store FFI (Layer 1)
char* cf_assert_constraint(struct CfHandle* h, const char* params_json);
int cf_retract_constraint(struct CfHandle* h, uint64_t fact_id);
char* cf_query_constraints(const struct CfHandle* h, const char* params_json);
char* cf_explain_constraint(const struct CfHandle* h, uint64_t fact_id);
int64_t cf_create_constraint_branch(struct CfHandle* h, uint64_t parent_id, const char* scope);
int cf_resolve_constraint_branch(struct CfHandle* h, uint64_t winner_id, uint64_t loser_id);

// Trigger tissue FFI (Layer 2)
int64_t cf_add_trigger(struct CfHandle* h, const char* params_json);
char* cf_fire_trigger(struct CfHandle* h, uint64_t trigger_id);
int cf_dismiss_trigger(struct CfHandle* h, uint64_t trigger_id);
char* cf_list_triggers(const struct CfHandle* h);
char* cf_evaluate_triggers(struct CfHandle* h);

// Predictive memory FFI (Layer 3)
char* cf_predict_needed(const struct CfHandle* h, size_t k);
int cf_retrain_predictor(struct CfHandle* h);
char* cf_constraint_stats(const struct CfHandle* h);

// Surprise memory FFI (Layer 4)
char* cf_record_surprise(struct CfHandle* h, const char* params_json);
char* cf_query_surprises(const struct CfHandle* h, const char* params_json);
char* cf_get_blind_spots(const struct CfHandle* h, const char* params_json);
char* cf_surprise_stats(const struct CfHandle* h);

// Epistemic debt FFI (Layer 5)
char* cf_register_debt(struct CfHandle* h, const char* params_json);
int cf_resolve_debt(struct CfHandle* h, uint64_t debt_id, const char* resolution_json);
int cf_defer_debt(struct CfHandle* h, uint64_t debt_id);
char* cf_query_debts(const struct CfHandle* h, const char* params_json);
char* cf_get_fragile_decisions(const struct CfHandle* h, const char* params_json);
char* cf_debt_stats(const struct CfHandle* h);

// Integration kernel FFI (Layer 6)
char* cf_record_feedback(struct CfHandle* h, const char* params_json);
char* cf_get_source_weights(const struct CfHandle* h, const char* params_json);
int cf_update_source_weight(struct CfHandle* h, const char* params_json);
char* cf_integration_stats(const struct CfHandle* h);

// Autonomous Learning FFI (Moves 1-6)
char* cf_surprise_learning_stats(const struct CfHandle* h);
char* cf_upsert_wisdom_candidate(struct CfHandle* h, const char* params_json);
int   cf_update_wisdom_lifecycle(struct CfHandle* h, uint64_t candidate_id, uint8_t new_state);
char* cf_query_wisdom_candidates(const struct CfHandle* h, const char* params_json);
char* cf_wisdom_promotion_stats(const struct CfHandle* h);
int   cf_attach_debt_evidence(struct CfHandle* h, uint64_t debt_id, const char* evidence_json);
int   cf_update_scorer_model(struct CfHandle* h, const char* model_json);
char* cf_learned_scorer_stats(const struct CfHandle* h);
char* cf_effective_scorer_weights(const struct CfHandle* h);
char* cf_auto_resolve_debts(struct CfHandle* h, float threshold);

// Intervention Ledger (Layer 7)
char* cf_start_intervention(struct CfHandle* h, const char* params_json);
char* cf_add_observation(struct CfHandle* h, const char* params_json);
int   cf_close_intervention(struct CfHandle* h, uint64_t intervention_id, uint8_t status);
int   cf_record_attribution(struct CfHandle* h, const char* params_json);
char* cf_query_interventions(const struct CfHandle* h, const char* params_json);
char* cf_get_intervention(const struct CfHandle* h, uint64_t intervention_id);
char* cf_intervention_stats(const struct CfHandle* h);
char* cf_list_open_interventions(const struct CfHandle* h);
int   cf_close_stale_interventions(struct CfHandle* h, int64_t threshold_ms);

// Agent Protocol Memory (Layer 8)
char* cf_register_task(struct CfHandle* h, const char* params_json);
int   cf_update_task(struct CfHandle* h, const char* params_json);
char* cf_add_delegation(struct CfHandle* h, const char* params_json);
char* cf_link_evidence(struct CfHandle* h, const char* params_json);
char* cf_add_probe(struct CfHandle* h, const char* params_json);
int   cf_resolve_probe(struct CfHandle* h, const char* params_json);
char* cf_set_criterion(struct CfHandle* h, const char* params_json);
char* cf_get_task(const struct CfHandle* h, uint64_t task_id);
char* cf_query_tasks(const struct CfHandle* h, const char* params_json);
char* cf_agent_protocol_stats(const struct CfHandle* h);
char* cf_auto_complete_tasks(struct CfHandle* h);

// Wisdom Homeostasis (Layer 9)
char* cf_enroll_wisdom_lineage(struct CfHandle* h, const char* params_json);
int   cf_transition_wisdom_lineage(struct CfHandle* h, uint64_t lineage_id, uint8_t new_state, const char* reason, uint64_t task_id);
int   cf_close_rederive(struct CfHandle* h, const char* params_json);
char* cf_query_wisdom_lineages(const struct CfHandle* h, const char* params_json);
char* cf_get_wisdom_lineage(const struct CfHandle* h, uint64_t lineage_id);
char* cf_wisdom_lineage_stats(const struct CfHandle* h);
char* cf_tick_lineage_staleness(struct CfHandle* h);
char* cf_lineage_expiry_check(const struct CfHandle* h);

// Soul REPL session store FFI
char* cf_repl_session_get(const struct CfHandle* h, const char* session_id);
int   cf_repl_session_set(struct CfHandle* h, const char* session_id, const char* namespace_json, int64_t updated_ms);
int   cf_repl_session_delete(struct CfHandle* h, const char* session_id);
char* cf_repl_session_list(const struct CfHandle* h);
// Atomic execute: get namespace, run code, persist namespace. Returns JSON.
char* cf_repl_execute(struct CfHandle* h, const char* session_id, const char* code,
                      int reset, const char* socket_path, int max_output);

// Code intel v2 FFI
int cf_upsert_code_file_v2(struct CfHandle* h,
    const char* path, const char* project, int64_t mtime,
    const char* content_hash, const char* git_commit,
    const char* git_author, int64_t git_timestamp_ms,
    int* out_changed, uint64_t* out_id);
int cf_invalidate_triplets_by_source_file(struct CfHandle* h, const char* source_file);
int cf_add_triplet_with_source(struct CfHandle* h,
    const char* subject, const char* predicate, const char* object,
    float weight, uint64_t source_memory_id, const char* source_file,
    uint64_t* out_triplet_id);

// Spectral stats FFI
int cf_spectral_stats_by_realm(struct CfHandle* h,
    uint8_t* buf, size_t buf_cap, size_t* written);
int64_t cf_trim_realm_names(struct CfHandle* h);
int cf_remap_realms(struct CfHandle* h, const char* mapping_json, bool dry_run,
    uint8_t* buf, size_t buf_cap, size_t* written);

// Contradiction detection FFI
char* cf_detect_contradictions(const struct CfHandle* h, uint64_t memory_id, const char* realm);
char* cf_scan_contradictions(const struct CfHandle* h, const char* realm, uint32_t limit);
char* cf_resolve_contradiction(const struct CfHandle* h, uint64_t winner_id, uint64_t loser_id, const char* reason);

int cf_save_spectral_snapshot(struct CfHandle* h,
    uint8_t* buf, size_t buf_cap, size_t* written);
int cf_spectral_drift(struct CfHandle* h,
    uint8_t* buf, size_t buf_cap, size_t* written);

// Bi-temporal triplet FFI
char* cf_triplet_query_as_of(struct CfHandle* h, const char* subject, int64_t world_ms);
int   cf_triplet_supersede(struct CfHandle* h, uint64_t old_id, uint64_t new_id, int64_t at_ms);
// Graph traversal FFI
char* cf_graph_traverse(struct CfHandle* h, const char* start, const char* edge_types_json,
                        size_t max_hops, size_t max_results, const char* direction,
                        size_t max_edges);
char* cf_graph_pagerank(struct CfHandle* h, const char* seeds_json, const char* edge_types_json,
                        float damping, uint8_t iterations, size_t top_k,
                        size_t max_nodes, size_t max_edges);

// Interaction Ledger FFI
int   cf_ledger_append(const struct CfHandle* h, const char* json_in, uint64_t* out_event_id);
char* cf_ledger_query(const struct CfHandle* h, const char* json_in);
int   cf_ledger_compile(const struct CfHandle* h, uint32_t* out_count);
char* cf_ledger_contradictions(const struct CfHandle* h);
int64_t cf_predicate_attach(const struct CfHandle* h, uint64_t memory_id, const char* check_cmd);
char*   cf_predicate_run(const struct CfHandle* h, uint64_t memory_id);
char*   cf_predicate_list(const struct CfHandle* h, uint64_t memory_id);

// Span Lane FFI (verbatim transcript atoms; no LLM/GPU on query path)
char* cf_span_query(struct CfHandle* h, const char* query, const char* realm, size_t k);
char* cf_span_backfill(struct CfHandle* h, const char* projects_dir);
int64_t cf_span_ingest(struct CfHandle* h, const char* transcript_path);
char* cf_span_stats(struct CfHandle* h);
// Memory↔span edge
char* cf_span_for_memory(struct CfHandle* h, uint64_t memory_id, size_t k);
int64_t cf_span_ingest_memory(struct CfHandle* h, uint64_t memory_id, const char* text, const char* realm);
char* cf_span_backfill_memories(struct CfHandle* h);
// Analogy Lane FFI (VSA over the triplet lane; structure, not surface content)
char* cf_recall_analogy(const struct CfHandle* h, const char* json_in);
char* cf_densify_backfill(struct CfHandle* h, bool apply);
char* cf_semantic_backfill(struct CfHandle* h, bool apply, size_t k, float min_cos);
char* cf_assoc_census(struct CfHandle* h);
char* cf_assoc_decay(struct CfHandle* h, uint8_t edge_type, float factor, float prune_below, bool apply);
int   cf_span_flush(struct CfHandle* h);
}

namespace chitta {

/// A single recall result from FieldStore.
struct FieldRecallHit {
    uint64_t    memory_id;
    float       score;
    float       semantic_score;
    int64_t     ts_ms;
    float       strength;
    float       confidence;
    uint32_t    access_count;
    std::string kind;
    std::string realm;
    std::string content;
    // Fraction of query content-tokens literally present in this hit (Tier-1 lexical
    // overlap, field_memory_recall.cpp:533). Lets the abstain gate credit a strong
    // lexical/exact hit that carries semantic_score 0 — otherwise a perfect keyword
    // match is capped at sigma(-0.85)~=0.30 and mislabeled UNKNOWN.
    float       lexical_score    = 0.0f;
    float       semantic_weight  = 0.0f;
    float       status_mul       = 0.0f;
    float       epistemic_mul    = 0.0f;
    float       strength_factor  = 0.0f;
    float       affect_valence   = 0.0f;
    float       affect_arousal   = 0.0f;
    float       actr_activation  = 0.0f;
    float       surprise_boost   = 1.0f;
    float       arousal_boost    = 1.0f;
    float       mood_congruence  = 1.0f;
    float       frustration_boost    = 1.0f;
    float       interference_factor  = 1.0f;
    float       spacing_boost        = 1.0f;
};

/// Thin RAII C++ wrapper around the chitta-field C FFI.
/// Manages a single CfHandle with automatic open/close lifetime.
class FieldStore {
public:
    explicit FieldStore(const std::string& data_dir, const std::string& lock_dir) {
        handle_ = cf_open(data_dir.c_str(), lock_dir.c_str());
        if (!handle_) {
            throw std::runtime_error(
                "cf_open failed: could not open chitta-field store at " + data_dir);
        }
    }

    ~FieldStore() {
        if (handle_) {
            cf_flush(handle_);
            cf_close(handle_);
            handle_ = nullptr;
        }
    }

    // Non-copyable, movable
    FieldStore(const FieldStore&) = delete;
    FieldStore& operator=(const FieldStore&) = delete;
    FieldStore(FieldStore&& o) noexcept : handle_(o.handle_) { o.handle_ = nullptr; }

    /// Store a new memory. Returns its stable MemoryId.
    uint64_t remember(
        const std::string&        kind,
        const std::string&        realm,
        const std::string&        content,
        const std::vector<float>& embedding,
        float                     confidence     = 1.0f,
        float                     decay_rate     = 0.001f,
        int64_t                   authored_at_ms = 0
    ) {
        uint64_t id = 0;
        int r = cf_put_memory(
            handle_,
            kind.c_str(), realm.c_str(),
            reinterpret_cast<const uint8_t*>(content.data()), content.size(),
            embedding.data(), embedding.size(),
            confidence, decay_rate, authored_at_ms,
            &id
        );
        cf_checked(r, __func__);
        return id;
    }

    /// Set memory lifecycle status: 0=Active 1=Superseded 2=Contradicted 3=Archived 4=Proposed 5=Observed 6=Verified
    void set_memory_status(uint64_t id, uint8_t status) {
        cf_set_memory_status(handle_, id, status);
    }

    /// Set epistemic status: 0=UserStated 1=ToolDerived 2=ModelInferred 3=AutonomousSynthesis
    void set_epistemic_status(uint64_t id, uint8_t es) {
        cf_set_epistemic_status(handle_, id, es);
    }

    /// Set affect dimensions: valence [-1,1], arousal [0,1].
    void set_affect(uint64_t id, float valence, float arousal) {
        cf_set_affect(handle_, id, valence, arousal);
    }

    /// Compact WAL: save full snapshot then delete covered segments. Returns deleted count or -1.
    int64_t compact_wal() {
        return cf_compact_wal(handle_);
    }

    /// Count WAL segment files.
    size_t wal_segment_count() const {
        return cf_wal_segment_count(handle_);
    }

    /// Compact WAL if segments > threshold and 1h cooldown elapsed.
    /// Returns 1=compacted, 0=skipped, -1=error.
    int maybe_compact_wal(size_t threshold = 50) {
        return cf_maybe_compact_wal(handle_, threshold);
    }

    /// Prune episode memories older than max_age_days (strength<0.3) and cap at max_count.
    int64_t prune_episodes(uint64_t max_age_days = 90, size_t max_count = 10000) {
        return cf_prune_episodes(handle_, max_age_days, max_count);
    }

    /// Promote staged memories that have been recalled; prune stale ones.
    /// Returns {promoted, pruned} packed as (promoted<<32)|pruned.
    std::pair<uint32_t,uint32_t> promote_staged_memories() {
        uint64_t r = cf_promote_staged_memories(handle_);
        return {static_cast<uint32_t>(r >> 32), static_cast<uint32_t>(r & 0xFFFFFFFF)};
    }

    /// Return JSON with staged memory count and oldest staged age.
    std::string write_gate_stats_json() const {
        char* s = cf_write_gate_stats(handle_);
        if (!s) { cf_soft(-1, __func__); return "{}"; }
        std::string r(s);
        cf_free_string(s);
        return r;
    }

    uint64_t log_symbol_event(const std::string& params_json) {
        return cf_log_symbol_event(handle_, params_json.c_str());
    }

    std::string query_symbol_events(const std::string& params_json) const {
        char* s = cf_query_symbol_events(handle_, params_json.c_str());
        if (!s) { cf_soft(-1, __func__); return "[]"; }
        std::string r(s);
        cf_free_string(s);
        return r;
    }

    bool mark_memory_invalidated(uint64_t memory_id, const std::string& reason) {
        return cf_mark_memory_invalidated(handle_, memory_id, reason.c_str()) == 0;
    }

    std::string query_cross_harness_conflicts(const std::string& realm, uint32_t limit, float min_score) const {
        char* s = cf_query_cross_harness_conflicts(handle_, realm.c_str(), limit, min_score);
        if (!s) { cf_soft(-1, __func__); return "[]"; }
        std::string r(s); cf_free_string(s); return r;
    }

    std::string symbol_stale_for_memory_json(uint64_t memory_id) const {
        char* s = cf_symbol_stale_for_memory(handle_, memory_id);
        if (!s) return R"({"stale":false,"reason":null})";
        std::string r(s); cf_free_string(s); return r;
    }

    std::string memory_claim_info_json(uint64_t memory_id, int64_t now_ms) const {
        char* s = cf_memory_claim_info(handle_, memory_id, now_ms);
        if (!s) { cf_soft(-1, __func__); return "{}"; }
        std::string r(s); cf_free_string(s); return r;
    }

    /// Backfill the bridge-lane artifact index from stored payloads. Returns
    /// (memories_touched, associations_created). One-shot bootstrap.
    std::pair<uint64_t, uint64_t> backfill_artifact_refs() const {
        uint64_t mems = 0, assoc = 0;
        cf_backfill_artifact_refs(handle_, &mems, &assoc);
        return {mems, assoc};
    }

    /// Return the chain tip hash as a 64-char hex string. Empty if only V1 data.
    std::string chain_head() const {
        uint8_t buf[32] = {};
        if (cf_chain_head(handle_, buf) != 0) return "";
        bool all_zero = true;
        for (int i = 0; i < 32; ++i) { if (buf[i]) { all_zero = false; break; } }
        if (all_zero) return "";
        static const char* hex = "0123456789abcdef";
        std::string out(64, '0');
        for (int i = 0; i < 32; ++i) {
            out[i*2]   = hex[buf[i] >> 4];
            out[i*2+1] = hex[buf[i] & 0xf];
        }
        return out;
    }

    // ── FEP Attractor Network ───────────────────────────────────────────

    /// Get reconstruction error (surprise) for a memory. Returns [0,1], -1 on error.
    float reconstruction_error(uint64_t memory_id) {
        return cf_reconstruction_error(handle_, memory_id);
    }

    /// Get cached surprise score from memory state.
    float memory_surprise(uint64_t memory_id) {
        return cf_memory_surprise(handle_, memory_id);
    }

    /// Record co-retrieval batch in Hopfield network.
    void hopfield_co_retrieval(const std::vector<uint64_t>& ids, int64_t ts_ms) {
        cf_hopfield_co_retrieval(handle_, ids.data(), ids.size(), ts_ms);
    }

    /// Adapt cortical vigilance based on aggregate reconstruction error.
    void adapt_vigilance(float avg_error) {
        cf_adapt_vigilance(handle_, avg_error);
    }

    /// Backfill embedding for a memory stored without one (embed_pending=true).
    void backfill_embedding(uint64_t id, const std::vector<float>& embedding) {
        int r = cf_backfill_embedding(handle_, id,
            embedding.data(), embedding.size());
        cf_checked(r, __func__);
    }

    /// Deferred batched backfill — Phase 1 (durable stage). Call under acquire_lock().
    /// `ids[i]` pairs with `embeddings[i*embed_dim .. (i+1)*embed_dim]`. Returns the
    /// number of ids still needing a global-HNSW plan.
    size_t backfill_stage(const std::vector<uint64_t>& ids,
                          const std::vector<float>& embeddings, size_t embed_dim) {
        size_t plan_count = 0;
        int r = cf_backfill_stage(handle_, ids.data(), embeddings.data(),
                                  ids.size(), embed_dim, &plan_count);
        cf_checked(r, __func__);
        return plan_count;
    }
    /// Phase 2 (plan) — call WITHOUT acquire_lock() so recall is not blocked.
    void backfill_plan() { cf_checked(cf_backfill_plan(handle_), __func__); }
    /// Phase 3 (apply) — call under acquire_lock(). Returns memories backfilled.
    size_t backfill_apply() {
        size_t applied = 0;
        cf_checked(cf_backfill_apply(handle_, &applied), __func__);
        return applied;
    }
    /// Persist the delta HNSW sidecar so a restart LOADS it instead of cold-
    /// reinserting from scratch. Call WITHOUT acquire_lock() (off-lock safe).
    /// Returns delta nodes written (0 = nothing to persist / non-fatal failure).
    size_t persist_delta_hnsw() {
        size_t persisted = 0;
        cf_checked(cf_persist_delta_hnsw(handle_, &persisted), __func__);
        return persisted;
    }

    /// Return IDs of memories waiting for an embedding (embed_pending=true).
    std::vector<uint64_t> pending_embeddings(size_t limit = 100) {
        std::vector<uint64_t> ids(limit);
        size_t written = 0;
        cf_pending_embeddings(handle_, ids.data(), limit, &written);
        ids.resize(written);
        return ids;
    }

    /// Strengthen a memory (positive feedback).
    void strengthen(uint64_t id, float amount = 0.1f) {
        cf_update_state(handle_, id,
            amount,
            std::numeric_limits<float>::quiet_NaN(),   // confidence_delta — no change
            std::numeric_limits<float>::quiet_NaN(),   // decay_rate — no change
            1,                                         // touch
            static_cast<int8_t>(-1)                    // pin — no change
        );
    }

    /// Weaken a memory (negative feedback).
    void weaken(uint64_t id, float amount = 0.1f) {
        cf_update_state(handle_, id,
            -amount,
            std::numeric_limits<float>::quiet_NaN(),
            std::numeric_limits<float>::quiet_NaN(),
            0,
            static_cast<int8_t>(-1)
        );
    }

    /// Update confidence directly (used to promote provisional→durable).
    void update_confidence(uint64_t id, float delta) {
        cf_update_state(handle_, id,
            std::numeric_limits<float>::quiet_NaN(),
            delta,
            std::numeric_limits<float>::quiet_NaN(),
            0,
            static_cast<int8_t>(-1)
        );
    }

    /// Touch (update access time without changing strength).
    void touch(uint64_t id) {
        cf_update_state(handle_, id,
            std::numeric_limits<float>::quiet_NaN(),
            std::numeric_limits<float>::quiet_NaN(),
            std::numeric_limits<float>::quiet_NaN(),
            1,
            static_cast<int8_t>(-1)
        );
    }

    /// Soft-delete a memory.
    void forget(uint64_t id) {
        cf_forget(handle_, id);
    }

    // Prune/down-weight memories by content pattern. Returns JSON array of matches
    // [{id,kind,content}]. action: 0=delete (forget), 1=archive (down-weight).
    std::string prune_memories(const std::string& patterns_json, bool apply, int action) {
        char* r = cf_prune_memories(handle_, patterns_json.c_str(), apply ? 1 : 0, action);
        if (!r) return std::string();
        std::string s(r);
        cf_free_string(r);
        return s;
    }
    void ack_memory(uint64_t id) {
        cf_ack_memory(handle_, id);
    }
    void nack_memory(uint64_t id) {
        cf_nack_memory(handle_, id);
    }

    // Accrue one outcome observation into the memory's Beta utility posterior.
    // Returns false if the memory does not exist; alpha/beta receive the update.
    bool record_outcome(uint64_t id, bool success, float weight,
                        float& alpha, float& beta) {
        return cf_record_outcome(handle_, id, success ? 1 : 0, weight, &alpha, &beta) == 0;
    }

    /// Semantic recall — find k most similar memories to query embedding.
    std::vector<FieldRecallHit> recall(
        const std::vector<float>& query_embedding,
        size_t                    k,
        const std::string&        realm = "",
        bool                      no_learn = false
    ) {
        constexpr size_t MAX_HITS = 256;
        CfRecallHit buf[MAX_HITS];
        size_t written = 0;

        const char* realm_ptr = realm.empty() ? nullptr : realm.c_str();
        int r = cf_recall_semantic(
            handle_,
            query_embedding.data(), query_embedding.size(),
            realm_ptr, k,
            buf, MAX_HITS, &written,
            no_learn
        );
        cf_checked(r, __func__);
        return hits_to_results(buf, written);
    }

    /// Hybrid recall: HNSW semantic + BM25 fused via RRF internally.
    /// Prefer over recall() for strategy="hybrid" — this runs the real
    /// recall_with_fallback path, not semantic-only.
    /// start_ms/end_ms (0/0 = off): authored_at_ms window GATE — the window
    /// gates candidates, semantic relevance ranks (never recency-sorts).
    std::vector<FieldRecallHit> recall_with_fallback(
        const std::vector<float>& query_embedding,
        const std::string&        query_text,
        size_t                    k,
        const std::string&        realm = "",
        int64_t                   start_ms = 0,
        int64_t                   end_ms = 0
    ) {
        constexpr size_t MAX_HITS = 256;
        CfRecallHit buf[MAX_HITS];
        size_t written = 0;
        const char* realm_ptr = realm.empty() ? nullptr : realm.c_str();
        int r = cf_recall_with_fallback(
            handle_,
            query_embedding.data(), query_embedding.size(),
            query_text.c_str(), realm_ptr, k,
            start_ms, end_ms,
            buf, MAX_HITS, &written
        );
        cf_checked(r, __func__);
        return hits_to_results(buf, written);
    }

    /// Parse a natural-language temporal phrase ("last week", "in June") out of
    /// a query. Returns empty string when no phrase matches (the no-match →
    /// unchanged-behavior guarantee); otherwise JSON {"from_ms","to_ms","stripped"}.
    std::string parse_time_window(const std::string& query, int64_t now_ms, int32_t tz_offset_min) {
        char* raw = cf_parse_time_window(query.c_str(), now_ms, tz_offset_min);
        if (!raw) return "";
        std::string out(raw);
        cf_free_string(raw);
        return out;
    }

    /// Semantic recall with affective context — mood-congruent retrieval.
    /// Pass NaN for valence/arousal to disable affect matching.
    std::vector<FieldRecallHit> recall_ctx(
        const std::vector<float>& query_embedding,
        size_t                    k,
        const std::string&        realm,
        float                     query_valence,
        float                     query_arousal
    ) {
        constexpr size_t MAX_HITS = 256;
        CfRecallHit buf[MAX_HITS];
        size_t written = 0;

        const char* realm_ptr = realm.empty() ? nullptr : realm.c_str();
        int r = cf_recall_semantic_ctx(
            handle_,
            query_embedding.data(), query_embedding.size(),
            realm_ptr, k,
            query_valence, query_arousal,
            buf, MAX_HITS, &written
        );
        cf_checked(r, __func__);
        return hits_to_results(buf, written);
    }

    /// Lane-0 atom bridge: co-atom partners of `anchor` with their BM25-IDF
    /// weights (parallel vectors, ranked by weight desc). Empty when the bridge
    /// is disabled or the anchor carries no shared atom.
    std::pair<std::vector<uint64_t>, std::vector<float>>
    bridge_lane(uint64_t anchor, const std::string& realm, size_t k) {
        constexpr size_t MAX = 64;
        uint64_t ids[MAX];
        float    weights[MAX];
        size_t   written = 0;
        const char* realm_ptr = realm.empty() ? nullptr : realm.c_str();
        int r = cf_bridge_lane(handle_, anchor, realm_ptr, std::min(k, MAX),
                               ids, weights, MAX, &written);
        cf_checked(r, __func__);
        return {std::vector<uint64_t>(ids, ids + written),
                std::vector<float>(weights, weights + written)};
    }

    /// Field-RAG recall — Modern Hopfield / DAM relaxation over HNSW candidates.
    std::vector<FieldRecallHit> recall_field(
        const std::vector<float>& query_embedding,
        const std::string&        query_text,
        size_t                    k,
        const std::string&        realm = ""
    ) {
        constexpr size_t MAX_HITS = 256;
        CfRecallHit buf[MAX_HITS];
        size_t written = 0;
        const char* realm_ptr  = realm.empty()      ? nullptr : realm.c_str();
        const char* qtext_ptr  = query_text.empty() ? nullptr : query_text.c_str();
        int r = cf_recall_field(
            handle_,
            query_embedding.data(), query_embedding.size(),
            qtext_ptr, realm_ptr, k,
            buf, MAX_HITS, &written
        );
        cf_checked(r, __func__);
        return hits_to_results(buf, written);
    }

    /// Temporal recall — find memories in a time range.
    std::vector<FieldRecallHit> recall_temporal(
        int64_t            start_ms,
        int64_t            end_ms,
        size_t             limit,
        const std::string& realm = ""
    ) {
        constexpr size_t MAX_HITS = 256;
        CfRecallHit buf[MAX_HITS];
        size_t written = 0;

        const char* realm_ptr = realm.empty() ? nullptr : realm.c_str();
        int r = cf_recall_temporal(handle_, start_ms, end_ms, realm_ptr, limit,
                                   buf, MAX_HITS, &written);
        cf_checked(r, __func__);
        return hits_to_results(buf, written);
    }

    /// Bridge query: discover active entities via EventTape, recall their memories.
    std::vector<FieldRecallHit> recall_temporal_events(
        int64_t start_ms,
        int64_t end_ms,
        size_t  limit
    ) {
        constexpr size_t MAX_HITS = 256;
        CfRecallHit buf[MAX_HITS];
        size_t written = 0;
        int r = cf_recall_temporal_events(handle_, start_ms, end_ms, limit,
                                          buf, MAX_HITS, &written);
        cf_checked(r, __func__);
        return hits_to_results(buf, written);
    }

    /// Artifact recall — find memories associated with a file path.
    std::vector<FieldRecallHit> recall_artifact(
        const std::string& path,
        size_t             limit
    ) {
        constexpr size_t MAX_HITS = 256;
        CfRecallHit buf[MAX_HITS];
        size_t written = 0;

        int r = cf_recall_artifact(handle_, path.c_str(), limit,
                                   buf, MAX_HITS, &written);
        cf_checked(r, __func__);
        return hits_to_results(buf, written);
    }

    /// Expand associations from seed memory IDs (spreading activation).
    std::vector<FieldRecallHit> expand_associations(
        const std::vector<uint64_t>& seed_ids,
        size_t                       max_hops = 2,
        size_t                       limit    = 20
    ) {
        constexpr size_t MAX_HITS = 256;
        CfRecallHit buf[MAX_HITS];
        size_t written = 0;

        int r = cf_expand_associations(handle_,
            seed_ids.data(), seed_ids.size(),
            max_hops, limit,
            buf, MAX_HITS, &written);
        cf_checked(r, __func__);
        return hits_to_results(buf, written);
    }

    /// Personalized-PageRank injection lane: seed with the top fused hits and
    /// their weights, return up to top_g graph-reachable (id, score) pairs with
    /// the seeds excluded (already ranked by the caller).
    std::vector<std::pair<uint64_t, float>> ppr_lane(
        const std::vector<uint64_t>& seed_ids,
        const std::vector<float>&    seed_weights,
        size_t                       top_g
    ) {
        std::vector<std::pair<uint64_t, float>> out;
        if (seed_ids.empty() || top_g == 0) return out;
        std::vector<uint64_t> ids(top_g);
        std::vector<float>    scores(top_g);
        size_t written = 0;
        int r = cf_ppr_lane(handle_,
            seed_ids.data(), seed_weights.data(), seed_ids.size(),
            top_g, ids.data(), scores.data(), top_g, &written);
        cf_checked(r, __func__);
        out.reserve(written);
        for (size_t i = 0; i < written; ++i) out.emplace_back(ids[i], scores[i]);
        return out;
    }

    /// Add an association edge between memories.
    /// edge_type: 0=DerivedFrom, 1=SameSession, 2=SameArtifact, 3=CoRetrieved
    void add_edge(uint64_t src, uint64_t dst, uint8_t edge_type = 3, float weight = 0.5f) {
        cf_add_assoc_edge(handle_, src, dst, edge_type, weight);
    }

    /// Register a file artifact, returns its artifact ID.
    uint64_t upsert_artifact(const std::string& path) {
        uint64_t id = 0;
        cf_upsert_artifact(handle_, path.c_str(), &id);
        return id;
    }

    /// Get content string for a memory. Retries with a heap buffer when the
    /// content exceeds the stack buffer (cf_get_content reports the required
    /// size via `written` on -2).
    std::string get_content(uint64_t id) {
        char buf[65536];
        size_t written = 0;
        int r = cf_get_content(handle_, id,
                               reinterpret_cast<uint8_t*>(buf), sizeof(buf), &written);
        if (r == 0) return std::string(buf, written);
        if (r == -2 && written > 0) {
            std::vector<uint8_t> big(written + 1);
            if (cf_get_content(handle_, id, big.data(), big.size(), &written) == 0)
                return std::string(reinterpret_cast<char*>(big.data()), written);
        }
        return "";
    }

    /// Stage B: the natural-language retrieval surface for a memory, or "" if none
    /// is stored (caller then embeds the telegraphic content). Mirrors get_content's
    /// stack-then-heap buffer retry on -2.
    std::string get_retrieval_surface(uint64_t id) {
        char buf[65536];
        size_t written = 0;
        int r = cf_get_retrieval_surface(handle_, id,
                                         reinterpret_cast<uint8_t*>(buf), sizeof(buf), &written);
        if (r == 0) return std::string(buf, written);
        if (r == -2 && written > 0) {
            std::vector<uint8_t> big(written + 1);
            if (cf_get_retrieval_surface(handle_, id, big.data(), big.size(), &written) == 0)
                return std::string(reinterpret_cast<char*>(big.data()), written);
        }
        return "";
    }

    /// Stage B: set (non-empty) or clear (empty) a memory's retrieval surface.
    void set_retrieval_surface(uint64_t id, const std::string& surface) {
        cf_set_retrieval_surface(handle_, id,
                                 reinterpret_cast<const uint8_t*>(surface.data()), surface.size());
    }

    /// Budgeted background competitive-weight refresh (consolidation sweep).
    size_t cw_refresh_sweep(size_t budget) {
        int64_t n = cf_cw_refresh_sweep(handle_, budget);
        return n > 0 ? static_cast<size_t>(n) : 0;
    }

    /// Number of live memories.
    size_t memory_count() const {
        return cf_memory_count(handle_);
    }
    size_t raw_memory_count() const {
        return cf_raw_memory_count(handle_);
    }
    size_t purge_orphan_embed_pending() {
        size_t n = 0;
        cf_purge_orphan_embed_pending(handle_, &n);
        return n;
    }

    size_t force_clear_embed_pending(const std::vector<uint64_t>& ids) {
        size_t n = 0;
        cf_force_clear_embed_pending(handle_, ids.data(), ids.size(), &n);
        return n;
    }

    // The stored vector, as the index holds it. Returns floats written (0 = no embedding).
    size_t get_embedding(uint64_t id, float* out, size_t cap) const {
        return cf_get_embedding(handle_, id, out, cap);
    }

    // (eligible, embedded) — memories that should have a vector vs those that do.
    std::pair<size_t, size_t> embed_coverage() const {
        size_t eligible = 0, embedded = 0;
        cf_embed_coverage(handle_, &eligible, &embedded);
        return {eligible, embedded};
    }

    size_t requeue_ghost_embeddings() {
        return cf_requeue_ghost_embeddings(handle_);
    }

    int64_t requeue_all_embeddings(const char* model_id, size_t model_id_len) {
        return cf_requeue_all_embeddings(handle_, model_id, model_id_len);
    }

    size_t raw_pending_count() const {
        return cf_pending_count(handle_);
    }

    /// Flush manifest to disk.
    void flush() {
        cf_flush(handle_);
    }

    /// fdatasync the WAL to disk (durable). Call this AFTER releasing the rpc_mutex so the
    /// per-write disk sync doesn't block recall — put_memory only flush_buf()s under the lock.
    void sync() {
        cf_sync(handle_);
    }

    /// Ingest new ops from foreign-instance segment files on shared storage.
    /// Returns number of ops applied, or -1 on error.
    int sync_foreign() {
        return cf_sync_foreign(handle_);
    }

    /// Phase 1 of sync_foreign: read peer ops off shared storage into a pending buffer.
    /// Blocking disk I/O — call WITHOUT the rpc lock. Returns ops now pending, or -1.
    int sync_foreign_collect() {
        return cf_sync_foreign_collect(handle_);
    }

    /// Phase 2 of sync_foreign: apply the pending ops. In-memory, takes ~40 write guards —
    /// call WITH the exclusive rpc lock. Returns ops applied, or -1.
    int sync_foreign_apply() {
        return cf_sync_foreign_apply(handle_);
    }

    /// Apply outcome feedback for a retrieval episode (route learning).
    void feedback(uint64_t episode_id, float reward) {
        cf_feedback(handle_, episode_id, reward);
    }

    /// Get recommended working memory window size for a session type.
    size_t recommended_window(const std::string& session_type) const {
        return cf_recommended_window(handle_, session_type.c_str());
    }

    /// Deterministic provenance lookup (keyed lane, capability #1).
    /// Exact-key resolution of a `[done]` record by content-hash and/or input
    /// path — bypasses the fuzzy retriever entirely. Returns {found, memory_id,
    /// content}. `sha` is tried first, then `input`; either may be empty.
    struct ProvenanceHit { bool found = false; uint64_t memory_id = 0; std::string content; };
    ProvenanceHit provenance_lookup(const std::string& sha, const std::string& input) {
        size_t cap = 8192;
        std::vector<uint8_t> buf(cap);
        uint64_t id = 0;
        size_t written = 0;
        const char* sha_p = sha.empty() ? nullptr : sha.c_str();
        const char* in_p  = input.empty() ? nullptr : input.c_str();
        int r = cf_provenance_lookup(handle_, sha_p, in_p, &id, buf.data(), buf.size(), &written);
        if (r == 1) return {};                       // clean miss
        if (r == -2) {                               // record longer than buffer: grow + retry
            buf.resize(written + 1);
            r = cf_provenance_lookup(handle_, sha_p, in_p, &id, buf.data(), buf.size(), &written);
            if (r == 1) return {};
        }
        cf_checked(r, __func__);
        return {true, id, std::string(reinterpret_cast<char*>(buf.data()), written)};
    }

    /// Deterministic correction lookup (keyed lane, capability #2 — durable
    /// corrections with override semantics). Exact-key bigram probe of free turn
    /// text against stored [correction] mistake-phrase triggers — reserves an
    /// injection slot for a correction whose mistake recurs, bypassing the fuzzy
    /// retriever entirely. Returns {found, memory_id, content} where content is
    /// up to 3 fired corrections (newest first), joined by "\n---\n".
    struct CorrectionHit { bool found = false; uint64_t memory_id = 0; std::string content; };
    CorrectionHit correction_check(const std::string& text) {
        size_t cap = 8192;
        std::vector<uint8_t> buf(cap);
        uint64_t id = 0;
        size_t written = 0;
        int r = cf_correction_check(handle_, text.c_str(), &id, buf.data(), buf.size(), &written);
        if (r == 1) return {};                       // clean miss
        if (r == -2) {                               // fired set longer than buffer: grow + retry
            buf.resize(written + 1);
            r = cf_correction_check(handle_, text.c_str(), &id, buf.data(), buf.size(), &written);
            if (r == 1) return {};
        }
        cf_checked(r, __func__);
        return {true, id, std::string(reinterpret_cast<char*>(buf.data()), written)};
    }

    /// Deterministic task-state lookup (keyed lane, capability #3 — task
    /// hand-off across discontinuous sessions). Exact-key resolution of a
    /// `[task]` record by its slug — bypasses the fuzzy retriever entirely and
    /// returns the LATEST record (status evolves). Returns {found, memory_id,
    /// content}; `id` accepts a raw slug or a `task:<slug>` key.
    struct TaskStateHit { bool found = false; uint64_t memory_id = 0; std::string content; };
    TaskStateHit task_state_lookup(const std::string& id) {
        size_t cap = 8192;
        std::vector<uint8_t> buf(cap);
        uint64_t mid = 0;
        size_t written = 0;
        int r = cf_task_state_lookup(handle_, id.c_str(), &mid, buf.data(), buf.size(), &written);
        if (r == 1) return {};                       // clean miss
        if (r == -2) {                               // record longer than buffer: grow + retry
            buf.resize(written + 1);
            r = cf_task_state_lookup(handle_, id.c_str(), &mid, buf.data(), buf.size(), &written);
            if (r == 1) return {};
        }
        cf_checked(r, __func__);
        return {true, mid, std::string(reinterpret_cast<char*>(buf.data()), written)};
    }

    /// BM25 keyword recall.
    std::vector<FieldRecallHit> recall_keyword(const std::string& query, size_t k,
                                               const std::string& realm = "",
                                               bool no_learn = false) {
        constexpr size_t MAX_HITS = 256;
        CfRecallHit buf[MAX_HITS];
        size_t written = 0;
        int r = cf_recall_keyword(handle_, query.c_str(), k,
                                  realm.empty() ? nullptr : realm.c_str(),
                                  buf, MAX_HITS, &written,
                                  no_learn);
        cf_checked(r, __func__);
        return hits_to_results(buf, written);
    }

    /// HDC (hyperdimensional) recall — best-effort, returns empty on error.
    std::vector<FieldRecallHit> recall_hdc(const std::string& query, size_t k,
                                           const std::string& realm = "") {
        constexpr size_t MAX_HITS = 256;
        CfRecallHit buf[MAX_HITS];
        size_t written = 0;
        int r = cf_recall_hdc(handle_, query.c_str(),
                              realm.empty() ? nullptr : realm.c_str(),
                              k, buf, MAX_HITS, &written);
        if (!cf_soft(r, __func__)) return {};
        return hits_to_results(buf, written);
    }

    /// Log a structured action event to the CEC tape and extend the CDAWG.
    void log_event(const std::string& tool, const std::string& entity,
                   uint8_t outcome, uint64_t session_id = 0, int64_t ts_ms = 0) {
        cf_log_event(handle_, tool.c_str(), entity.c_str(), outcome, session_id, ts_ms);
    }

    /// Recall last k occurrences of (tool, entity) from the CDAWG — best-effort.
    std::vector<FieldRecallHit> recall_last_action(const std::string& tool,
                                                   const std::string& entity, size_t k) {
        constexpr size_t MAX_HITS = 256;
        CfRecallHit buf[MAX_HITS];
        size_t written = 0;
        int r = cf_recall_last_action(handle_, tool.c_str(), entity.c_str(),
                                      k, buf, MAX_HITS, &written);
        if (!cf_soft(r, __func__)) return {};
        return hits_to_results(buf, written);
    }

    /// Return top-k failure patterns as JSON string — best-effort, caller frees with cf_free_string.
    std::string recall_failure_pattern_json(size_t k) {
        char* raw = cf_recall_failure_pattern(handle_, k);
        if (!raw) { cf_soft(-1, __func__); return "[]"; }
        std::string result(raw);
        cf_free_string(raw);
        return result;
    }

    /// Return top-k PMI-ranked causal antecedents for (tool, entity) as JSON.
    /// JSON array: [{rank, content, pmi, count}]
    std::string recall_causal_antecedent_json(const std::string& tool,
                                              const std::string& entity,
                                              size_t k) {
        char* raw = cf_recall_causal_antecedent(handle_, tool.c_str(), entity.c_str(), k);
        if (!raw) { cf_soft(-1, __func__); return "[]"; }
        std::string result(raw);
        cf_free_string(raw);
        return result;
    }

    std::string recall_hdcbind_json(const std::string& known_role,
                                    const std::string& known_val,
                                    const std::string& query_role,
                                    size_t k) {
        char* raw = cf_recall_hdcbind(handle_, known_role.c_str(), known_val.c_str(),
                                      query_role.c_str(), k);
        if (!raw) { cf_soft(-1, __func__); return "[]"; }
        std::string result(raw);
        cf_free_string(raw);
        return result;
    }

    std::string recall_counterfactual_json(const std::string& tool,
                                           const std::string& entity,
                                           uint8_t outcome, size_t k) {
        char* raw = cf_recall_counterfactual(handle_, tool.c_str(), entity.c_str(), outcome, k);
        if (!raw) { cf_soft(-1, __func__); return "[]"; }
        std::string result(raw);
        cf_free_string(raw);
        return result;
    }

    std::string consolidation_preview_json(size_t k = 5) {
        char* raw = cf_consolidation_preview(handle_, k);
        if (!raw) { cf_soft(-1, __func__); return "[]"; }
        std::string result(raw);
        cf_free_string(raw);
        return result;
    }

    std::string consolidation_pass_json() {
        char* raw = cf_consolidation_pass(handle_);
        if (!raw) { cf_soft(-1, __func__); return "{}"; }
        std::string result(raw);
        cf_free_string(raw);
        return result;
    }

    std::string refutation_stats_json(size_t k = 10) {
        char* raw = cf_refutation_stats(handle_, k);
        if (!raw) return "refutation_stats: no data";
        std::string result(raw);
        cf_free_string(raw);
        return result;
    }

    std::string recall_motif_value_json(const std::string& tool, const std::string& entity, size_t k = 5) {
        char* raw = cf_recall_motif_value(handle_, tool.c_str(), entity.c_str(), k);
        if (!raw) { cf_soft(-1, __func__); return "[]"; }
        std::string result(raw);
        cf_free_string(raw);
        return result;
    }

    // Span Lane: verbatim transcript atoms, realm-scoped, no LLM/GPU on query path.
    std::string span_query_json(const std::string& query, const std::string& realm, size_t k = 6) {
        char* raw = cf_span_query(handle_, query.c_str(),
                                  realm.empty() ? nullptr : realm.c_str(), k);
        if (!raw) { cf_soft(-1, __func__); return "[]"; }
        std::string result(raw);
        cf_free_string(raw);
        return result;
    }

    std::string span_backfill_json(const std::string& projects_dir) {
        char* raw = cf_span_backfill(handle_, projects_dir.c_str());
        if (!raw) { cf_soft(-1, __func__); return "{}"; }
        std::string result(raw);
        cf_free_string(raw);
        return result;
    }

    int64_t span_ingest(const std::string& transcript_path) {
        return cf_span_ingest(handle_, transcript_path.c_str());
    }

    std::string span_stats_json() {
        char* raw = cf_span_stats(handle_);
        if (!raw) { cf_soft(-1, __func__); return "{}"; }
        std::string result(raw);
        cf_free_string(raw);
        return result;
    }

    // Memory→span forward edge: the verbatim atoms a recalled memory references.
    std::string span_for_memory_json(uint64_t memory_id, size_t k = 6) {
        char* raw = cf_span_for_memory(handle_, memory_id, k);
        if (!raw) { cf_soft(-1, __func__); return "[]"; }
        std::string result(raw);
        cf_free_string(raw);
        return result;
    }

    // Analogy Lane: a:b :: c:? and structural-signature match over the triplet
    // lane. Args and reply are JSON (see cf_recall_analogy); "{}" on failure,
    // which the caller distinguishes from a hit-less reply by the absent
    // "results" key.
    std::string recall_analogy_json(const std::string& args_json) const {
        char* raw = cf_recall_analogy(handle_, args_json.c_str());
        if (!raw) { cf_soft(-1, __func__); return "{}"; }
        std::string result(raw);
        cf_free_string(raw);
        return result;
    }

    int64_t span_ingest_memory(uint64_t memory_id, const std::string& text, const std::string& realm) {
        return cf_span_ingest_memory(handle_, memory_id, text.c_str(),
                                     realm.empty() ? nullptr : realm.c_str());
    }

    std::string span_backfill_memories_json() {
        char* raw = cf_span_backfill_memories(handle_);
        if (!raw) { cf_soft(-1, __func__); return "{}"; }
        std::string result(raw);
        cf_free_string(raw);
        return result;
    }

    std::string densify_backfill_json(bool apply) {
        char* raw = cf_densify_backfill(handle_, apply);
        if (!raw) { cf_soft(-1, __func__); return "{}"; }
        std::string result(raw);
        cf_free_string(raw);
        return result;
    }

    std::string semantic_backfill_json(bool apply, size_t k, float min_cos) {
        char* raw = cf_semantic_backfill(handle_, apply, k, min_cos);
        if (!raw) { cf_soft(-1, __func__); return "{}"; }
        std::string result(raw);
        cf_free_string(raw);
        return result;
    }

    std::string assoc_census_json() {
        char* raw = cf_assoc_census(handle_);
        if (!raw) { cf_soft(-1, __func__); return "[]"; }
        std::string result(raw);
        cf_free_string(raw);
        return result;
    }

    std::string assoc_decay_json(uint8_t edge_type, float factor, float prune_below, bool apply) {
        char* raw = cf_assoc_decay(handle_, edge_type, factor, prune_below, apply);
        if (!raw) { cf_soft(-1, __func__); return "{}"; }
        std::string result(raw);
        cf_free_string(raw);
        return result;
    }

    // Persist span links deferred off the memory-write hot path (1 = saved).
    int span_flush() {
        return cf_span_flush(handle_);
    }

    std::string executor_flush_json() {
        char* raw = cf_executor_flush(handle_);
        if (!raw) { cf_soft(-1, __func__); return "{}"; }
        std::string result(raw);
        cf_free_string(raw);
        return result;
    }

    std::string list_policies_json(bool active_only = false) {
        char* raw = cf_list_policies(handle_, active_only);
        if (!raw) { cf_soft(-1, __func__); return "[]"; }
        std::string result(raw);
        cf_free_string(raw);
        return result;
    }

    int log_event_ex(const std::string& tool, const std::string& entity,
                     uint8_t outcome, uint64_t session_id, int64_t ts_ms,
                     uint32_t token_cost, uint32_t latency_ms, uint8_t retry_count) {
        return cf_log_event_ex(handle_, tool.c_str(), entity.c_str(),
                               outcome, session_id, ts_ms, token_cost, latency_ms, retry_count);
    }

    int log_decision(const std::string& chosen_tool, const std::string& chosen_entity,
                     uint8_t chosen_outcome, const std::string& rejected_json,
                     float confidence_delta, int64_t ts_ms) {
        return cf_log_decision(handle_, chosen_tool.c_str(), chosen_entity.c_str(),
                               chosen_outcome, rejected_json.c_str(), confidence_delta, ts_ms);
    }

    std::string recall_true_counterfactual_json(const std::string& tool,
                                                const std::string& entity,
                                                uint8_t outcome = 0, size_t k = 5) {
        char* raw = cf_recall_true_counterfactual(handle_, tool.c_str(), entity.c_str(), outcome, k);
        if (!raw) { cf_soft(-1, __func__); return "[]"; }
        std::string result(raw);
        cf_free_string(raw);
        return result;
    }

    std::string hypothesis_probes_json(size_t k = 10) {
        char* raw = cf_hypothesis_probes(handle_, k);
        if (!raw) { cf_soft(-1, __func__); return "{}"; }
        std::string result(raw);
        cf_free_string(raw);
        return result;
    }

    std::string turiya_status_json() {
        char* raw = cf_turiya_status(handle_);
        if (!raw) return R"({"status":"no_data"})";
        std::string result(raw);
        cf_free_string(raw);
        return result;
    }

    std::string tape_stats_json() {
        char* raw = cf_tape_stats(handle_);
        if (!raw) return R"({"events":0})";
        std::string result(raw);
        cf_free_string(raw);
        return result;
    }

    std::string verbalize_rules_json(size_t k = 10) {
        char* raw = cf_verbalize_rules(handle_, k);
        if (!raw) return R"({"total":0,"rules":[]})";
        std::string result(raw);
        cf_free_string(raw);
        return result;
    }

    std::string queue_experiments_json(size_t k = 5) {
        char* raw = cf_queue_experiments(handle_, k);
        if (!raw) return R"({"queued":0})";
        std::string result(raw);
        cf_free_string(raw);
        return result;
    }

    std::string fep_status_json() {
        char* raw = cf_fep_status(handle_);
        if (!raw) return R"({"obs_count":0})";
        std::string result(raw);
        cf_free_string(raw);
        return result;
    }

    std::string routed_recall_json(const std::string& request_json) {
        char* raw = cf_routed_recall(handle_, request_json.c_str());
        if (!raw) return R"({"dispatch":"error","hits":[]})";
        std::string result(raw);
        cf_free_string(raw);
        return result;
    }

    std::string witness_memory_json(uint64_t memory_id, const std::string& witness_kind) {
        char* raw = cf_witness_memory(handle_, memory_id, witness_kind.c_str());
        if (!raw) return R"({"ok":false})";
        std::string result(raw);
        cf_free_string(raw);
        return result;
    }

    std::string reconcile_pass_json() {
        char* raw = cf_reconcile_pass(handle_);
        if (!raw) return R"({"illegal_edges":0,"contradictions":0,"unresolved":0})";
        std::string result(raw);
        cf_free_string(raw);
        return result;
    }

    std::string harvest_scope_json() {
        char* raw = cf_harvest_scope(handle_);
        if (!raw) return R"({"error":"unavailable"})";
        std::string result(raw);
        cf_free_string(raw);
        return result;
    }

    std::string seed_hdc_geometry_json(const std::string& json_path) {
        char* raw = cf_seed_hdc_geometry(handle_, json_path.c_str());
        if (!raw) return R"({"error":"unavailable"})";
        std::string result(raw);
        cf_free_string(raw);
        return result;
    }

    struct SessionHit {
        std::string session_id;
        float score;
        uint32_t chunk_count;
        float max_chunk_score;
        std::string best_evidence;
    };

    void set_source_session(uint64_t memory_id, const std::string& session_id) {
        cf_set_source_session(handle_, memory_id, session_id.c_str());
    }

    std::vector<SessionHit> recall_session(
            const std::vector<float>& embedding,
            const std::string& query,
            size_t k,
            const std::string& realm = "") {
        constexpr size_t MAX_SESS = 64;
        CfSessionHit buf[MAX_SESS];
        size_t written = 0;
        char* combined_json = nullptr;
        const float* emb_ptr = embedding.empty() ? nullptr : embedding.data();
        size_t emb_len = embedding.size();
        int r = cf_recall_session(
            handle_,
            emb_ptr, emb_len,
            query.empty() ? nullptr : query.c_str(),
            realm.empty() ? nullptr : realm.c_str(),
            k, buf, MAX_SESS, &written, &combined_json);
        if (r != 0) {
            if (combined_json) cf_free_string(combined_json);
            throw std::runtime_error(last_error());
        }
        // Parse session IDs and evidence from returned JSON
        std::vector<std::string> ids, evidence;
        if (combined_json) {
            try {
                auto parsed = nlohmann::json::parse(combined_json);
                for (auto& s : parsed.value("session_ids", nlohmann::json::array()))
                    ids.push_back(s.get<std::string>());
                for (auto& s : parsed.value("evidence", nlohmann::json::array()))
                    evidence.push_back(s.get<std::string>());
            } catch (...) {}
            cf_free_string(combined_json);
        }
        std::vector<SessionHit> results;
        results.reserve(written);
        for (size_t i = 0; i < written; ++i) {
            results.push_back({
                i < ids.size() ? ids[i] : "",
                buf[i].score, buf[i].chunk_count, buf[i].max_chunk_score,
                i < evidence.size() ? evidence[i] : ""
            });
        }
        return results;
    }

    struct SpreadingHit {
        uint64_t    memory_id;
        float       score;
        std::string text;
        std::string kind;
        std::string realm;
    };

    std::vector<SpreadingHit> recall_spreading(
        const std::string& query, size_t k, const std::string& realm = "",
        size_t max_nodes = 256, size_t max_entries_per_entity = 64,
        uint8_t depth = 2) const
    {
        char buf[1 << 20];
        int n = cf_recall_spreading(handle_, query.c_str(), k,
                                    realm.empty() ? nullptr : realm.c_str(),
                                    max_nodes, max_entries_per_entity, depth,
                                    buf, sizeof(buf));
        if (n <= 0) return {};
        std::vector<SpreadingHit> out;
        try {
            auto j = nlohmann::json::parse(buf);
            for (auto& r : j.at("results")) {
                SpreadingHit sh;
                sh.memory_id = r.value("memory_id", uint64_t(0));
                sh.score     = r.value("score", 0.0f);
                sh.text      = r.value("text", std::string{});
                sh.kind      = r.value("kind", std::string{});
                sh.realm     = r.value("realm", std::string{});
                out.push_back(std::move(sh));
            }
        } catch (...) {}
        return out;
    }

    /// Add an SPO triplet fact.
    uint64_t add_triplet(const std::string& subject, const std::string& predicate,
                         const std::string& object, float weight = 1.0f,
                         uint64_t source_memory_id = 0) {
        uint64_t id = 0;
        cf_add_triplet(handle_, subject.c_str(), predicate.c_str(), object.c_str(),
                       weight, source_memory_id, &id);
        return id;
    }

    /// Select retrieval route for query. Returns {episode_id, route} where
    /// route: 0=Semantic, 1=Keyword, 2=Temporal, 3=Artifact, 4=Hybrid, 5=Full
    struct RouteSelection { uint64_t episode_id; uint8_t route; };
    RouteSelection select_route(const std::string& query) {
        RouteSelection sel{0, 4};  // default Hybrid
        cf_select_route(handle_, query.c_str(), &sel.episode_id, &sel.route);
        return sel;
    }

    /// Record retrieval outcome for a route episode. reward in [-1, 1].
    void route_feedback(uint64_t episode_id, float reward) {
        cf_route_feedback(handle_, episode_id, reward);
    }

    /// Remove triplet by subject+predicate+object (invalidates matching entry).
    bool forget_triplet(const std::string& subject, const std::string& predicate,
                        const std::string& object) {
        return cf_forget_triplet(handle_, subject.c_str(), predicate.c_str(),
                                 object.c_str()) == 0;
    }

    /// Query triplets by subject, returns JSON string.
    std::string query_subject(const std::string& subject) {
        char buf[65536]; size_t written = 0;
        cf_query_subject(handle_, subject.c_str(), buf, sizeof(buf), &written);
        return std::string(buf, written);
    }

    /// Query triplets by object, returns JSON string.
    std::string query_object(const std::string& object) {
        char buf[65536]; size_t written = 0;
        cf_query_object(handle_, object.c_str(), buf, sizeof(buf), &written);
        return std::string(buf, written);
    }

    /// Query triplets for subject valid at world_ms, excluding superseded. Returns JSON string.
    std::string query_subject_as_of(const std::string& subject, int64_t world_ms) {
        char* s = cf_triplet_query_as_of(handle_, subject.c_str(), world_ms);
        if (!s) { cf_soft(-1, __func__); return "[]"; }
        std::string result(s);
        cf_free_string(s);
        return result;
    }

    /// Mark old_id as superseded by new_id at ingestion-time at_ms.
    void triplet_supersede(uint64_t old_id, uint64_t new_id, int64_t at_ms) {
        cf_triplet_supersede(handle_, old_id, new_id, at_ms);
    }

    /// BFS traversal from start node. Returns JSON array of TraversalHit objects.
    std::string graph_traverse(const std::string& start, const std::string& edge_types_json,
                               size_t max_hops, size_t max_results, const std::string& direction,
                               size_t max_edges) {
        char* s = cf_graph_traverse(handle_, start.c_str(), edge_types_json.c_str(),
                                    max_hops, max_results, direction.c_str(), max_edges);
        if (!s) { cf_soft(-1, __func__); return "[]"; }
        std::string result(s);
        cf_free_string(s);
        return result;
    }

    /// Personalized PageRank. Returns JSON array of [node, score] pairs.
    std::string graph_pagerank(const std::string& seeds_json, const std::string& edge_types_json,
                               float damping, uint8_t iterations, size_t top_k,
                               size_t max_nodes, size_t max_edges) {
        char* s = cf_graph_pagerank(handle_, seeds_json.c_str(), edge_types_json.c_str(),
                                    damping, iterations, top_k, max_nodes, max_edges);
        if (!s) { cf_soft(-1, __func__); return "[]"; }
        std::string result(s);
        cf_free_string(s);
        return result;
    }

    /// Health check — returns true if store is accessible.
    bool healthy() const {
        return handle_ != nullptr;
    }

    // ── Code Intelligence ────────────────────────────────────────────────────

    /// Upsert a symbol. Returns its SymbolId.
    uint64_t upsert_symbol(
        const std::string& kind,
        const std::string& name,
        const std::string& signature,
        const std::string& file_path,
        uint32_t line_start,
        uint32_t line_end,
        uint64_t repo_id,
        const std::vector<float>& embedding,
        const std::string& description = "",
        uint64_t memory_id = 0
    ) {
        uint64_t id = 0;
        const char* desc_ptr = description.empty() ? nullptr : description.c_str();
        int r = cf_upsert_symbol(handle_,
            kind.c_str(), name.c_str(), signature.c_str(), file_path.c_str(),
            line_start, line_end, repo_id,
            embedding.data(), embedding.size(),
            desc_ptr, memory_id, &id);
        cf_checked(r, __func__);
        // Auto-log symbol event for code-intel tracking
        {
            nlohmann::json ev;
            ev["symbol_name"] = name;
            ev["file_path"]   = file_path;
            ev["symbol_id"]   = id;
            ev["kind"]        = 0; // Edited (Created also mapped here; upsert is idempotent)
            ev["session_id"]  = "";
            ev["harness"]     = "";
            cf_log_symbol_event(handle_, ev.dump().c_str());
        }
        return id;
    }

    void remove_symbol(uint64_t symbol_id) {
        cf_remove_symbol(handle_, symbol_id);
    }

    std::vector<CfSymbolHit> search_symbols_by_name(const std::string& query, size_t limit) {
        constexpr size_t MAX = 256;
        CfSymbolHit buf[MAX];
        size_t written = 0;
        cf_search_symbols_by_name(handle_, query.c_str(), limit, buf, MAX, &written);
        return std::vector<CfSymbolHit>(buf, buf + written);
    }

    std::vector<CfSymbolHit> search_symbols_semantic(const std::vector<float>& query, size_t k) {
        constexpr size_t MAX = 256;
        CfSymbolHit buf[MAX];
        size_t written = 0;
        cf_search_symbols_semantic(handle_, query.data(), query.size(), k, buf, MAX, &written);
        return std::vector<CfSymbolHit>(buf, buf + written);
    }

    std::vector<CfSymbolHit> symbols_in_file(const std::string& file_path) {
        constexpr size_t MAX = 1024;
        CfSymbolHit buf[MAX];
        size_t written = 0;
        cf_symbols_in_file(handle_, file_path.c_str(), buf, MAX, &written);
        return std::vector<CfSymbolHit>(buf, buf + written);
    }

    /// Name search restricted to file paths containing `path_filter`
    /// (empty = unscoped).
    std::vector<CfSymbolHit> search_symbols_by_name_scoped(
        const std::string& query, size_t limit, const std::string& path_filter) {
        constexpr size_t MAX = 256;
        CfSymbolHit buf[MAX];
        size_t written = 0;
        cf_search_symbols_by_name_scoped(handle_, query.c_str(), limit,
            path_filter.empty() ? nullptr : path_filter.c_str(),
            buf, MAX, &written);
        return std::vector<CfSymbolHit>(buf, buf + written);
    }

    /// Remove every symbol in a file (per-file invalidation before re-extract).
    /// Returns the number removed.
    size_t remove_symbols_by_file(const std::string& file_path) {
        size_t removed = 0;
        cf_remove_symbols_by_file(handle_, file_path.c_str(), &removed);
        return removed;
    }

    /// GC the symbol index. `path_excludes_csv` = comma-separated substrings.
    /// Returns the JSON stats string (empty on failure).
    std::string dedupe_symbols(bool dry_run, bool check_fs,
                               const std::string& path_excludes_csv) {
        std::vector<uint8_t> buf(1 << 20);
        size_t written = 0;
        int r = cf_dedupe_symbols(handle_, dry_run ? 1 : 0, check_fs ? 1 : 0,
            path_excludes_csv.empty() ? nullptr : path_excludes_csv.c_str(),
            buf.data(), buf.size(), &written);
        if (r != 0) return "";
        return std::string(reinterpret_cast<char*>(buf.data()), written);
    }

    void add_sym_call_edge(uint64_t caller_id, uint64_t callee_id) {
        cf_add_sym_call_edge(handle_, caller_id, callee_id);
    }

    std::vector<uint64_t> get_callees(uint64_t symbol_id) {
        constexpr size_t MAX = 1024;
        uint64_t buf[MAX];
        size_t written = 0;
        cf_get_callees(handle_, symbol_id, buf, MAX, &written);
        return std::vector<uint64_t>(buf, buf + written);
    }

    std::vector<uint64_t> get_callers(uint64_t symbol_id) {
        constexpr size_t MAX = 1024;
        uint64_t buf[MAX];
        size_t written = 0;
        cf_get_callers(handle_, symbol_id, buf, MAX, &written);
        return std::vector<uint64_t>(buf, buf + written);
    }

    std::vector<uint64_t> get_conflicts(uint64_t memory_id) {
        constexpr size_t MAX = 256;
        uint64_t buf[MAX];
        size_t written = 0;
        cf_get_conflicts(handle_, memory_id, buf, MAX, &written);
        return std::vector<uint64_t>(buf, buf + written);
    }

    std::vector<uint64_t> get_supersession_chain(uint64_t memory_id) {
        constexpr size_t MAX = 256;
        uint64_t buf[MAX];
        size_t written = 0;
        cf_get_supersession_chain(handle_, memory_id, buf, MAX, &written);
        return std::vector<uint64_t>(buf, buf + written);
    }

    std::vector<uint64_t> get_confirmations(uint64_t memory_id) {
        constexpr size_t MAX = 256;
        uint64_t buf[MAX];
        size_t written = 0;
        cf_get_confirmations(handle_, memory_id, buf, MAX, &written);
        return std::vector<uint64_t>(buf, buf + written);
    }

    uint64_t upsert_code_file(const std::string& path, const std::string& project, int64_t mtime) {
        uint64_t id = 0;
        int r = cf_upsert_code_file(handle_, path.c_str(), project.c_str(), mtime, &id);
        cf_checked(r, __func__);
        return id;
    }

    // ── Contradiction detection wrappers ─────────────────────────────────────

    /// Detect contradictions for a just-stored memory against realm peers.
    /// Returns raw JSON string (caller must free with cf_free_string) or "" on error.
    std::string detect_contradictions(uint64_t memory_id, const std::string& realm) {
        char* raw = cf_detect_contradictions(handle_, memory_id, realm.c_str());
        if (!raw) { cf_soft(-1, __func__); return "[]"; }
        std::string result(raw);
        cf_free_string(raw);
        return result;
    }

    /// Background scan of entire realm for contradictions.
    std::string scan_contradictions(const std::string& realm, uint32_t limit = 100) {
        char* raw = cf_scan_contradictions(handle_, realm.c_str(), limit);
        if (!raw) { cf_soft(-1, __func__); return "[]"; }
        std::string result(raw);
        cf_free_string(raw);
        return result;
    }

    /// Resolve a contradiction pair. Returns ResolutionOps JSON for caller to apply.
    std::string resolve_contradiction(uint64_t winner_id, uint64_t loser_id,
                                      const std::string& reason = "manual") {
        char* raw = cf_resolve_contradiction(handle_, winner_id, loser_id, reason.c_str());
        if (!raw) { cf_soft(-1, __func__); return "{}"; }
        std::string result(raw);
        cf_free_string(raw);
        return result;
    }

    /// Upsert a code file with content hash and git provenance.
    /// Returns {file_id, was_updated} where was_updated indicates content changed.
    std::pair<uint64_t, bool> upsert_code_file_v2(
        const std::string& path, const std::string& project, int64_t mtime,
        const std::string& content_hash, const std::string& git_commit,
        const std::string& git_author, int64_t git_timestamp_ms)
    {
        uint64_t id = 0;
        int changed = 0;
        const char* hash_ptr = content_hash.empty() ? nullptr : content_hash.c_str();
        const char* commit_ptr = git_commit.empty() ? nullptr : git_commit.c_str();
        const char* author_ptr = git_author.empty() ? nullptr : git_author.c_str();
        int r = cf_upsert_code_file_v2(handle_, path.c_str(), project.c_str(), mtime,
            hash_ptr, commit_ptr, author_ptr, git_timestamp_ms,
            &changed, &id);
        cf_checked(r, __func__);
        return {id, changed != 0};
    }

    /// Invalidate all active triplets associated with a source file.
    void invalidate_triplets_by_source_file(const std::string& path) {
        cf_invalidate_triplets_by_source_file(handle_, path.c_str());
    }

    /// Add a triplet with optional source file tracking.
    uint64_t add_triplet_with_source(const std::string& subject, const std::string& predicate,
                                     const std::string& object, float weight,
                                     uint64_t source_memory_id, const std::string& source_file) {
        uint64_t id = 0;
        const char* sf_ptr = source_file.empty() ? nullptr : source_file.c_str();
        cf_add_triplet_with_source(handle_, subject.c_str(), predicate.c_str(), object.c_str(),
                                   weight, source_memory_id, sf_ptr, &id);
        return id;
    }

    size_t symbol_count() const {
        return cf_symbol_count(handle_);
    }

    size_t code_file_count() const {
        return cf_code_file_count(handle_);
    }

    /// Encode all unindexed memories into sparse codes. Returns count encoded.
    size_t encode_all() {
        return cf_encode_all(handle_);
    }

    /// Get cortical index size (how many memories have sparse codes).
    size_t cortical_count() const {
        return cf_cortical_count(handle_);
    }

    /// Get number of prototype clusters in the cortical index.
    size_t prototype_count() {
        return cf_prototype_count(handle_);
    }

    /// Save the cortical index to a binary snapshot file. Returns true on success.
    bool save_snapshot() const {
        return cf_save_snapshot(handle_);
    }

    /// Save the full in-memory state to a binary snapshot file. Returns true on success.
    bool save_full_snapshot() const {
        return cf_save_full_snapshot(handle_);
    }

    /// Force a full rebuild of all search indices from current embeddings.
    /// Required after an embedding-dimension migration. Returns true on success.
    bool force_reindex() const {
        return cf_force_reindex(handle_) == 0;
    }

    // ── Lite Encoder ─────────────────────────────────────────────────────────

    /// Check if the lite encoder is trained and ready.
    bool lite_encoder_ready() const {
        return cf_lite_encoder_ready(handle_) != 0;
    }

    /// Train the lite encoder from all memories with sparse codes.
    /// Returns true if at least one training example was used.
    bool train_lite_encoder() {
        return cf_train_lite_encoder(handle_) > 0;
    }

    /// Save the lite encoder to disk. Returns true on success.
    bool save_lite_encoder() {
        return cf_save_lite_encoder(handle_) == 0;
    }

    /// Encode text via lite encoder. Returns (atom_idx, weight) pairs.
    /// Returns empty vector if not trained or no words match vocab.
    std::vector<std::pair<uint32_t, float>> encode_lite(const std::string& text) {
        static constexpr size_t K_ACTIVE = 64;
        uint32_t atoms[K_ACTIVE] = {};
        float weights[K_ACTIVE] = {};
        int32_t n = cf_encode_lite(handle_,
            reinterpret_cast<const uint8_t*>(text.data()), text.size(),
            atoms, weights);
        if (n <= 0) return {};
        std::vector<std::pair<uint32_t, float>> result;
        result.reserve(static_cast<size_t>(n));
        for (int32_t i = 0; i < n; ++i) {
            result.emplace_back(atoms[i], weights[i]);
        }
        return result;
    }

    /// Run a tier demotion pass. Returns (demoted_count, deleted_count).
    std::pair<size_t, size_t> run_demotion(int64_t now_ms) const {
        uint64_t r = cf_run_demotion(handle_, now_ms);
        return {static_cast<size_t>(r & 0xFFFFFFFF), static_cast<size_t>(r >> 32)};
    }

    // ── Task / Sadhana organ ─────────────────────────────────────────────────

    /// Create a task entry in chitta-field. Returns 0 on success.
    int task_create(const std::string& task_id, const std::string& kind,
                    const std::string& payload_json, int64_t now_ms,
                    uint64_t fencing_token = 0) {
        return cf_task_create(handle_,
            task_id.c_str(), kind.c_str(),
            reinterpret_cast<const uint8_t*>(payload_json.data()), payload_json.size(),
            now_ms, fencing_token);
    }

    /// Transition a task status. new_status: "start"|"pause"|"resume"|"complete"|"fail".
    /// Returns true on success.
    bool task_transition(const std::string& task_id, const std::string& new_status,
                         int64_t now_ms, uint64_t fencing_token = 0) {
        return cf_task_transition(handle_,
            task_id.c_str(), new_status.c_str(),
            now_ms, fencing_token) == 0;
    }

    /// List tasks by kind. Returns JSON array. active_only=true filters non-terminal.
    std::string task_list(const std::string& kind = "", bool active_only = false) {
        std::vector<uint8_t> buf(262144);
        size_t written = 0;
        const char* kind_ptr = kind.empty() ? nullptr : kind.c_str();
        int r = cf_task_list(handle_, kind_ptr, active_only ? 1 : 0,
                             buf.data(), buf.size(), &written);
        if (r != 0 || written == 0) return "[]";
        return std::string(reinterpret_cast<char*>(buf.data()), written);
    }

    // ── Domain Event Log ─────────────────────────────────────────────────────

    /// Emit a domain event. Returns the assigned event_id.
    /// fencing_token=0 means intent/report tier; non-zero means authoritative (leader-only).
    uint64_t emit_event(const std::string& domain, const std::string& kind,
                        const std::string& entity_id, const std::string& payload_json,
                        uint64_t fencing_token = 0, const std::string& realm = "") {
        uint64_t event_id = 0;
        int r = cf_emit_event(handle_,
            domain.c_str(), kind.c_str(), entity_id.c_str(),
            reinterpret_cast<const uint8_t*>(payload_json.data()), payload_json.size(),
            realm.empty() ? nullptr : realm.c_str(),
            fencing_token, &event_id);
        cf_checked(r, __func__);
        return event_id;
    }

    /// Startup-only replay, grow until complete; never silently truncate history.
    std::string task_ledger_events() {
        std::vector<uint8_t> buf(64 * 1024);
        size_t written = 0;
        for (;;) {
            int r = cf_get_events_by_domain_kind(handle_, "ledger", "task_records",
                std::numeric_limits<size_t>::max(), buf.data(), buf.size(), &written);
            if (r == -2) { buf.resize(buf.size() * 2); continue; }
            cf_checked(r, __func__);
            return std::string(reinterpret_cast<char*>(buf.data()), written);
        }
    }

    /// Query events by domain+kind+target. Returns JSON array string.
    std::string get_events_by_target(const std::string& domain, const std::string& kind,
                                     const std::string& target, size_t limit = 20) {
        std::vector<uint8_t> buf(64 * 1024);
        size_t written = 0;
        int r = cf_get_events_by_target(handle_,
            domain.c_str(), kind.c_str(), target.c_str(), limit,
            buf.data(), buf.size(), &written);
        if (r == -2) {
            buf.resize(512 * 1024);
            r = cf_get_events_by_target(handle_,
                domain.c_str(), kind.c_str(), target.c_str(), limit,
                buf.data(), buf.size(), &written);
        }
        if (r != 0 || written == 0) return "[]";
        return std::string(reinterpret_cast<char*>(buf.data()), written);
    }

    /// Query all events matching domain+kind across all targets. Returns JSON array (newest-first).
    std::string get_events_by_domain_kind(const std::string& domain, const std::string& kind,
                                          size_t limit = 100) {
        std::vector<uint8_t> buf(64 * 1024);
        size_t written = 0;
        int r = cf_get_events_by_domain_kind(handle_,
            domain.c_str(), kind.c_str(), limit,
            buf.data(), buf.size(), &written);
        if (r == -2) {
            buf.resize(512 * 1024);
            r = cf_get_events_by_domain_kind(handle_,
                domain.c_str(), kind.c_str(), limit,
                buf.data(), buf.size(), &written);
        }
        if (r != 0 || written == 0) return "[]";
        return std::string(reinterpret_cast<char*>(buf.data()), written);
    }

    /// Check whether any event exists for (domain, kind, target). Returns true if found.
    bool has_event(const std::string& domain, const std::string& kind, const std::string& target) {
        return cf_has_event(handle_, domain.c_str(), kind.c_str(), target.c_str()) == 1;
    }

    /// Look up a single event by event_id. Returns JSON object string, or "{}" if not found.
    std::string get_event_by_id(uint64_t event_id) {
        std::vector<uint8_t> buf(32 * 1024);
        size_t written = 0;
        int r = cf_get_event_by_id(handle_, event_id, buf.data(), buf.size(), &written);
        if (r != 0 || written == 0) return "{}";
        return std::string(reinterpret_cast<char*>(buf.data()), written);
    }

    /// Iterate the event log from from_seqno, invoking cb(op_json, seqno) for each entry.
    void iterate_log(uint64_t from_seqno,
                     std::function<void(const std::string& op_json, uint64_t seqno)> cb) {
        struct Ctx { std::function<void(const std::string&, uint64_t)>* fn; };
        Ctx ctx{&cb};
        cf_iterate_log(handle_, from_seqno,
            [](const uint8_t* op_json, size_t op_len, uint64_t seqno, void* raw) {
                auto* c = static_cast<Ctx*>(raw);
                (*c->fn)(std::string(reinterpret_cast<const char*>(op_json), op_len), seqno);
            }, &ctx);
    }

    /// Upsert a user model entity (key: entity_id, tag: entity_type).
    /// Returns 0 on success, negative on error.
    int user_model_upsert(const std::string& entity_id, const std::string& entity_type,
                          const std::string& payload_json, int64_t now_ms) {
        return cf_user_model_upsert(handle_,
            entity_id.c_str(), entity_type.c_str(),
            reinterpret_cast<const uint8_t*>(payload_json.data()), payload_json.size(),
            now_ms);
    }

    // ── Theme Management ─────────────────────────────────────────────────────

    /// List all themes. Returns JSON string array.
    std::string theme_list() {
        std::vector<uint8_t> buf(65536);
        size_t written = 0;
        cf_theme_list(handle_, buf.data(), buf.size(), &written);
        return std::string(reinterpret_cast<char*>(buf.data()), written);
    }

    /// Get a single theme by ID. Returns JSON string or empty on not found.
    std::string theme_get(uint64_t theme_id) {
        std::vector<uint8_t> buf(65536);
        size_t written = 0;
        int r = cf_theme_get(handle_, theme_id, buf.data(), buf.size(), &written);
        if (r != 0 || written == 0) return "";
        return std::string(reinterpret_cast<char*>(buf.data()), written);
    }

    /// Get theme statistics. Returns JSON string.
    std::string theme_stats(const std::string& realm = "") {
        std::vector<uint8_t> buf(65536);
        size_t written = 0;
        const char* realm_ptr = realm.empty() ? nullptr : realm.c_str();
        cf_theme_stats(handle_, realm_ptr, buf.data(), buf.size(), &written);
        return std::string(reinterpret_cast<char*>(buf.data()), written);
    }

    /// Recall themes by embedding similarity. Returns JSON string array of {theme_id, score}.
    std::string theme_recall(const std::vector<float>& embedding,
                             size_t k,
                             const std::string& realm = "") {
        std::vector<uint8_t> buf(65536);
        size_t written = 0;
        const char* realm_ptr = realm.empty() ? nullptr : realm.c_str();
        cf_theme_recall(handle_,
            embedding.data(), embedding.size(),
            k, realm_ptr,
            buf.data(), buf.size(), &written);
        return std::string(reinterpret_cast<char*>(buf.data()), written);
    }

    /// Run theme maintenance (split/merge/reassign). Returns JSON result.
    std::string theme_maintain() {
        std::vector<uint8_t> buf(65536);
        size_t written = 0;
        cf_theme_maintain(handle_, buf.data(), buf.size(), &written);
        return std::string(reinterpret_cast<char*>(buf.data()), written);
    }

    /// Assign orphan memories to themes. Returns JSON with {assigned, remaining}.
    std::string theme_assign_orphans(size_t batch_size = 500,
                                     const std::string& realm = "") {
        std::vector<uint8_t> buf(65536);
        size_t written = 0;
        const char* realm_ptr = realm.empty() ? nullptr : realm.c_str();
        cf_theme_assign_orphans(handle_, batch_size, realm_ptr,
                                buf.data(), buf.size(), &written);
        return std::string(reinterpret_cast<char*>(buf.data()), written);
    }

    /// Get the payload of the most recent domain event matching domain+kind+entity_id.
    /// Currently supports domain="user_model"; kind matches entity_type.
    /// Returns the JSON payload string if found, or nullopt if not found or on error.
    std::optional<std::string> get_latest_event(
        const std::string& domain,
        const std::string& kind,
        const std::string& entity_id) {
        std::vector<uint8_t> buf(65536);
        size_t written = 0;
        int rc = cf_get_latest_event(handle_, domain.c_str(), kind.c_str(), entity_id.c_str(),
                                     buf.data(), buf.size(), &written);
        if (rc == 0 && written > 0) {
            return std::string(reinterpret_cast<char*>(buf.data()), written);
        }
        return std::nullopt;
    }

    // ── Phase 0: New query/management methods ───────────────────────────────

    /// 1. Filtered recall — returns JSON array of matching memories.
    std::string recall_filtered(const std::string& kind = "", const std::string& realm = "",
                                float min_confidence = 0.0f, float min_strength = 0.0f,
                                size_t limit = 50) {
        std::vector<uint8_t> buf(262144);
        size_t written = 0;
        const char* kind_ptr = kind.empty() ? nullptr : kind.c_str();
        const char* realm_ptr = realm.empty() ? nullptr : realm.c_str();
        int r = cf_recall_filtered(handle_, kind_ptr, realm_ptr,
                                   min_confidence, min_strength, limit,
                                   buf.data(), buf.size(), &written);
        if (r != 0 && r != -2) return "[]";
        return std::string(reinterpret_cast<char*>(buf.data()), written);
    }

    /// 2. Paginated memory listing sorted by strength/recency/confidence.
    /// Retries with a growing buffer on -2 (truncated) up to 64 MiB so large
    /// limits don't silently return an empty string.
    std::string list_memories(const std::string& kind = "", const std::string& realm = "",
                              const std::string& sort_by = "recency",
                              size_t limit = 50, size_t offset = 0) {
        const char* kind_ptr = kind.empty() ? nullptr : kind.c_str();
        const char* realm_ptr = realm.empty() ? nullptr : realm.c_str();
        size_t cap = 262144;
        constexpr size_t kMaxCap = 64 * 1024 * 1024;
        for (;;) {
            std::vector<uint8_t> buf(cap);
            size_t written = 0;
            int r = cf_list_memories(handle_, kind_ptr, realm_ptr,
                                     sort_by.c_str(), limit, offset,
                                     buf.data(), buf.size(), &written);
            if (r == 0) return std::string(reinterpret_cast<char*>(buf.data()), written);
            if (r != -2 || cap >= kMaxCap) return "[]";
            cap = std::min(cap * 4, kMaxCap);
        }
    }

    /// 3. Aggregate memory stats. Returns JSON with count_by_kind, avg_confidence, etc.
    std::string memory_stats(const std::string& realm = "") {
        std::vector<uint8_t> buf(65536);
        size_t written = 0;
        const char* realm_ptr = realm.empty() ? nullptr : realm.c_str();
        int r = cf_memory_stats(handle_, realm_ptr, buf.data(), buf.size(), &written);
        if (!cf_soft(r, __func__)) return "{}";
        return std::string(reinterpret_cast<char*>(buf.data()), written);
    }

    /// Per-realm embedding geometry stats (effective dimensionality, isotropy, mean cosine sim).
    std::string spectral_stats_by_realm() {
        std::vector<uint8_t> buf(65536);
        size_t written = 0;
        int r = cf_spectral_stats_by_realm(handle_, buf.data(), buf.size(), &written);
        if (!cf_soft(r, __func__)) return "[]";
        return std::string(reinterpret_cast<char*>(buf.data()), written);
    }

    /// Trim trailing whitespace from realm names. Returns count fixed.
    int64_t trim_realm_names() {
        return cf_trim_realm_names(handle_);
    }

    /// Bulk-remap realms per a JSON {old:new} object. dry_run = census only.
    std::string remap_realms(const std::string& mapping_json, bool dry_run) {
        std::vector<uint8_t> buf(65536);
        size_t written = 0;
        int r = cf_remap_realms(handle_, mapping_json.c_str(), dry_run,
                                buf.data(), buf.size(), &written);
        if (!cf_soft(r, __func__)) return "{}";
        return std::string(reinterpret_cast<char*>(buf.data()), written);
    }

    /// Save spectral snapshot for drift tracking.
    std::string save_spectral_snapshot() {
        std::vector<uint8_t> buf(4096);
        size_t written = 0;
        int r = cf_save_spectral_snapshot(handle_, buf.data(), buf.size(), &written);
        if (!cf_soft(r, __func__)) return "";
        return std::string(reinterpret_cast<char*>(buf.data()), written);
    }

    /// Get spectral drift since last snapshot.
    std::string spectral_drift() {
        std::vector<uint8_t> buf(65536);
        size_t written = 0;
        int r = cf_spectral_drift(handle_, buf.data(), buf.size(), &written);
        if (!cf_soft(r, __func__)) return "{}";
        return std::string(reinterpret_cast<char*>(buf.data()), written);
    }

    /// 4. Get single task by ID. Returns JSON string, or empty on not found.
    std::string task_get(const std::string& task_id) {
        std::vector<uint8_t> buf(65536);
        size_t written = 0;
        int r = cf_task_get(handle_, task_id.c_str(), buf.data(), buf.size(), &written);
        if (r != 0 || written == 0) return "";
        return std::string(reinterpret_cast<char*>(buf.data()), written);
    }

    /// 5. Update task payload. Returns true on success.
    bool task_update_payload(const std::string& task_id,
                             const std::string& payload_json, int64_t now_ms) {
        return cf_task_update_payload(handle_, task_id.c_str(),
                                      payload_json.c_str(), now_ms) == 0;
    }

    /// 6. List sessions as JSON array. active_only=true filters by active status.
    std::string session_list(bool active_only = false) {
        std::vector<uint8_t> buf(65536);
        size_t written = 0;
        int r = cf_session_list(handle_, active_only ? 1 : 0,
                                buf.data(), buf.size(), &written);
        if (!cf_soft(r, __func__)) return "[]";
        return std::string(reinterpret_cast<char*>(buf.data()), written);
    }

    /// 7. List transcripts as JSON array, most recent first.
    std::string transcript_list(size_t limit = 50) {
        std::vector<uint8_t> buf(65536);
        size_t written = 0;
        int r = cf_transcript_list(handle_, limit, buf.data(), buf.size(), &written);
        if (!cf_soft(r, __func__)) return "[]";
        return std::string(reinterpret_cast<char*>(buf.data()), written);
    }

    /// 8. Get memory metadata by ID. Returns JSON string, or empty on not found.
    std::string get_memory_metadata(uint64_t memory_id) {
        std::vector<uint8_t> buf(4096);
        size_t written = 0;
        int r = cf_get_memory_metadata(handle_, memory_id, buf.data(), buf.size(), &written);
        if (r != 0 || written == 0) return "";
        return std::string(reinterpret_cast<char*>(buf.data()), written);
    }

    /// 9. Update memory kind field. Returns true on success.
    bool update_memory_kind(uint64_t memory_id, const std::string& new_kind) {
        return cf_update_memory_kind(handle_, memory_id, new_kind.c_str()) == 0;
    }

    /// Set the primary realm of a memory, updating in-memory indexes immediately.
    bool set_realm(uint64_t memory_id, const std::string& new_realm) {
        return cf_set_realm(handle_, memory_id, new_realm.c_str()) == 0;
    }

    /// 10. List all triplets where entity is subject OR object. Returns JSON string.
    std::string list_triplets_for_entity(const std::string& entity, size_t limit = 100) {
        std::vector<char> buf(65536);
        size_t written = 0;
        int r = cf_list_triplets_for_entity(handle_, entity.c_str(), limit,
                                            buf.data(), buf.size(), &written);
        if (!cf_soft(r, __func__)) return "[]";
        return std::string(buf.data(), written);
    }

    // ── Query and management methods ────────────────────────────────────────

    std::string list_code_files(const std::string& project = "") {
        std::vector<uint8_t> buf(131072);
        size_t written = 0;
        const char* proj_ptr = project.empty() ? nullptr : project.c_str();
        int r = cf_list_code_files(handle_, proj_ptr, buf.data(), buf.size(), &written);
        if (r != 0 && r != -2) return "[]";
        return std::string(reinterpret_cast<char*>(buf.data()), written);
    }

    int clear_project(const std::string& project) {
        return cf_clear_project(handle_, project.c_str());
    }

    int set_symbol_description(uint64_t symbol_id, const std::string& desc) {
        return cf_set_symbol_description(handle_, symbol_id, desc.c_str(), desc.size());
    }

    int update_memory_content(uint64_t id, const std::string& content,
                              const std::vector<float>& embedding = {}) {
        const float* emb_ptr = embedding.empty() ? nullptr : embedding.data();
        return cf_update_memory_content(handle_, id,
            reinterpret_cast<const uint8_t*>(content.data()), content.size(),
            emb_ptr, embedding.size());
    }

    std::string realm_list() {
        std::vector<uint8_t> buf(32768);
        size_t written = 0;
        int r = cf_realm_list(handle_, buf.data(), buf.size(), &written);
        if (!cf_soft(r, __func__)) return "[]";
        return std::string(reinterpret_cast<char*>(buf.data()), written);
    }

    std::vector<FieldRecallHit> recall_by_kind(const std::string& kind, size_t limit) {
        std::vector<uint8_t> buf(131072);
        size_t written = 0;
        int r = cf_recall_by_kind(handle_, kind.c_str(), limit, buf.data(), buf.size(), &written);
        if (r != 0 && r != -2) return {};
        auto json_str = std::string(reinterpret_cast<char*>(buf.data()), written);
        try {
            auto arr = nlohmann::json::parse(json_str, nullptr, false);
            if (arr.is_discarded() || !arr.is_array()) return {};
            std::vector<FieldRecallHit> hits;
            for (const auto& item : arr) {
                FieldRecallHit h;
                h.memory_id = item.value("id", uint64_t(0));
                h.confidence = item.value("confidence", 0.0f);
                h.content = item.value("content", std::string{});
                h.kind = kind;
                hits.push_back(h);
            }
            return hits;
        } catch (...) { return {}; }
    }

    /// Purge corrupt memories (empty/whitespace content or non-finite affect).
    /// Returns the count of memories purged.
    size_t purge_corrupt() {
        size_t purged = 0;
        if (cf_purge_corrupt(handle_, &purged) != 0) return 0;
        return purged;
    }

    /// Record co-retrieval for Hebbian association strengthening.
    void record_co_retrieval(const std::vector<uint64_t>& memory_ids,
                             float base_assoc_delta = 0.05f) {
        if (memory_ids.size() < 2) return;
        int64_t ts_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        cf_record_recall_batch(handle_,
            memory_ids.data(), memory_ids.size(),
            nullptr, 0, 1.0f, 0, ts_ms, base_assoc_delta);
    }

    /// Get association edges for a memory as JSON string.
    std::string get_assoc_edges(uint64_t memory_id, size_t limit = 20) {
        char buf[32768];
        size_t written = 0;
        int r = cf_get_assoc_edges(handle_, memory_id, limit,
                                    buf, sizeof(buf), &written);
        if (!cf_soft(r, __func__)) return "[]";
        return std::string(buf, written);
    }

    /// Batch-fetch embeddings for multiple memories as JSON.
    std::string get_memory_embeddings_batch(const std::vector<uint64_t>& ids) {
        if (ids.empty()) return "{}";
        char buf[1 << 20]; // 1MB
        size_t written = 0;
        int r = cf_get_memory_embeddings_batch(handle_,
            ids.data(), ids.size(), buf, sizeof(buf), &written);
        if (!cf_soft(r, __func__)) return "{}";
        return std::string(buf, written);
    }

    // ── Skill Registry ───────────────────────────────────────────────

    /// Upload a new skill version. Returns the version number.
    int skill_upload(const std::string& skill_id, const std::string& content,
                     const std::string& uploaded_by, const std::string& tags_json, int64_t ts_ms) {
        return cf_skill_upload(handle_, skill_id.c_str(), content.c_str(),
            uploaded_by.c_str(), tags_json.c_str(), ts_ms);
    }

    /// Read a skill version as JSON. version=0 means latest.
    std::string skill_read(const std::string& skill_id, uint32_t version = 0) {
        char* json = cf_skill_read(handle_, skill_id.c_str(), version);
        if (!json) return "";
        std::string result(json);
        cf_free_string(json);
        return result;
    }

    /// List all skills as JSON array.
    std::string skill_list() {
        char* json = cf_skill_list(handle_);
        if (!json) { cf_soft(-1, __func__); return "[]"; }
        std::string result(json);
        cf_free_string(json);
        return result;
    }

    /// Search skills by query. Returns JSON array.
    std::string skill_search(const std::string& query, size_t limit = 20) {
        char* json = cf_skill_search(handle_, query.c_str(), limit);
        if (!json) { cf_soft(-1, __func__); return "[]"; }
        std::string result(json);
        cf_free_string(json);
        return result;
    }

    /// Deprecate a skill.
    int skill_deprecate(const std::string& skill_id) {
        return cf_skill_deprecate(handle_, skill_id.c_str());
    }

    // ── Agent Registry ──────────────────────────────────────────────

    /// Register or update an agent. Returns 1 if new, 0 if updated.
    int agent_upsert(const std::string& agent_id, const std::string& display_name,
                     const std::string& description, int64_t ts_ms) {
        return cf_agent_upsert(handle_, agent_id.c_str(), display_name.c_str(),
            description.c_str(), ts_ms);
    }

    /// Record activity for an agent.
    int agent_record_activity(const std::string& agent_id, int64_t ts_ms) {
        return cf_agent_record_activity(handle_, agent_id.c_str(), ts_ms);
    }

    /// Record a session for an agent.
    int agent_record_session(const std::string& agent_id, int64_t ts_ms) {
        return cf_agent_record_session(handle_, agent_id.c_str(), ts_ms);
    }

    /// Get an agent record as JSON.
    std::string agent_get(const std::string& agent_id) {
        char* json = cf_agent_get(handle_, agent_id.c_str());
        if (!json) return "";
        std::string result(json);
        cf_free_string(json);
        return result;
    }

    /// List all agents as JSON array.
    std::string agent_list() {
        char* json = cf_agent_list(handle_);
        if (!json) { cf_soft(-1, __func__); return "[]"; }
        std::string result(json);
        cf_free_string(json);
        return result;
    }

    /// Disable (revoke) an agent.
    int agent_disable(const std::string& agent_id) {
        return cf_agent_disable(handle_, agent_id.c_str());
    }

    // ── Soul REPL Session Store ─────────────────────────────────────────────

    std::string repl_session_get(const std::string& session_id) {
        char* json = cf_repl_session_get(handle_, session_id.c_str());
        if (!json) return "";
        std::string result(json);
        cf_free_string(json);
        return result;
    }

    int repl_session_set(const std::string& session_id, const std::string& namespace_json, int64_t updated_ms) {
        return cf_repl_session_set(handle_, session_id.c_str(), namespace_json.c_str(), updated_ms);
    }

    int repl_session_delete(const std::string& session_id) {
        return cf_repl_session_delete(handle_, session_id.c_str());
    }

    std::string repl_session_list() {
        char* json = cf_repl_session_list(handle_);
        if (!json) { cf_soft(-1, __func__); return "[]"; }
        std::string result(json);
        cf_free_string(json);
        return result;
    }

    // Atomic execute: load namespace, run code, persist namespace.
    // Returns JSON: {success, output, error, session_id, trajectory}.
    std::string repl_execute(const std::string& session_id, const std::string& code,
                             bool reset, const std::string& socket_path,
                             int max_output = 10000) {
        char* json = cf_repl_execute(handle_, session_id.c_str(), code.c_str(),
                                      reset ? 1 : 0, socket_path.c_str(), max_output);
        if (!json) return R"({"success":false,"error":"cf_repl_execute failed"})";
        std::string result(json);
        cf_free_string(json);
        return result;
    }

    CfHandle* handle() const { return handle_; }

    // ── Interaction Ledger ──────────────────────────────────────────────────

    uint64_t ledger_append(const std::string& json_in) {
        uint64_t event_id = 0;
        int rc = cf_ledger_append(handle_, json_in.c_str(), &event_id);
        cf_checked(rc, "cf_ledger_append");
        return event_id;
    }

    std::string ledger_query_json(const std::string& json_in = "{}") {
        char* raw = cf_ledger_query(handle_, json_in.c_str());
        if (!raw) { cf_soft(-1, __func__); return "[]"; }
        std::string result(raw);
        cf_free_string(raw);
        return result;
    }

    uint32_t ledger_compile() {
        uint32_t count = 0;
        cf_ledger_compile(handle_, &count);
        return count;
    }

    std::string ledger_contradictions_json() {
        char* raw = cf_ledger_contradictions(handle_);
        if (!raw) { cf_soft(-1, __func__); return "[]"; }
        std::string result(raw);
        cf_free_string(raw);
        return result;
    }

    int64_t predicate_attach(uint64_t memory_id, const std::string& check_cmd) {
        return cf_predicate_attach(handle_, memory_id, check_cmd.c_str());
    }

    std::string predicate_run_json(uint64_t memory_id) {
        char* raw = cf_predicate_run(handle_, memory_id);
        if (!raw) { cf_soft(-1, __func__); return "{}"; }
        std::string result(raw);
        cf_free_string(raw);
        return result;
    }

    std::string predicate_list_json(uint64_t memory_id) {
        char* raw = cf_predicate_list(handle_, memory_id);
        if (!raw) { cf_soft(-1, __func__); return "{}"; }
        std::string result(raw);
        cf_free_string(raw);
        return result;
    }

private:
    CfHandle* handle_ = nullptr;

    std::string last_error() const {
        if (!handle_) return "handle is null";
        const char* e = cf_last_error(handle_);
        return e ? e : "unknown error";
    }

    // ── FFI error policy (single envelope; see ffi.rs json_null/err) ─────
    // rc != 0 (or a NULL JSON pointer) means cf_last_error() carries context.
    //   cf_checked — must-succeed calls: throw with call-site context.
    //   cf_soft    — read paths where empty-result degradation is acceptable:
    //                log (budgeted per process) and return false. Never silent.
    void cf_checked(int rc, const char* what) const {
        if (rc != 0)
            throw std::runtime_error(std::string(what) + ": " + last_error());
    }
    bool cf_soft(int rc, const char* what) const {
        if (rc == 0) return true;
        static std::atomic<int> budget{500};
        if (budget.fetch_sub(1, std::memory_order_relaxed) > 0)
            std::cerr << "[field_store] " << what << " failed: " << last_error() << "\n";
        return false;
    }

    /// Enrich CfRecallHit with content/kind/realm strings.
    std::vector<FieldRecallHit> hits_to_results(const CfRecallHit* buf, size_t n) {
        std::vector<FieldRecallHit> out;
        out.reserve(n);
        char strbuf[65536];

        for (size_t i = 0; i < n; ++i) {
            FieldRecallHit h;
            h.memory_id        = buf[i].memory_id;
            h.score            = buf[i].score;
            h.semantic_score   = buf[i].semantic_score;
            h.ts_ms            = buf[i].ts_ms;
            h.strength         = buf[i].strength;
            h.confidence       = buf[i].confidence;
            h.access_count     = buf[i].access_count;
            h.semantic_weight  = buf[i].semantic_weight;
            h.status_mul       = buf[i].status_mul;
            h.epistemic_mul    = buf[i].epistemic_mul;
            h.strength_factor  = buf[i].strength_factor;
            h.affect_valence   = buf[i].affect_valence;
            h.affect_arousal   = buf[i].affect_arousal;
            h.actr_activation  = buf[i].actr_activation;
            h.surprise_boost   = buf[i].surprise_boost;
            h.arousal_boost    = buf[i].arousal_boost;
            h.mood_congruence  = buf[i].mood_congruence;
            h.frustration_boost   = buf[i].frustration_boost;
            h.interference_factor = buf[i].interference_factor;
            h.spacing_boost       = buf[i].spacing_boost;

            // Fetch content (handles oversized payloads via heap retry)
            h.content = get_content(h.memory_id);

            // Fetch kind (null-terminated, no written out-param)
            strbuf[0] = '\0';
            if (cf_get_kind(handle_, h.memory_id,
                            reinterpret_cast<uint8_t*>(strbuf),
                            sizeof(strbuf)) == 0) {
                h.kind = strbuf;
            }

            // Fetch realm (null-terminated, no written out-param)
            strbuf[0] = '\0';
            if (cf_get_realm(handle_, h.memory_id,
                             reinterpret_cast<uint8_t*>(strbuf),
                             sizeof(strbuf)) == 0) {
                h.realm = strbuf;
            }

            out.push_back(std::move(h));
        }
        return out;
    }
};

} // namespace chitta
