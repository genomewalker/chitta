// field_memory_ops RPC handlers — bodies for declarations in
// chitta/include/chitta/rpc/handlers/field_memory_ops.hpp.

#include "../../include/chitta/rpc/field_handler.hpp"
#include "../../include/chitta/speech_act.hpp"

namespace chitta {

ToolResult FieldRpcHandler::tool_strengthen(const json& params) {
    uint64_t id  = extract_id(params);
    float amount = params.value("amount", 0.1f);
    if (id == 0) return ToolResult::error("id is required");
    if (field_store_->get_content(id).empty()) {
        return ToolResult::error("memory not found: " + std::to_string(id));
    }
    field_store_->strengthen(id, amount);
    return ToolResult::ok("Strengthened memory #" + std::to_string(id));
}

ToolResult FieldRpcHandler::tool_weaken(const json& params) {
    uint64_t id  = extract_id(params);
    float amount = params.value("amount", 0.1f);
    if (id == 0) return ToolResult::error("id is required");
    if (field_store_->get_content(id).empty()) {
        return ToolResult::error("memory not found: " + std::to_string(id));
    }
    field_store_->weaken(id, amount);
    return ToolResult::ok("Weakened memory #" + std::to_string(id));
}

ToolResult FieldRpcHandler::tool_forget(const json& params) {
    uint64_t id = extract_id(params);
    if (id == 0) return ToolResult::error("id is required");
    if (field_store_->get_content(id).empty()) {
        return ToolResult::error("memory not found: " + std::to_string(id));
    }
    field_store_->forget(id);
    if (!field_store_->get_content(id).empty()) {
        return ToolResult::error("failed to forget memory #" + std::to_string(id));
    }
    return ToolResult::ok("Forgot memory #" + std::to_string(id));
}

ToolResult FieldRpcHandler::tool_ack_memory(const json& params) {
    uint64_t id = extract_id(params);
    if (id == 0) return ToolResult::error("id is required");
    if (field_store_->get_content(id).empty()) {
        return ToolResult::error("memory not found: " + std::to_string(id));
    }
    field_store_->ack_memory(id);
    return ToolResult::ok("Acked memory #" + std::to_string(id));
}

ToolResult FieldRpcHandler::tool_nack_memory(const json& params) {
    uint64_t id = extract_id(params);
    if (id == 0) return ToolResult::error("id is required");
    if (field_store_->get_content(id).empty()) {
        return ToolResult::error("memory not found: " + std::to_string(id));
    }
    field_store_->nack_memory(id);
    return ToolResult::ok("Nacked memory #" + std::to_string(id));
}

ToolResult FieldRpcHandler::tool_memory_outcome(const json& params) {
    uint64_t id = extract_id(params);
    if (id == 0) return ToolResult::error("id is required");
    if (!params.contains("success") || !params["success"].is_boolean()) {
        return ToolResult::error("success (boolean) is required");
    }
    if (field_store_->get_content(id).empty()) {
        return ToolResult::error("memory not found: " + std::to_string(id));
    }
    bool success = params["success"].get<bool>();
    float weight = params.value("weight", 1.0f);
    // The store silently ignores non-positive/NaN weight; reject here so the
    // caller is never told "Recorded" for an observation that never landed.
    if (!(weight > 0.0f)) {
        return ToolResult::error("weight must be > 0");
    }
    float alpha = 0.0f, beta = 0.0f;
    if (!field_store_->record_outcome(id, success, weight, alpha, beta)) {
        return ToolResult::error("failed to record outcome for memory #" + std::to_string(id));
    }
    return ToolResult::ok("Recorded " + std::string(success ? "success" : "failure") +
                          " for memory #" + std::to_string(id) +
                          " (alpha=" + std::to_string(alpha) +
                          " beta=" + std::to_string(beta) + ")");
}

ToolResult FieldRpcHandler::tool_batch_forget(const json& params) {
    size_t count = 0;
    if (params.contains("ids") && params["ids"].is_array()) {
        for (const auto& v : params["ids"]) {
            uint64_t id = 0;
            if (v.is_number_integer()) id = static_cast<uint64_t>(v.get<int64_t>());
            else if (v.is_string()) { try { id = std::stoull(v.get<std::string>()); } catch (...) {} }
            if (id != 0) { field_store_->forget(id); count++; }
        }
    }
    if (params.contains("pattern") && params["pattern"].is_string()) {
        std::string pattern = params["pattern"].get<std::string>();
        auto hits = field_store_->recall_keyword(pattern, 100);
        for (const auto& h : hits) {
            field_store_->forget(h.memory_id);
            count++;
        }
    }
    return ToolResult::ok("Forgot " + std::to_string(count) + " memories", {{"count", count}});
}

ToolResult FieldRpcHandler::tool_observe(const json& params) {
    std::string title   = params.value("title", "");
    std::string content = params.value("content", "");
    if (content.empty()) return ToolResult::error("content is required");

    std::string category = params.value("category", "episode");
    std::string realm    = params.value("realm", "brahman");
    std::string source   = params.value("source", "mcp_tool");
    std::string evidence = params.value("evidence", "");
    float confidence = params.contains("confidence")
        ? params["confidence"].get<float>()
        : category_to_confidence(category);

    // Auto speech-act classification for episode memories
    if (category == "episode") {
        if (auto act = classify_speech_act(content)) category = *act;
    }

    std::string ssl_content = to_ssl_format(content, category);

    std::vector<float> embedding;
    if (params.contains("_preembedding")) {
        embedding = params["_preembedding"].get<std::vector<float>>();
    } else {
        embedding = embed_text(ssl_content);
    }
    uint64_t id = field_store_->remember(category, realm, ssl_content, embedding, confidence, 0.001f);

    // Provenance: source + evidence + epistemic status + initial lifecycle status
    if (id > 0 && !source.empty()) {
        field_store_->add_triplet(std::to_string(id), "source", source);
        if (!evidence.empty())
            field_store_->add_triplet(std::to_string(id), "evidence", evidence);
        uint8_t es = epistemic_status_for_source(source);
        uint8_t ms = initial_status_for_source(source);
        if (es != 1) field_store_->set_epistemic_status(id, es);
        if (ms != 0) field_store_->set_memory_status(id, ms);
    }
    // Auto-tag with category so --tag filter on recall works
    if (id > 0 && !category.empty() && category != "episode") {
        field_store_->add_triplet(std::to_string(id), "tagged", category);
    }
    if (params.contains("tags")) {
        if (params["tags"].is_array()) {
            for (const auto& t : params["tags"]) {
                if (t.is_string()) {
                    field_store_->add_triplet(std::to_string(id), "tagged", t.get<std::string>());
                }
            }
        } else if (params["tags"].is_string()) {
            std::istringstream iss(params["tags"].get<std::string>());
            std::string tag;
            while (std::getline(iss, tag, ',')) {
                tag.erase(0, tag.find_first_not_of(' '));
                tag.erase(tag.find_last_not_of(' ') + 1);
                if (!tag.empty()) {
                    field_store_->add_triplet(std::to_string(id), "tagged", tag);
                }
            }
        }
    }

    // Explicit supersession: force_supersede_ids bypasses cosine threshold
    // Accepts both JSON array ["id1","id2"] and comma-separated string "id1,id2"
    if (id > 0 && params.contains("force_supersede_ids")) {
        std::vector<std::string> supersede_ids;
        const auto& fsi = params["force_supersede_ids"];
        if (fsi.is_array()) {
            for (const auto& v : fsi) supersede_ids.push_back(v.get<std::string>());
        } else if (fsi.is_string()) {
            std::istringstream ss(fsi.get<std::string>());
            std::string tok;
            while (std::getline(ss, tok, ',')) {
                tok.erase(0, tok.find_first_not_of(' '));
                tok.erase(tok.find_last_not_of(' ') + 1);
                if (!tok.empty()) supersede_ids.push_back(tok);
            }
        }
        for (const auto& sid_str : supersede_ids) {
            try {
                uint64_t sid = std::stoull(sid_str);
                field_store_->add_triplet(std::to_string(id), "supersedes", sid_str, 1.0f, id);
                field_store_->weaken(sid, 0.15f);
                field_store_->set_memory_status(sid, 1); // Superseded
            } catch (...) {}
        }
    }

    // Affect dimensions: valence (-1..+1) and arousal (0..1)
    if (id > 0 && (params.contains("valence") || params.contains("arousal"))) {
        float valence = params.value("valence", 0.0f);
        float arousal = params.value("arousal", 0.0f);
        field_store_->set_affect(id, valence, arousal);
    }

    // Semantic flags: structural importance markers stored as tags
    if (id > 0 && params.contains("flags") && params["flags"].is_string()) {
        std::istringstream fss(params["flags"].get<std::string>());
        std::string flag;
        while (std::getline(fss, flag, ',')) {
            flag.erase(0, flag.find_first_not_of(' '));
            flag.erase(flag.find_last_not_of(' ') + 1);
            if (!flag.empty()) {
                field_store_->add_triplet(std::to_string(id), "has_flag", flag);
            }
        }
    }

    // Cross-references: create association edges from →@ref annotations
    if (id > 0 && params.contains("refs") && params["refs"].is_string()) {
        std::istringstream rss(params["refs"].get<std::string>());
        std::string ref;
        while (std::getline(rss, ref, ',')) {
            ref.erase(0, ref.find_first_not_of(' '));
            ref.erase(ref.find_last_not_of(' ') + 1);
            if (!ref.empty()) {
                // Try numeric ID first, fall back to tag-based lookup
                try {
                    uint64_t ref_id = std::stoull(ref);
                    field_store_->add_edge(id, ref_id, 2 /*CoRetrieved*/, 0.5f);
                } catch (...) {
                    // Store as named reference triplet
                    field_store_->add_triplet(std::to_string(id), "references", ref);
                }
            }
        }
    }

    // SSL v0.4: granularity tier (G:0-4) stored as triplet
    if (id > 0 && params.contains("granularity")) {
        int g = -1;
        if (params["granularity"].is_number_integer()) {
            g = params["granularity"].get<int>();
        } else if (params["granularity"].is_string()) {
            try { g = std::stoi(params["granularity"].get<std::string>()); } catch (...) {}
        }
        if (g >= 0 && g <= 4) {
            field_store_->add_triplet(std::to_string(id), "granularity", std::to_string(g));
        }
    }

    // SSL v0.4: derivation provenance (<=@refs) — comma-separated source memory IDs
    if (id > 0 && params.contains("derivation") && params["derivation"].is_string()) {
        std::istringstream dss(params["derivation"].get<std::string>());
        std::string dref;
        while (std::getline(dss, dref, ',')) {
            dref.erase(0, dref.find_first_not_of(' '));
            dref.erase(dref.find_last_not_of(' ') + 1);
            if (!dref.empty()) {
                field_store_->add_triplet(std::to_string(id), "abstracted_from", dref);
            }
        }
    }

    // SSL v0.4: external source grounding (src:loc)
    if (id > 0 && params.contains("source_loc") && params["source_loc"].is_string()) {
        std::string sloc = params["source_loc"].get<std::string>();
        if (!sloc.empty()) {
            field_store_->add_triplet(std::to_string(id), "source_loc", sloc);
        }
    }

    // Correction supersession: when category=correction, weaken semantically similar memories
    if (category == "correction" && id > 0 && !embedding.empty()) {
        auto hits = field_store_->recall(embedding, 5, realm);
        std::string target_id = params.value("target_id", "");
        if (!target_id.empty()) {
            // Explicit target
            try {
                uint64_t tid = std::stoull(target_id);
                field_store_->add_triplet(std::to_string(id), "supersedes", target_id, 1.0f, id);
                field_store_->weaken(tid, 0.15f);
                field_store_->set_memory_status(tid, 1); // Superseded
            } catch (...) {}
        } else {
            for (const auto& h : hits) {
                if (h.memory_id == id) continue;
                if (h.realm != realm) continue;
                if (h.kind == "correction") continue;
                if (h.semantic_score < 0.88f) continue;  // slightly lower threshold for direct calls
                field_store_->add_triplet(std::to_string(id), "supersedes", std::to_string(h.memory_id), 1.0f, id);
                field_store_->weaken(h.memory_id, 0.15f);
                field_store_->set_memory_status(h.memory_id, 1);
            }
        }
    }

    return ToolResult::ok("Observed [" + category + "] #" + std::to_string(id),
        {{"id", std::to_string(id)}, {"category", category}, {"confidence", confidence}});
}

ToolResult FieldRpcHandler::tool_grow(const json& params) {
    std::string type    = params.value("type", "wisdom");
    std::string content = params.value("content", "");
    if (content.empty()) return ToolResult::error("content is required");

    std::string title = params.value("title", "");
    std::string ssl = title.empty() ? to_ssl_format(content, type) :
        "[" + type + "] " + title + "\n" + content;

    std::vector<float> embedding;
    if (params.contains("_preembedding")) {
        embedding = params["_preembedding"].get<std::vector<float>>();
    } else {
        embedding = embed_text(ssl);
    }
    float confidence = (type == "wisdom" || type == "belief") ? 0.85f : 0.70f;
    uint64_t id = field_store_->remember(type, "brahman", ssl, embedding, confidence, 0.001f);

    if (params.contains("tags")) {
        if (params["tags"].is_array()) {
            for (const auto& t : params["tags"]) {
                if (t.is_string()) {
                    field_store_->add_triplet(std::to_string(id), "tagged", t.get<std::string>());
                }
            }
        } else if (params["tags"].is_string()) {
            std::istringstream iss(params["tags"].get<std::string>());
            std::string tag;
            while (std::getline(iss, tag, ',')) {
                tag.erase(0, tag.find_first_not_of(' '));
                tag.erase(tag.find_last_not_of(' ') + 1);
                if (!tag.empty()) field_store_->add_triplet(std::to_string(id), "tagged", tag);
            }
        }
    }

    return ToolResult::ok("Grew " + type + " #" + std::to_string(id),
        {{"id", std::to_string(id)}, {"type", type}});
}

ToolResult FieldRpcHandler::tool_set_affect(const json& params) {
    uint64_t id = extract_id(params);
    if (id == 0) return ToolResult::error("id is required");
    float valence = params.value("valence", 0.0f);
    float arousal = params.value("arousal", 0.0f);
    field_store_->set_affect(id, valence, arousal);
    return ToolResult::ok("Set affect on #" + std::to_string(id) +
        " (v=" + std::to_string(valence) + " a=" + std::to_string(arousal) + ")",
        {{"id", std::to_string(id)}, {"valence", valence}, {"arousal", arousal}});
}

ToolResult FieldRpcHandler::tool_get(const json& params) {
    uint64_t id = extract_id(params);
    if (id == 0) return ToolResult::error("id is required");

    std::string content = field_store_->get_content(id);
    if (content.empty()) return ToolResult::error("Memory not found: " + std::to_string(id));

    std::string meta_json = field_store_->get_memory_metadata(id);
    json meta = meta_json.empty() ? json::object() : json::parse(meta_json, nullptr, false);

    json result;
    result["id"] = std::to_string(id);
    result["content"] = content;
    if (meta.is_object()) {
        result["type"]       = meta.value("kind", "");
        result["realm"]      = meta.value("realm", "");
        result["confidence"] = meta.value("confidence", 0.0f);
        result["strength"]   = meta.value("strength", 0.0f);
    }

    return ToolResult::ok(content.substr(0, 500), result);
}

ToolResult FieldRpcHandler::tool_get_embeddings(const json& params) {
    if (!params.contains("ids")) return ToolResult::error("ids array required");
    std::vector<uint64_t> ids;
    for (const auto& v : params["ids"]) {
        uint64_t id = 0;
        if (v.is_string()) id = std::stoull(v.get<std::string>());
        else if (v.is_number()) id = v.get<uint64_t>();
        if (id) ids.push_back(id);
    }
    if (ids.empty()) return ToolResult::error("No valid ids");
    std::string emb_json = field_store_->get_memory_embeddings_batch(ids);
    json result = json::parse(emb_json, nullptr, false);
    if (result.is_discarded()) return ToolResult::error("Failed to fetch embeddings");
    return ToolResult::ok("Embeddings for " + std::to_string(ids.size()) + " memories", result);
}

ToolResult FieldRpcHandler::tool_expand_memory(const json& params) {
    uint64_t id = extract_id(params);
    if (id == 0) return ToolResult::error("id is required");

    std::string content = field_store_->get_content(id);
    if (content.empty()) return ToolResult::error("Memory not found");

    std::string meta_json = field_store_->get_memory_metadata(id);
    json result;
    result["id"] = std::to_string(id);
    result["content"] = content;
    if (!meta_json.empty()) {
        json meta = json::parse(meta_json, nullptr, false);
        if (meta.is_object()) result["metadata"] = meta;
    }

    // Expand associations
    std::vector<uint64_t> seeds = {id};
    auto assoc = field_store_->expand_associations(seeds, 2, 10);
    if (!assoc.empty()) {
        result["associations"] = hits_to_results_json(assoc);
    }

    return ToolResult::ok(content, result);
}

ToolResult FieldRpcHandler::tool_update(const json& params) {
    uint64_t id = extract_id(params);
    std::string content = params.value("content", "");
    if (id == 0 || content.empty()) return ToolResult::error("id and content are required");

    // Re-store: forget old, remember new with same kind/realm
    std::string meta_json = field_store_->get_memory_metadata(id);
    std::string kind = "episode", realm = "brahman";
    float confidence = 0.8f;
    if (!meta_json.empty()) {
        json meta = json::parse(meta_json, nullptr, false);
        if (meta.is_object()) {
            kind = meta.value("kind", kind);
            realm = meta.value("realm", realm);
            confidence = meta.value("confidence", confidence);
        }
    }

    field_store_->forget(id);
    std::vector<float> embedding;
    if (params.contains("_preembedding")) {
        embedding = params["_preembedding"].get<std::vector<float>>();
    } else {
        embedding = embed_text(content);
    }
    uint64_t new_id = field_store_->remember(kind, realm, content, embedding, confidence, 0.001f);

    // Link old→new for audit
    field_store_->add_triplet(std::to_string(new_id), "updated_from", std::to_string(id));

    return ToolResult::ok("Updated → #" + std::to_string(new_id),
        {{"old_id", std::to_string(id)}, {"new_id", std::to_string(new_id)}});
}

ToolResult FieldRpcHandler::tool_query(const json& params) {
    std::string subject = params.value("subject", "");
    std::string object  = params.value("object", "");

    json results_json = json::array();
    std::ostringstream ss;

    if (!subject.empty()) {
        std::string raw = field_store_->query_subject(subject);
        try {
            auto arr = json::parse(raw);
            for (const auto& t : arr) {
                ss << "  " << subject << " → " << t.value("predicate", "?") << " → " << t.value("object", "?") << "\n";
                results_json.push_back(t);
            }
        } catch (...) {}
    }
    if (!object.empty()) {
        std::string raw = field_store_->query_object(object);
        try {
            auto arr = json::parse(raw);
            for (const auto& t : arr) {
                ss << "  " << t.value("subject", "?") << " → " << t.value("predicate", "?") << " → " << object << "\n";
                results_json.push_back(t);
            }
        } catch (...) {}
    }

    if (subject.empty() && object.empty()) {
        return ToolResult::error("subject or object required");
    }

    return ToolResult::ok(ss.str(), {{"triplets", results_json}});
}

ToolResult FieldRpcHandler::tool_tag(const json& params) {
    uint64_t id = extract_id(params);
    if (id == 0) return ToolResult::error("id is required");

    std::string add_tag = params.value("add", "");
    std::string rm_tag  = params.value("remove", "");

    if (!add_tag.empty()) {
        field_store_->add_triplet(std::to_string(id), "tagged", add_tag);
    }
    if (!rm_tag.empty()) {
        bool removed = field_store_->forget_triplet(std::to_string(id), "tagged", rm_tag);
        if (!removed) return ToolResult::ok("Tag not found", {{"id", std::to_string(id)}, {"tag", rm_tag}});
    }

    return ToolResult::ok("OK", {{"id", std::to_string(id)}});
}

ToolResult FieldRpcHandler::tool_explore_recall(const json& params) {
    std::string query = params.value("query", "");
    if (query.empty()) return ToolResult::error("query is required");
    size_t limit = static_cast<size_t>(params.value("limit", 10));

    auto embedding = embed_query(query);

    std::vector<FieldRecallHit> hits;
    if (!embedding.empty()) {
        hits = field_store_->recall(embedding, limit);
    } else {
        hits = field_store_->recall_keyword(query, limit);
    }
    json results = json::array();
    std::ostringstream ss;
    for (const auto& h : hits) {
        std::string preview = h.content.substr(0, 80);
        results.push_back({
            {"id", std::to_string(h.memory_id)},
            {"score", h.score},
            {"type", h.kind},
            {"preview", preview}
        });
        int pct = static_cast<int>(h.score * 100);
        ss << "[" << pct << "%] #" << h.memory_id << " " << preview << "\n";
    }
    return ToolResult::ok(ss.str(), {{"results", results}});
}

ToolResult FieldRpcHandler::tool_explore_peek(const json& params) {
    uint64_t id = extract_id(params);
    if (id == 0) return ToolResult::error("id is required");
    std::string content = field_store_->get_content(id);
    if (content.empty()) return ToolResult::error("Not found");
    std::string preview = content.substr(0, 200);
    return ToolResult::ok(preview, {{"id", std::to_string(id)}, {"preview", preview}});
}

ToolResult FieldRpcHandler::tool_explore_expand(const json& params) {
    return tool_get(params);
}

ToolResult FieldRpcHandler::tool_explore_neighbors(const json& params) {
    std::string node = params.value("node", "");
    if (node.empty()) return ToolResult::error("node is required");

    std::string raw = field_store_->list_triplets_for_entity(node, 50);
    try {
        auto arr = json::parse(raw);
        std::ostringstream ss;
        ss << "Neighbors of '" << node << "':\n";
        for (const auto& t : arr) {
            ss << "  " << t.value("subject", "?") << " → " << t.value("predicate", "?")
               << " → " << t.value("object", "?") << "\n";
        }
        return ToolResult::ok(ss.str(), {{"triplets", arr}});
    } catch (...) {
        return ToolResult::ok(raw);
    }
}

ToolResult FieldRpcHandler::tool_list_memories_brief(const json& params) {
    size_t limit      = static_cast<size_t>(params.value("limit", 200));
    size_t offset     = static_cast<size_t>(params.value("offset", 0));
    std::string realm = params.value("realm", "");
    std::string kind  = params.value("kind", "");

    std::string raw = field_store_->list_memories(kind, realm, "recency", limit, offset);
    try {
        auto arr = json::parse(raw);
        std::ostringstream ss;
        json results = json::array();
        for (const auto& m : arr) {
            std::string content = m.value("content", "");
            // Truncate on a UTF-8 boundary: a byte-slice at 80 can split a
            // multibyte char (e.g. an em-dash straddling byte 80), leaving an
            // invalid sequence. brief.dump() below uses the throwing handler, so
            // a bad preview aborts the whole page → the CLI --json sees `null`.
            size_t cut = std::min<size_t>(content.size(), 80);
            while (cut > 0 && (static_cast<unsigned char>(content[cut]) & 0xC0) == 0x80) --cut;
            std::string preview = content.substr(0, cut);
            uint64_t mid = m["id"].is_number() ? m["id"].get<uint64_t>() : 0;
            json brief = {
                {"id",         mid},
                {"kind",       m.value("kind", "")},
                {"realm",      m.value("realm", "")},
                {"tags",       m.value("tags", json::array())},
                {"content",    content},
                {"preview",    preview},
                {"confidence", m.value("confidence", 0.0f)},
            };
            results.push_back(brief);
            // Use the replacing handler: a memory whose stored content is not
            // valid UTF-8 must not throw and null the entire page.
            ss << brief.dump(-1, ' ', false, json::error_handler_t::replace) << "\n";
        }
        return ToolResult::ok(ss.str(), {{"memories", results}, {"count", results.size()}});
    } catch (...) {
        return ToolResult::ok(raw);
    }
}

ToolResult FieldRpcHandler::tool_forget_kind(const json& params) {
    std::string kind  = params.value("kind", "");
    std::string realm = params.value("realm", "");
    size_t limit      = static_cast<size_t>(params.value("limit", 5000));
    if (kind.empty()) return ToolResult::error("kind is required");

    std::string raw = field_store_->list_memories(kind, realm, "recency", limit, 0);
    json arr;
    try { arr = json::parse(raw); } catch (...) {
        return ToolResult::error("failed to list memories");
    }

    size_t deleted = 0, failed = 0;
    for (const auto& m : arr) {
        uint64_t id = 0;
        if (m["id"].is_number()) id = m["id"].get<uint64_t>();
        else if (m["id"].is_string()) {
            try { id = std::stoull(m["id"].get<std::string>()); } catch (...) {}
        }
        if (id == 0) { ++failed; continue; }
        try { field_store_->forget(id); ++deleted; } catch (...) { ++failed; }
    }

    return ToolResult::ok(
        "Deleted " + std::to_string(deleted) + " " + kind + " memories"
        + (failed ? " (" + std::to_string(failed) + " failed)" : ""),
        {{"deleted", deleted}, {"failed", failed}, {"kind", kind}});
}

ToolResult FieldRpcHandler::tool_set_priority_tier(const json& params) {
    // Priority tiers stored as triplets
    uint64_t id = extract_id(params, "memory_id");
    int tier = params.value("tier", 0);
    if (id == 0) return ToolResult::error("memory_id is required");
    field_store_->add_triplet(std::to_string(id), "priority_tier", std::to_string(tier));
    return ToolResult::ok("Set tier " + std::to_string(tier) + " for #" + std::to_string(id));
}

ToolResult FieldRpcHandler::tool_set_memory_type(const json& params) {
    uint64_t id = extract_id(params, "memory_id");
    std::string type = params.value("type", "");
    if (id == 0 || type.empty()) return ToolResult::error("memory_id and type are required");
    field_store_->update_memory_kind(id, type);
    return ToolResult::ok("Set type to '" + type + "' for #" + std::to_string(id));
}

ToolResult FieldRpcHandler::tool_memory_type_stats(const json& params) {
    std::string realm = params.value("realm", "");
    std::string raw = field_store_->memory_stats(realm);
    try {
        auto j = json::parse(raw);
        return ToolResult::ok(j.dump(2), j);
    } catch (...) {
        return ToolResult::ok(raw);
    }
}

ToolResult FieldRpcHandler::tool_recall_by_priority(const json& params) {
    // Simple: recall by strength (high confidence first = de-facto priority)
    std::string query = params.value("query", "");
    std::string realm = params.value("realm", "");
    size_t budget     = static_cast<size_t>(params.value("budget_tokens", 4000));

    std::string raw = field_store_->recall_filtered("", realm, 0.5f, 0.0f, 50);
    try {
        auto arr = json::parse(raw);
        json results = json::array();
        size_t tokens_used = 0;
        for (const auto& m : arr) {
            std::string content = m.value("content", "");
            size_t est_tokens = content.size() / 4;
            if (tokens_used + est_tokens > budget) break;
            tokens_used += est_tokens;
            results.push_back(m);
        }
        return ToolResult::ok("Priority recall: " + std::to_string(results.size()) + " memories",
            {{"results", results}, {"tokens_used", tokens_used}});
    } catch (...) {
        return ToolResult::ok(raw);
    }
}

ToolResult FieldRpcHandler::tool_expand_query(const json& params) {
    std::string query = params.value("query", "");
    if (query.empty()) return ToolResult::error("query is required");
    auto eq = expand_query(query);
    return ToolResult::ok(
        "lex: " + eq.lex + "\nvec: " + eq.vec + "\nhyde: " + eq.hyde,
        {{"lex", eq.lex}, {"vec", eq.vec}, {"hyde", eq.hyde}});
}

ToolResult FieldRpcHandler::tool_connect(const json& params) {
    std::string subject   = params.value("subject", "");
    std::string predicate = params.value("predicate", "");
    std::string object    = params.value("object", "");
    if (subject.empty() || predicate.empty() || object.empty())
        return ToolResult::error("subject, predicate, and object are required");
    field_store_->add_triplet(subject, predicate, object);
    return ToolResult::ok(subject + " --[" + predicate + "]--> " + object,
        {{"subject", subject}, {"predicate", predicate}, {"object", object}});
}

ToolResult FieldRpcHandler::tool_connect_temporal(const json& params) {
    std::string subject    = params.value("subject", "");
    std::string predicate  = params.value("predicate", "");
    std::string object     = params.value("object", "");
    std::string valid_from = params.value("valid_from", "");
    std::string valid_to   = params.value("valid_to", "");
    if (subject.empty() || predicate.empty() || object.empty())
        return ToolResult::error("subject, predicate, and object are required");
    field_store_->add_triplet(subject, predicate, object);
    json payload = {{"valid_from", valid_from}, {"valid_to", valid_to}};
    field_store_->emit_event("triplet", "temporal", subject + "|" + predicate + "|" + object,
                             payload.dump());
    return ToolResult::ok(subject + " --[" + predicate + "]--> " + object + " (temporal)",
        {{"subject", subject}, {"predicate", predicate}, {"object", object},
         {"valid_from", valid_from}, {"valid_to", valid_to}});
}

ToolResult FieldRpcHandler::tool_triplet_history(const json& params) {
    std::string subject   = params.value("subject", "");
    std::string predicate = params.value("predicate", "");
    if (subject.empty() || predicate.empty())
        return ToolResult::error("subject and predicate are required");
    auto raw = field_store_->query_subject(subject);
    auto triplets = json::parse(raw, nullptr, false);
    json results = json::array();
    if (!triplets.is_discarded() && triplets.is_array()) {
        for (const auto& t : triplets) {
            if (t.value("predicate", "") == predicate)
                results.push_back(t);
        }
    }
    return ToolResult::ok(std::to_string(results.size()) + " history entries",
        {{"subject", subject}, {"predicate", predicate}, {"history", results}});
}

ToolResult FieldRpcHandler::tool_query_triplets_temporal(const json& params) {
    std::string subject   = params.value("subject", "");
    std::string predicate = params.value("predicate", "");
    std::string object    = params.value("object", "");
    // Query by subject or object
    json results = json::array();
    if (!subject.empty()) {
        auto raw = field_store_->query_subject(subject);
        auto triplets = json::parse(raw, nullptr, false);
        if (!triplets.is_discarded() && triplets.is_array()) {
            for (const auto& t : triplets) {
                if (!predicate.empty() && t.value("predicate", "") != predicate) continue;
                if (!object.empty() && t.value("object", "") != object) continue;
                results.push_back(t);
            }
        }
    } else if (!object.empty()) {
        auto raw = field_store_->query_object(object);
        auto triplets = json::parse(raw, nullptr, false);
        if (!triplets.is_discarded() && triplets.is_array()) {
            for (const auto& t : triplets) {
                if (!predicate.empty() && t.value("predicate", "") != predicate) continue;
                results.push_back(t);
            }
        }
    }
    return ToolResult::ok(std::to_string(results.size()) + " triplet(s)",
        {{"triplets", results}, {"count", results.size()}});
}

ToolResult FieldRpcHandler::tool_triplet_query_as_of(const json& params) {
    if (!field_store_->triplets_ready()) return triplets_loading();
    std::string subject = params.value("subject", "");
    if (subject.empty()) return ToolResult::error("subject is required");
    int64_t world_ms = params.value("world_ms", int64_t(0));
    if (world_ms == 0) {
        world_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
    }
    std::string raw = field_store_->query_subject_as_of(subject, world_ms);
    auto triplets = json::parse(raw, nullptr, false);
    if (triplets.is_discarded()) triplets = json::array();
    return ToolResult::ok(std::to_string(triplets.size()) + " triplet(s) as of " + std::to_string(world_ms),
        {{"subject", subject}, {"world_ms", world_ms}, {"triplets", triplets}});
}

ToolResult FieldRpcHandler::tool_triplet_supersede(const json& params) {
    uint64_t old_id = params.value("old_id", uint64_t(0));
    uint64_t new_id = params.value("new_id", uint64_t(0));
    if (old_id == 0 || new_id == 0) return ToolResult::error("old_id and new_id are required");
    int64_t at_ms = params.value("at_ms", int64_t(0));
    if (at_ms == 0) {
        at_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
    }
    field_store_->triplet_supersede(old_id, new_id, at_ms);
    return ToolResult::ok("Triplet #" + std::to_string(old_id) + " superseded by #" + std::to_string(new_id),
        {{"old_id", old_id}, {"new_id", new_id}, {"at_ms", at_ms}});
}

ToolResult FieldRpcHandler::tool_graph_traverse(const json& params) {
    std::string start = params.value("start", "");
    if (start.empty()) return ToolResult::error("start is required");
    json bounded_edges = params.value("edge_types", json::array());
    if (!bounded_edges.is_array()) bounded_edges = json::array();
    if (bounded_edges.size() > 32) {
        std::cerr << "[rpc] clamp tool=graph_traverse param=edge_types from="
                  << bounded_edges.size() << " to=32\n";
        bounded_edges.erase(bounded_edges.begin() + 32, bounded_edges.end());
    }
    std::string edge_types_json = bounded_edges.dump();
    size_t max_hops    = params.value("max_hops",    3);
    size_t max_results = params.value("max_results", 50);
    std::string direction = params.value("direction", "outgoing");
    std::string raw = field_store_->graph_traverse(
        start, edge_types_json, max_hops, max_results, direction, 10000);
    auto hits = json::parse(raw, nullptr, false);
    if (hits.is_discarded()) hits = json::array();
    return ToolResult::ok(std::to_string(hits.size()) + " node(s) reachable from " + start,
        {{"start", start}, {"direction", direction}, {"hits", hits}});
}

ToolResult FieldRpcHandler::tool_graph_pagerank(const json& params) {
    if (!params.contains("seeds") || !params["seeds"].is_array())
        return ToolResult::error("seeds array is required");
    json bounded_seeds = params["seeds"];
    if (bounded_seeds.size() > 32) {
        std::cerr << "[rpc] clamp tool=graph_pagerank param=seeds from="
                  << bounded_seeds.size() << " to=32\n";
        bounded_seeds.erase(bounded_seeds.begin() + 32, bounded_seeds.end());
    }
    std::string seeds_json = bounded_seeds.dump();
    json bounded_edges = params.value("edge_types", json::array());
    if (!bounded_edges.is_array()) bounded_edges = json::array();
    if (bounded_edges.size() > 32) {
        std::cerr << "[rpc] clamp tool=graph_pagerank param=edge_types from="
                  << bounded_edges.size() << " to=32\n";
        bounded_edges.erase(bounded_edges.begin() + 32, bounded_edges.end());
    }
    std::string edge_types_json = bounded_edges.dump();
    float   damping    = params.value("damping",    0.85f);
    uint8_t iterations = params.value("iterations", uint8_t(20));
    size_t  top_k      = params.value("top_k",      size_t(20));
    std::string raw = field_store_->graph_pagerank(
        seeds_json, edge_types_json, damping, iterations, top_k, 512, 20000);
    auto ranked = json::parse(raw, nullptr, false);
    if (ranked.is_discarded()) ranked = json::array();
    return ToolResult::ok(std::to_string(ranked.size()) + " nodes ranked",
        {{"seeds", bounded_seeds}, {"ranked", ranked}});
}

ToolResult FieldRpcHandler::tool_list_by_status(const json& params) {
    std::string status_filter = params.value("status", "superseded");
    size_t limit = params.value("limit", 50);
    std::string realm = params.value("realm", "");

    // Over-fetch to account for filtering
    std::string raw = field_store_->list_memories("", realm, "recency", limit * 3, 0);
    json all_mems = json::parse(raw, nullptr, false);
    if (all_mems.is_discarded() || !all_mems.is_array()) {
        return ToolResult::ok("0 memories with status=" + status_filter,
            {{"memories", json::array()}, {"status_filter", status_filter}});
    }

    json filtered = json::array();
    for (const auto& m : all_mems) {
        if (filtered.size() >= limit) break;
        auto mid = std::to_string(m.value("id", uint64_t(0)));
        auto ms_params = json{{"id", mid}};
        auto ms = tool_memory_status(ms_params);
        auto status = ms.structured.value("status", "active");
        if (status_filter == "all" || status == status_filter) {
            json entry = {
                {"id", mid},
                {"status", status},
                {"kind", m.value("kind", "")},
                {"text", m.value("content", "").substr(0, 120)},
                {"confidence", m.value("confidence", 0.0f)},
            };
            filtered.push_back(entry);
        }
    }
    std::ostringstream ss;
    ss << filtered.size() << " memories with status=" << status_filter << "\n";
    for (const auto& entry : filtered) {
        ss << "  [" << entry.value("status","?") << "] #" << entry.value("id","?")
           << " " << entry.value("text","").substr(0,80) << "\n";
    }
    return ToolResult::ok(ss.str(), {{"memories", filtered}, {"status_filter", status_filter}});
}

} // namespace chitta
