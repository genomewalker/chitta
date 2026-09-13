// Included into FieldRpcHandler class body — not a standalone header, declarations only.
// Bodies live in chitta/src/handlers/field_memory_recall.cpp.
// Memory tools: remember, recall, strengthen, weaken, forget, observe, grow,
// hybrid_recall, smart_recall, recall_keyword, recall_temporal, full_resonate.

    // ── Shared hit-line renderer ─────────────────────────────────────────────
    // Line contract (parsed by hooks/prompt-core.sh and the outcome-ledger tap):
    //   #<id> [<pct>%][ [<type>]][ (on: YYYY-MM-DD)] <text truncated to 400>
    // Both parse `^#([0-9]+)` and the bracketed percent; dropping the id prefix
    // silently kills exposure/credit tracking (it happened once). Every recall
    // lane renders through format_hits() so a contract change cannot drift
    // across call sites — the lanes differ only in the flags below.
    struct HitLineOpts {
        bool show_type    = true;   // append ` [<type>]`
        bool show_date    = false;  // append ` (on: YYYY-MM-DD)` when ts_ms > 0
        bool link_atoms   = false;  // emit the `↳` span sub-line, set r["atoms"]
        bool norm_lexical = false;  // keyword lane: max-normalize pct over `lexical`
        bool record_pct   = false;  // write the printed value back as r["display_pct"]
    };
    void format_hits(std::ostringstream& ss, json& results, const std::string& query,
                     const HitLineOpts& opts);

    void fire_recall_callback(const json& results, int passes) {
        if (!recall_callback_ || !results.is_array() || results.empty()) return;
        std::vector<uint64_t> ids;
        ids.reserve(results.size());
        for (const auto& r : results) {
            std::string id_str = r.value("id", "");
            if (!id_str.empty()) {
                try { ids.push_back(std::stoull(id_str)); } catch (...) {}
            }
        }
        if (!ids.empty()) recall_callback_(ids, passes);
    }

    // ── Iterative resonance helpers ────────────────────────────────────────

    static constexpr int    kMaxResonancePasses   = 3;
    static constexpr float  kResonanceAlpha       = 0.7f;
    static constexpr float  kEntropyStopDelta     = 0.01f;
    static constexpr float  kBaseAssocDelta       = 0.03f;
    static constexpr size_t kRetrievalCtxDims     = 32;

    static float score_entropy(const std::vector<std::pair<uint64_t, float>>& scored) {
        if (scored.empty()) return 0.0f;
        float sum = 0.0f;
        for (const auto& [id, s] : scored) sum += s;
        if (sum < 1e-9f) return 0.0f;
        float h = 0.0f;
        for (const auto& [id, s] : scored) {
            float p = s / sum;
            if (p > 1e-9f) h -= p * std::log2f(p);
        }
        return h;
    }

    static uint64_t query_context_hash(const std::vector<float>& q, const std::string& realm) {
        uint64_t h = 14695981039346656037ULL;
        for (float v : q) {
            uint32_t bits;
            std::memcpy(&bits, &v, sizeof(bits));
            h ^= bits;
            h *= 1099511628211ULL;
        }
        for (char c : realm) {
            h ^= static_cast<uint8_t>(c);
            h *= 1099511628211ULL;
        }
        return h;
    }

    static void project_to_sketch(const std::vector<float>& q,
                                   std::vector<int8_t>& out_q,
                                   float& out_scale) {
        out_q.resize(kRetrievalCtxDims);
        size_t stride = q.size() / kRetrievalCtxDims;
        if (stride == 0) stride = 1;
        float max_abs = 0.0f;
        std::vector<float> sketch(kRetrievalCtxDims, 0.0f);
        for (size_t i = 0; i < kRetrievalCtxDims && i * stride < q.size(); ++i) {
            float sum = 0.0f;
            int cnt = 0;
            for (size_t j = i * stride; j < std::min((i + 1) * stride, q.size()); ++j) {
                sum += q[j]; ++cnt;
            }
            sketch[i] = cnt > 0 ? sum / cnt : 0.0f;
            max_abs = std::max(max_abs, std::abs(sketch[i]));
        }
        out_scale = max_abs > 1e-9f ? max_abs / 127.0f : 1.0f;
        for (size_t i = 0; i < kRetrievalCtxDims; ++i) {
            out_q[i] = static_cast<int8_t>(std::clamp(sketch[i] / out_scale, -127.0f, 127.0f));
        }
    }



    // ── Strength/forget ops ──────────────────────────────────────────────────

    ToolResult tool_remember(const json& params);
    ToolResult tool_remember_batch(const json& params);
    ToolResult tool_flush_embeddings(const json& params);
    ToolResult tool_recall(const json& params);
    ToolResult tool_recall_temporal(const json& params);
    ToolResult tool_recall_temporal_events(const json& params);
    ToolResult tool_recall_keyword(const json& params);
    ToolResult tool_provenance_check(const json& params);
    ToolResult tool_correction_check(const json& params);
    ToolResult tool_task_state(const json& params);
    ToolResult tool_hybrid_recall(const json& params);
    ToolResult tool_smart_recall(const json& params);
    ToolResult tool_recall_lanes(const json& params);
    ToolResult tool_recall_session(const json& params);
    ToolResult tool_recall_spreading(const json& params);
    ToolResult tool_full_resonate(const json& params);
    ToolResult tool_route_stats(const json&);
    // CEC: Event tape + CDAWG
    ToolResult tool_log_event(const json& params);
    ToolResult tool_log_event_ex(const json& params);
    ToolResult tool_log_decision(const json& params);
    ToolResult tool_recall_last_action(const json& params);
    ToolResult tool_recall_failure_pattern(const json& params);
    ToolResult tool_recall_causal_antecedent(const json& params);
    ToolResult tool_recall_hdcbind(const json& params);
    ToolResult tool_recall_counterfactual(const json& params);
    ToolResult tool_consolidation_pass(const json& params);
    ToolResult tool_refutation_stats(const json& params);
    ToolResult tool_recall_motif_value(const json& params);
    // Analogy Lane: VSA over the triplet lane — structure, not surface content
    ToolResult tool_recall_analogy(const json& params);
    // Span Lane: verbatim transcript atoms (realm-scoped, no LLM/GPU on query path)
    ToolResult tool_span_query(const json& params);
    ToolResult tool_span_backfill(const json& params);
    ToolResult tool_span_backfill_memories(const json& params);
    ToolResult tool_densify_backfill(const json& params);
    ToolResult tool_semantic_backfill(const json& params);
    ToolResult tool_assoc_census(const json& params);
    ToolResult tool_assoc_decay(const json& params);
    ToolResult tool_span_stats(const json& params);
    ToolResult tool_executor_flush(const json& params);
    ToolResult tool_list_policies(const json& params);
    ToolResult tool_recall_true_counterfactual(const json& params);
    ToolResult tool_hypothesis_probes(const json& params);
    ToolResult tool_turiya_status(const json& params);
    ToolResult tool_tape_stats(const json& params);
    ToolResult tool_verbalize_rules(const json& params);
    ToolResult tool_queue_experiments(const json& params);
    ToolResult tool_fep_status(const json& params);
    ToolResult tool_routed_recall(const json& params);
    ToolResult tool_witness_memory(const json& params);
    ToolResult tool_reconcile_pass(const json& params);
    ToolResult tool_harvest_scope(const json& params);
    ToolResult tool_seed_hdc_geometry(const json& params);
    ToolResult tool_ledger_append(const json& params);
    ToolResult tool_ledger_query(const json& params);
    ToolResult tool_ledger_compile(const json& params);
    ToolResult tool_ledger_contradictions(const json& params);
    ToolResult tool_predicate_attach(const json& params);
    ToolResult tool_predicate_run(const json& params);
    ToolResult tool_predicate_list(const json& params);
