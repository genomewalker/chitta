#include "chitta/rpc/field_handler.hpp"
#include "chitta/prompt_fusion.hpp"
#include <chitta/hook_prompt_policy.hpp>
#include <chitta/llm_http.hpp>
#include <filesystem>

namespace chitta {
ToolResult FieldRpcHandler::tool_prompt_context(const json& params) {
    try {
        const auto hook_started = std::chrono::steady_clock::now();
        auto state = params.at("state");
        if (!state.is_object()) return ToolResult::error("state must be an object");
        const bool hook = state.contains("input");
        auto envelope = state;
        json plan;
        if (hook) {
            plan = hook_policy::prompt_prepare(envelope);
            if (plan.value("skip", false)) return ToolResult::ok("", {{"hook",plan}});
            state = plan.at("policy_state");
        }
        json retrieval;
        if (state.contains("retrieval")) {
            auto request = state.at("retrieval");
            const auto started = std::chrono::steady_clock::now();
            auto elapsed = [&] {
                return std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - started).count();
            };
            // Match recall_lanes dispatcher preparation exactly. The query cache
            // shares this vector with smart_recall when its query is identical;
            // context/temporal strings keep their distinct, existing identities.
            std::string q = request.value("query", "");
            if (q.empty()) return ToolResult::error("retrieval.query is required");
            if (field_store_ && !request.contains("_twindow")) {
                int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count();
                time_t tt = time(nullptr);
                struct tm loc {};
                localtime_r(&tt, &loc);
                int32_t tz_min = static_cast<int32_t>((timegm(&loc) - tt) / 60);
                std::string w = field_store_->parse_time_window(q, now, tz_min);
                if (!w.empty()) {
                    try {
                        auto wj = json::parse(w);
                        request["_twindow"] = {wj.value("from_ms", int64_t(0)), wj.value("to_ms", int64_t(0))};
                        std::string stripped = wj.value("stripped", "");
                        if (!stripped.empty()) q = stripped;
                    } catch (...) {}
                }
            }
            auto embedding = embed_query(q);
            if (embedding.empty() && embed_queue_) embed_queue_->enqueue_write(q);
            if (!embedding.empty()) request["_preembedding"] = embedding;
            const auto embedding_ms = elapsed();
            // Leave the hook client margin to receive the reply before its own timeout.
            request["wait_ms"] = std::max<int64_t>(200,
                std::min(state.value("lane_budget_ms", int64_t(2000)),
                         state.value("remaining_ms", int64_t(3000))) - 250);
            auto batch = tool_recall_lanes(request);
            if (batch.is_error) return batch;
            auto lanes = batch.structured.at("lanes");
            const auto options = state.value("fusion_options", json::object());
            retrieval = prompt_policy::fuse(lanes, options);
            auto budget_left = [&] { return elapsed() < state.value("remaining_ms", int64_t(3000)); };
            // Keep the old missing-hybrid-header and empty scoped recall retries.
            if (retrieval["c2_pct"] == "" && lanes.contains("hyb") && budget_left()) {
                auto retry = request;
                retry["limit"] = 1;
                retry["strategy"] = "hybrid";
                retrieval["c2_pct"] = prompt_policy::c2_from_text(tool_recall(retry).text);
            }
            if (retrieval["memories"] == "" && request.value("realm", "") != "brahman"
                && options.value("cross_realm", true) && budget_left()) {
                const auto retry_started = elapsed();
                // A standalone unscoped recall has the same prepared query vector.
                auto retry = request;
                retry.erase("realm");
                retry["limit"] = 5;
                retry["strategy"] = "hybrid";
                auto result = tool_recall(retry);
                const auto ms = elapsed() - retry_started;
                lanes["xr"] = {{"text", result.text}, {"ms", ms},
                    {"timed_out", ms > state.value("lane_budget_ms", 2000)},
                    {"results", result.structured.value("results", json::array())}};
                if (result.text.find("No memories") == std::string::npos)
                    retrieval["memories"] = prompt_policy::lane_rows(result.text, "xr");
            }
            json ms = json::object(), timeouts = json::object();
            for (auto& [name, lane] : lanes.items()) {
                const auto text = lane.value("text", "");
                const bool empty = lane.at("results").empty()
                    && (name != "corrk" || text.rfind("CORRECTION FIRED", 0) != 0);
                // An unavailable primary embedding is explicit, even if keyword
                // results survived. Code/keyword routes and exact-key probes do
                // not depend on that vector. No recall score is changed here.
                const bool smart = name == "sem" || name == "ctx";
                const bool degraded = smart
                    ? prompt_policy::matches(text, "^Smart recall \\((semantic|artifact|hybrid|full), ep=0\\)")
                    : embedding.empty() && name != "kw" && name != "corrk";
                lane["status"] = lane.value("timed_out", false) ? "timeout"
                    : degraded ? "degraded" : empty ? "empty" : "complete";
                ms[name] = state.value("pin_timings", false) ? 0 : lane.at("ms").get<int64_t>();
                timeouts[name] = lane.at("timed_out");
            }
            state.update(retrieval);
            state["lane_ms"] = ms;
            state["lane_timeout"] = timeouts;
            retrieval["lanes"] = lanes;
            retrieval["embedding_ms"] = embedding_ms;
            retrieval["retrieval_ms"] = elapsed();
        }
        const auto admission_started = std::chrono::steady_clock::now();
        auto result = prompt_policy::admit(state);
        if (!retrieval.is_null()) {
            result["retrieval"] = retrieval;
            result["admission_ms"] = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - admission_started).count();
        }
        if (hook) {
            auto invoke = [&](const std::string& tool, const json& args) {
                if (tool == "$hook_classifier") {
                    const auto model = mind_path_ + "/hook-classifier.bin";
                    std::string intent;
                    if (std::filesystem::is_regular_file(model)) {
                        const std::string script = "import fasttext,sys; m=fasttext.load_model(sys.argv[1]); t=sys.argv[2].strip().replace(chr(10),' '); l,s=m.predict(t,k=1); print(l[0].replace('__label__','') if s[0]>=0.55 and l[0]!='__label__neutral' else '')";
                        intent = hook_ledger::trim(fork_exec_capture({"python3", "-c", script, model, args.at("query").get<std::string>()}, 1));
                    }
                    return json{{"text",""},{"structured",{{"intent",intent}}}};
                }
                const auto handler = handlers_.find(tool);
                if (handler == handlers_.end()) throw std::runtime_error("missing hook dependency: " + tool);
                const auto reply = handler->second(args);
                if (reply.is_error) throw std::runtime_error(reply.text);
                return json{{"text",reply.text},{"structured",reply.structured}};
            };
            result["lane_ms"] = state.at("lane_ms");
            result["lane_timeout"] = state.at("lane_timeout");
            envelope["hook_ms"] = envelope.value("pin_timings", false) ? 0 :
                std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - hook_started).count();
            result["hook"] = hook_policy::prompt_finish(envelope, plan, result, invoke);
        }
        return ToolResult::ok(result.at("fused_block").get<std::string>(), result);
    } catch (const std::exception& error) {
        return ToolResult::error(error.what());
    }
}
} // namespace chitta
