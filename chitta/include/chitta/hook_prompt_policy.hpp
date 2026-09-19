#pragma once
#include <chitta/hook_compact_policy.hpp>
#include <chitta/hook_saddle_policy.hpp>
#include <chitta/prompt_fusion.hpp>

namespace chitta::hook_policy {
inline std::string one_line(std::string value) {
    std::replace(value.begin(), value.end(), '\n', ' ');
    return value;
}
inline int number(const json& object, const std::string& key, int fallback = 0) {
    try {
        const auto& value = object.at(key);
        return value.is_string() ? std::stoi(value.get<std::string>()) : value.get<int>();
    } catch (...) {
        return fallback;
    }
}
inline std::string token_text(const std::set<std::string>& tokens,
                              const std::string& separator = "\n") {
    return joined({tokens.begin(), tokens.end()}, separator);
}
inline json prompt_prepare(const json& a) {
    const auto input = a.value("input", json::object()), local = a.value("local", json::object()),
               env = a.value("settings", json::object());
    const auto sid = str(input, "session_id", "unknown"), raw = str(input, "prompt"),
               query = str(a, "clean_query"), previous = str(local, ".last_user_message"),
               realm = str(a, "realm", "brahman");
    auto p           = plan();
    p["state"]       = json::object();
    p["query"]       = query;
    if (query.empty()) {
        p["skip"] = true;
        return p;
    }
    auto opt = [&](const char* key, const char* fallback) { return str(env, key, fallback); };
    const auto tokens = prompt_policy::tokens(query);
    std::set<std::string> distinct;
    const std::string generic =
        " perfect missing else please thanks thank great done ready working broken wrong correct "
        "fine okay again still also need want check status update issue problem think know maybe "
        "sure whats what's hello there anything everything something nothing already yet already "
        "once twice around later earlier today tomorrow yesterday tonight morning evening about "
        "after again also always another anything around because been before being better between "
        "both build called change changed changes check code come could current data does doing "
        "done each either else enough error even every everything file files find first fix from "
        "give going good great have help here high into issue just keep know last later less like "
        "line lines list little long look made make makes making manage many maybe mean message "
        "messages might more most much must need needs never next nothing only other others over "
        "please point problem project query question really right same seems short should show "
        "simple since small some something start still stuff sure take than that their them then "
        "there these they thing things think this those three through time today tried true type "
        "under unrelated until update used user uses using very want wants well were what when "
        "where whether which while will with within without work working works would write wrong "
        "your ";
    for (const auto& token : tokens)
        if (generic.find(" " + token + " ") == std::string::npos &&
            (token.size() >= 5 || match(token, "[0-9_/>-]")))
            distinct.insert(token);
    std::istringstream query_words(query);
    const auto word_count = std::distance(std::istream_iterator<std::string>(query_words),
                                          std::istream_iterator<std::string>());
    auto retrieval_query  = query;
    if (!previous.empty() && previous != query &&
        (word_count < 15 ||
         match(query, R"(\b(that|this|it|those|them|they|he|she|the above|do it|fix it|add it)\b)",
               true)))
        retrieval_query = previous + " " + query;
    std::string context;
    if (opt("CTX_LANE", "1") == "1" && sid != "unknown") {
        auto ring          = lines(str(local, (".ctx_window_" + sid).c_str()));
        const auto current = token_text(tokens, " ") + (tokens.empty() ? "" : " ");
        const auto prior   = ring.empty() ? "" : ring.back(),
                   older   = ring.size() < 2 ? "" : ring[ring.size() - 2];
        context = current + " " + current + " " + current + " " + prior + " " + prior + " " + older;
        p["state"][".ctx_window_" + sid] =
            (prior.empty() ? "" : prior + "\n") + (current.empty() ? "" : current + "\n");
    }
    p["state"][".last_user_message"] = query + "\n";
    auto ablated                     = "," + opt("ABLATE_LANES", "") + ",";
    if (distinct.size() < size_t(std::max(0, number(env, "MIN_QUERY_TOKENS", 1))))
        ablated += "sem,hyb,kw,ctx,xr,";
    json lanes = json::array();
    for (const auto* lane : {"sem", "hyb", "kw", "corr"})
        if (ablated.find("," + std::string(lane) + ",") == std::string::npos) lanes.push_back(lane);
    lanes.push_back("corrk");
    if (!context.empty() && ablated.find(",ctx,") == std::string::npos) lanes.push_back("ctx");
    json state           = {{"query", query},
                            {"query_tokens", token_text(tokens)},
                            {"distinct_tokens", token_text(distinct)},
                            {"context_tokens", token_text(prompt_policy::words(context))},
                            {"seen_hashes", str(local, (".injected_hashes_" + sid).c_str())},
                            {"session_id", sid},
                            {"c2_pct", ""},
                            {"small_realm", false},
                            {"ablated", opt("ABLATE_LANES", "")},
                            {"unknown_silence", opt("UNKNOWN_SILENCE", "1") == "1"},
                            {"anchor_enforce", opt("ANCHOR_ENFORCE", "0") == "1"},
                            {"debug", !opt("ADMIT_DEBUG", "").empty()},
                            {"kw_single_token_min", number(env, "KW_SINGLE_TOKEN_MIN", 60)},
                            {"lane_ms", json::object()},
                            {"lane_timeout", json::object()},
                            {"retrieval",
                             {{"query", retrieval_query},
                              {"ctx_query", context},
                              {"realm", realm},
                              {"lanes", lanes},
                              {"limits", {{"sem", 6}, {"ctx", 4}, {"hyb", 5}, {"kw", 3}, {"corr", 3}}}}},
                            {"fusion_options",
                             {{"hyb_limit", 5},
                              {"small_realm_enabled", opt("C2_SMALL_REALM", "1") == "1"},
                              {"small_realm_maxn", number(env, "C2_SMALL_REALM_MAXN", 3)},
                              {"small_realm_minpct", number(env, "C2_SMALL_REALM_MINPCT", 50)},
                              {"cross_realm", ablated.find(",xr,") == std::string::npos}}},
                            {"remaining_ms", a.value("remaining_ms", 6000)},
                            {"lane_budget_ms", a.value("lane_budget_ms", 2000)},
                            {"pin_timings", a.value("pin_timings", false)}};
    p["policy_state"]    = state;
    p["retrieval_query"] = retrieval_query;
    return p;
}
inline json prompt_finish(const json& a, json p, const json& admitted, const Invoke& invoke) {
    if (p.value("skip", false)) return p;
    const auto input = a.value("input", json::object()), local = a.value("local", json::object()),
               env = a.value("settings", json::object());
    const auto sid = str(input, "session_id", "unknown"), realm = str(a, "realm", "brahman"),
               query = str(p, "query"), retrieval_query = str(p, "retrieval_query");
    auto opt = [&](const char* key, const char* fallback) { return str(env, key, fallback); };
    const auto turn    = a.value("turn", 0);
    const auto now     = a.value("now", 0LL);
    const bool persist = sid != "unknown" && !sid.empty();
    if (persist && a.value("heartbeat_age", 999999LL) >= 120) {
        queue(p, "session_heartbeat",
              {{"session_id", sid},
               {"metadata",
                {{"thread_id", str(input, "thread_id")}, {"client", str(input, "client")}}}});
        p["heartbeat"] = true;
    }
    queue(p, "store_turn",
          {{"session_id", sid},
           {"role", "user"},
           {"content", str(input, "prompt") + "\n"},
           {"turn_index", turn}});
    if (persist)
        queue(p, "ledger_append",
              {{"kind", "Retrieve"},
               {"session_id", sid},
               {"payload",
                {{"Retrieve",
                  {{"query", retrieval_query},
                   {"strategy", "prompt_hook"},
                   {"limit", 0},
                   {"refs", json::array()}}}}}});
    const auto interval = std::max(1, number(env, "CHECKPOINT_INTERVAL", 10));
    if (turn > 0 && turn % interval == 0) {
        queue(p, "ledger_save",
              {{"session_id", sid},
               {"project", realm},
               {"transcript_path", str(input, "transcript_path")},
               {"mood", "working"},
               {"snapshot", "Turn " + std::to_string(turn) + " checkpoint"}});
        queue(p, "distill_trigger", {{"session_id", sid}});
        p["thinking"] = true;
        p["stderr"]   = "[ledger] checkpoint at turn " + std::to_string(turn) +
                      "\n[distill] queued incremental distillation at turn " +
                      std::to_string(turn) + "\n";
    }
    queue(p, "log_event",
          {{"tool", "user_prompt"}, {"entity", realm}, {"outcome", 0}, {"ts_ms", now * 1000}});
    p["profile"]         = admitted;
    p["stderr"]          = str(p, "stderr") + str(admitted, "debug_text");
    const auto retrieval = admitted.at("retrieval");
    const auto memories  = str(retrieval, "memories");
    std::string cache, sessionwarn;
    if (persist && local.contains(".last_stop_time_" + sid)) {
        const auto gap = now - number(local, ".last_stop_time_" + sid);
        const auto ttl = number(env, "CACHE_TTL_MIN", 60);
        if (gap > ttl * 60)
            cache = "[cache-expired: " + std::to_string(gap / 60) + "m idle (> " +
                    std::to_string(ttl) +
                    "m TTL) — full context re-prices at cache-write rates; run /compact or start "
                    "new session with /recap]";
    }
    const auto mb = a.value("transcript_size", 0LL) / 1048576;
    if (mb > 50)
        sessionwarn = "[context-bloat: " + std::to_string(mb) +
                      "MB transcript — consider /compact or start fresh with /recap to reduce "
                      "cache-write costs]";
    else if (mb > 20 && !local.contains(".size_warned_" + sid)) {
        sessionwarn = "[context-growing: " + std::to_string(mb) +
                      "MB — /compact saves cache-write tokens; /recap starts lean]";
        p["state"][".size_warned_" + sid] = "";
    }
    if (memories.empty() && cache.empty() && sessionwarn.empty()) {
        p["outcome"] = {{"event", "recall_empty"},
                        {"lane_ms", admitted.at("lane_ms")},
                        {"lane_timeout", admitted.at("lane_timeout")},
                        {"hook_ms", 0}};
        return p;
    }
    const auto count   = admitted.at("count").get<int>();
    std::string output = str(admitted, "fused_block");
    if (persist && count > 0) {
        auto hashes = str(local, (".injected_hashes_" + sid).c_str());
        if (!hashes.empty()) hashes += '\n';
        for (const auto& hash : admitted.at("hashes"))
            hashes += hash.get<std::string>() + "\n";
        p["state"][".injected_hashes_" + sid] = hashes;
    }
    p["anchor_shadow"] = admitted.at("shadow");
    const auto lanes   = retrieval.at("lanes");
    auto corr          = admitted.value("terse_negation", false)
                             ? ""
                             : str(lanes.value("corr", json::object()), "text"),
         corrk         = str(lanes.value("corrk", json::object()), "text");
    json ids = json::array(), ranks = json::array(), scores = json::array();
    const int min_conf =
        prompt_policy::words(p.at("policy_state").value("distinct_tokens", "")).size() == 1 ? 70
                                                                                            : 0;
    for (const auto& line : lines(memories)) {
        auto id  = found(line, R"(^(\[[a-z]*\])?#([0-9]+))", false, 2),
             pct = found(line, R"(\[([0-9]+)%\])", false, 1);
        if (id.empty() || pct.empty() || std::stoi(pct) < min_conf) continue;
        ids.push_back(std::stoull(id));
        ranks.push_back(ids.size());
        scores.push_back(std::stod(pct) / 100);
        if (ids.size() == 3) break;
    }
    if (!ids.empty())
        queue(p, "log_exposure",
              {{"session_id", sid},
               {"turn_id", turn},
               {"hook_type", "user_prompt"},
               {"memory_ids", ids},
               {"ranks", ranks},
               {"resonance_scores", scores}});
    json correction_ids = json::array();
    std::string latest;
    for (const auto& line : lines(memories)) {
        auto id = found(line, R"(^#([0-9]+))", false, 1);
        if (!id.empty()) {
            latest = id;
            continue;
        }
        if (!latest.empty() && match(line, R"(^\s+\[correction\])")) {
            correction_ids.push_back(std::stoull(latest));
            latest.clear();
        }
    }
    auto keyed = found(corrk, R"(FIRED \(#([0-9]+))", false, 1);
    if (!keyed.empty()) correction_ids.push_back(std::stoull(keyed));
    if (!correction_ids.empty()) p["state"][".exposed_corrections_" + sid] = correction_ids.dump();
    std::string hints;
    const bool skip = match(
        query,
        R"(\[LEARN\]|\[DISCIPLINE\]|UserPromptSubmit says|learn_correction NOW|context-bloat)");
    if (!skip) {
        const std::vector<std::pair<std::string, std::string>> patterns = {
            {"correction",
             "(that'?s (wrong|incorrect|not right|not what)|you('re| are) "
             "(wrong|incorrect|mistaken|off)|use your memory|check.*your memory|did you forget|you "
             "forgot\\b|you missed\\b|that breaks\\b|wrong order\\b|not like that\\b|not this "
             "way\\b|before.*not after\\b|I (said|meant) .{0,30}not\\b|^no[,. "
             "].{0,50}(instead|should|is|use|try|that|the)\\b)"},
            {"preference",
             "(I (prefer|like|always|never|don'?t like)|please (don'?t|always|never)|stop "
             "doing|keep doing|from now on|in the future|more concise|always use\\b|never "
             "use\\b|don'?t use\\b|use .* instead\\b|prefer .* over\\b|no inline\\b|no "
             "comments\\b|no stubs\\b|no placeholders\\b|^(always|never) [a-z])"},
            {"belief",
             "(I always|we always|I never|we never|our convention|our standard|we typically|we "
             "usually|by convention|in this (project|codebase|repo)|the standard (approach|way)|we "
             "(always|never) use|our (approach|workflow|setup) is)"},
            {"milestone", "(it "
                          "works|finally|success|done|shipped|released|completed|finished|passed|"
                          "merged|deployed)"}};
        std::string intent;
        if (a.value("classifier_present", false) &&
            std::any_of(patterns.begin(), patterns.end(),
                        [&](const auto& item) { return match(query, item.second.c_str(), true); }))
            intent = str(invoke("$hook_classifier", {{"query", query}}).at("structured"), "intent");
        for (const auto& item : patterns)
            if (intent == item.first && match(query, item.second.c_str(), true)) {
                const auto context =
                    one_line(bytes(query + "\n", item.first == "milestone" ? 300 : 200));
                if (item.first == "correction") {
                    hints = "[LEARN] ⚠️ CORRECTION detected - call learn_correction NOW\n  User "
                            "said: \"" +
                            context + "\"";
                    p["state"][".last_correction_context"] = query + "\n";
                }
                if (item.first == "preference")
                    hints += (hints.empty() ? "" : "; ") +
                             std::string("[LEARN] Preference detected → use learn_preference tool");
                json args = {{"content", "[" + item.first + "] " + context},
                             {"category", item.first},
                             {"realm", realm},
                             {"tags", {item.first}}};
                if (item.first != "milestone") {
                    args["tags"].push_back("auto");
                    args["visibility"] = 2;
                }
                queue(p, "observe", args);
            }
        if (match(
                query,
                "(frustrated|annoyed|confused|stuck|lost|this is (hard|difficult|confusing)|I give "
                "up|help me understand|what am I missing|tedious|repetitive|not sure|overthinking)",
                true))
            hints +=
                (hints.empty() ? "" : "; ") +
                std::string("[LEARN] User state detected → use learn_approach if something helps");
    }
    auto last_store = number(local, ".last_store_turn_" + sid, turn);
    if (!local.contains(".last_store_turn_" + sid))
        p["state"][".last_store_turn_" + sid] = std::to_string(turn) + "\n";
    if (local.contains(".last_auto_store_ts") && now - number(local, ".last_auto_store_ts") < 120) {
        last_store                            = turn;
        p["state"][".last_store_turn_" + sid] = std::to_string(turn) + "\n";
    }
    const auto since          = turn - last_store,
               store_interval = std::max(1, number(env, "STORE_INTERVAL", 7));
    if (since >= store_interval && turn > 0) {
        if (since >= store_interval * 3 && opt("DISCIPLINE_ENFORCE", "0") == "1") {
            p["stdout"] =
                json{{"decision", "block"},
                     {"reason", "[DISCIPLINE] " + std::to_string(since) +
                                    " turns without a soul store. Call "
                                    "remember/learn_correction/learn_milestone before continuing — "
                                    "memories are the persistent layer that survives compaction."}}
                    .dump() +
                "\n";
            return p;
        }
        hints += (hints.empty() ? "" : "; ") + std::string("[DISCIPLINE] ") +
                 std::to_string(since) +
                 " turns without storing — consider remember/learn_correction/learn_milestone";
    }
    queue(p, "narrative_log",
          {{"session_id", sid},
           {"kind", "user_message"},
           {"summary", one_line(bytes(retrieval_query + "\n", 200))}});
    const auto enrich_interval = std::max(1, number(env, "ENRICH_INTERVAL", 5));
    bool enrich                = turn <= 1 || turn % enrich_interval == 0;
    std::string narrative, goals, curiosity, habits, anticipations, messages = str(a, "notify");
    auto read = [&](const char* tool, const json& args) {
        return invoke(tool, args).at("structured");
    };
    auto meta = [&](const char* tool, const json& args) {
        return read(tool, args).value("metadata", json::object());
    };
    auto percent = [](double value) {
        std::ostringstream out;
        out << std::fixed << std::setprecision(0) << value * 100;
        return out.str();
    };
    p["state"][".last_predictions.json"] = nullptr;
    if (enrich) {
        const auto status = meta("narrative_status", {{"session_id", sid}});
        auto mode         = str(status, "mode", "unknown");
        if (mode != "unknown")
            narrative = "[narrative:" + mode + ":" + percent(status.value("confidence", 0.)) + "%]";
        const auto candidates = meta("anticipation_filter", {{"session_id", sid}, {"max", 3}})
                                    .value("candidates", json::array());
        if (!candidates.empty()) p["state"][".last_predictions.json"] = candidates.dump(2) + "\n";
        for (const auto& row : candidates)
            if (!str(row, "prediction").empty())
                anticipations += "[anticipate:" + str(row, "source", "rule") + ":" +
                                 percent(row.value("confidence", 0.)) + "%] " +
                                 prompt_policy::prefix(str(row, "prediction"), 100) + "\n";
        // The daemon owns both prediction sources; an empty result selects its older pattern model, not a shell compatibility implementation.
        if (anticipations.empty())
            for (const auto& row :
                 meta("anticipation_predict",
                      {{"context", one_line(retrieval_query + "\n")}, {"limit", 3}})
                     .value("patterns", json::array()))
                if ((row.value("frequency", 0) > 2 || row.value("success_count", 0) > 0) &&
                    !str(row, "action").empty())
                    anticipations +=
                        "[anticipate] " + prompt_policy::prefix(str(row, "action"), 100) + "\n";
        for (const auto& row :
             meta("habit_match",
                  {{"context", one_line(bytes(retrieval_query + "\n", 100))}, {"min_strength", .7}})
                 .value("habits", json::array()))
            if (!str(row, "response").empty())
                habits += "[habit:" + percent(row.value("strength", 0.)) + "%] " +
                          prompt_policy::prefix(str(row, "response"), 100) + "\n";
        for (const auto& row :
             meta("goal_list", {{"status", "active"}, {"limit", 3}}).value("goals", json::array()))
            if (!str(row, "title").empty())
                goals += "[goal:" +
                         (row.contains("id") ? (row["id"].is_string() ? row["id"].get<std::string>()
                                                                      : row["id"].dump())
                                             : "") +
                         "] " + prompt_policy::prefix(str(row, "title"), 80) + " (" +
                         percent(row.value("progress", 0.)) + "%)\n";
    }
    if (!local.contains(".gaps_surfaced")) {
        p["local"][".gaps_surfaced"] = "";
        for (const auto& row : meta("curiosity_gaps", {{"limit", 1}}).value("gaps", json::array()))
            if (!str(row, "content").empty()) {
                auto text = str(row, "content");
                if (text.rfind("[gap] ", 0) == 0) text.erase(0, 6);
                curiosity = "[curiosity] Unresolved: " + one_line(bytes(text + "\n", 150));
                break;
            }
    }
    if (!local.contains(".session_active")) {
        p["local"][".session_active"] = "";
        auto recent = str(invoke("recall", {{"query", "session_summary"}, {"limit", 1}}), "text");
        auto line   = first_line(recent, R"(^#[0-9]+ \[[0-9]+%\] \[[^\]]+\]\s+\S)", 150);
        if (!line.empty() && recent.find("No memories") == std::string::npos)
            anticipations += "[last-session] " + line + "\n";
    }
    if (!sid.empty() && sid != "default") {
        queue(p, "session_register",
              {{"session_id", sid}, {"realm", realm}, {"pid", a.value("pid", 0)}});
        auto inbox = read("msg_inbox", {{"session_id", sid}, {"limit", 3}, {"min_priority", 1}});
        if (inbox.value("count", 0) > 0) {
            std::vector<std::string> rows;
            for (const auto& row : inbox.value("messages", json::array())) {
                auto sender = str(row, "sender_session_id", "unknown");
                if (sender.empty()) sender = "unknown";
                if (!str(row, "sender_realm").empty()) sender += "@" + str(row, "sender_realm");
                if (!str(row, "sender_host").empty()) sender += "/" + str(row, "sender_host");
                const auto id     = row.value("memory_id", json());
                const auto idtext = id.is_string() ? id.get<std::string>() : id.dump();
                rows.push_back((row.value("score", 0.) >= 3   ? "[MSG:URGENT:"
                                : row.value("score", 0.) >= 2 ? "[MSG:important:"
                                                              : "[msg:") +
                               sender + "|id:" + idtext + "] " + str(row, "content"));
                queue(p, "msg_ack", {{"message_id", id}});
            }
            messages = joined(rows, "\n");
        }
    }
    std::string final;
    const int maximum = number(env, "MAX_OUTPUT_CHARS", cache.empty() ? 500 : 2000);
    auto append       = [&](std::string text) {
        const auto used     = hook_saddle::decode(final).size();
        const int remaining = maximum - int(used);
        if (text.empty() || remaining <= 20) return;
        final += prompt_policy::prefix(text, remaining);
    };
    append(hints);
    if (!hints.empty()) append("\n");
    if (!messages.empty()) {
        append("[cross-session messages]\n");
        append(messages + "\n");
        append("[/cross-session messages]\n");
    }
    if (count > 0 && !memories.empty()) {
        p["state"][".exposed_memories_" + sid] = memories + "\n";
        p["hint_metric"]                       = {{"backend", "recall"},
                                                  {"outcome", "recall"},
                                                  {"mem_count", count},
                                                  {"hint_surfaced", 0}};
        int n                                  = 0;
        for (const auto& line : lines(memories))
            if (line.find("[hint:realtime]") != std::string::npos) ++n;
        p["hint_metric"]["hint_surfaced"] = n;
    }
    auto admit = str(admitted, "admit_line");
    if (!admit.empty())
        admit += std::regex_replace(str(admitted, "admit_tail"), std::regex("@HOOK_MS@"),
                                    std::to_string(a.value("hook_ms", 0LL)));
    bool emitted = false;
    if (!output.empty() && count > 0) {
        append("[soul]\n");
        if (!admit.empty()) {
            const auto room = std::max(0, maximum - int(hook_saddle::decode(final).size()) -
                                              int(hook_saddle::decode(admit).size()) - 2);
            output          = prompt_policy::prefix(output, room);
        }
        append(output);
        if (!admit.empty()) append("\n" + admit + "\n");
        emitted = true;
    } else if (str(admitted, "c2_tag") == "UNKNOWN" && !admit.empty())
        append("[soul]\n" + admit + "\n");
    if (!narrative.empty()) append(narrative + "\n");
    append(goals);
    if (!curiosity.empty()) append(curiosity + "\n");
    append(habits);
    append(anticipations);
    auto failure = read("recall_failure_pattern", {{"k", 1}}).value("patterns", json::array());
    std::string cec;
    if (!failure.empty() && failure[0].value("fail_ratio", 0.) > .7 &&
        failure[0].value("fail_count", 0) >= 3 && !str(failure[0], "content").empty())
        cec = "⚠ CEC: " + prompt_policy::prefix(str(failure[0], "content"), 200);
    if (!final.empty() || !cache.empty() || !sessionwarn.empty() ||
        corrk.rfind("CORRECTION FIRED", 0) == 0) {
        std::string system;
        auto add = [&](const std::string& text) {
            if (!text.empty()) system += (system.empty() ? "" : " | ") + text;
        };
        add(cache);
        add(sessionwarn);
        add(cec);
        if (hints.find("CORRECTION") != std::string::npos) add(hints);
        if (since >= store_interval * 2 && hints.find("DISCIPLINE") != std::string::npos)
            add("[DISCIPLINE] " + std::to_string(since) +
                " turns without storing — call remember/learn/milestone NOW");
        // Keyed hits carry the record inline; fuzzy hits must clear the
        // correction lane's OWN relevance, not the hybrid lane's (an unrelated
        // 87% decision used to open the gate for 47% corrections).
        std::string correction;
        if (corrk.rfind("CORRECTION FIRED", 0) == 0) {
            const std::regex re(R"(\[correction\][^|\n]+)");
            std::smatch match;
            if (std::regex_search(corrk, match, re)) correction = prompt_policy::prefix(match[0].str() + " ", 400);
        }
        if (correction.empty() && !corr.empty() && std::atoi(prompt_policy::c2_from_text(corr).c_str()) >= 81)
            correction = prompt_policy::correction_rows(corr);
        if (!correction.empty()) add("CORRECTION: " + correction);
        if (!cache.empty() && count > 0 && !output.empty()) {
            std::vector<std::string> facts;
            for (const auto& line : lines(output))
                if (line.rfind("[thought]", 0) != 0)
                    facts.push_back(std::regex_replace(line, std::regex(R"(^\[(hyb|corr)\])"), ""));
            auto text = joined(take(facts, 4), "\n");
            if (!text.empty())
                system += (system.empty() ? "" : "\n") + std::string("[soul-context]\n") + text +
                          "\n[/soul-context]";
        }
        std::vector<std::string> trimmed;
        for (const auto& line : lines(final))
            trimmed.push_back(std::regex_replace(line, std::regex(R"(\s+$)"), ""));
        final = joined(trimmed, "\n");
        while (!final.empty() && final.back() == '\n')
            final.pop_back();
        nlohmann::ordered_json response = {
            {"hookSpecificOutput",
             {{"hookEventName", "UserPromptSubmit"}, {"additionalContext", final}}}};
        if (!system.empty()) response["systemMessage"] = system;
        p["stdout"] = response.dump(2) + "\n";
    }
    if (emitted) {
        json seen = json::array(), by_lane = json::object();
        std::set<std::string> unique;
        for (const auto& line : lines(str(admitted, "fused_block"))) {
            std::smatch m;
            static const std::regex re(R"(^\[(\w+)\]#(\d+))");
            if (std::regex_search(line, m, re)) {
                by_lane[m[1].str()].push_back(m[2].str());
                unique.insert(m[2].str());
            }
        }
        for (const auto& id : unique)
            seen.push_back(id);
        if (!seen.empty())
            p["outcome"] = {{"event", "injected"},
                            {"ids", seen},
                            {"lanes", by_lane},
                            {"lane_ms", admitted.at("lane_ms")},
                            {"lane_timeout", admitted.at("lane_timeout")},
                            {"hook_ms", 0}};
    }
    p["hint"] = turn >= 6 && turn % 6 == 0;
    return p;
}
} // namespace chitta::hook_policy
