// register_session_transcript_tools — chunk extracted from register_tools() so editing
// tool metadata only retemplates one chunk at a time.

#include "../../include/chitta/rpc/field_handler.hpp"

namespace chitta {

void FieldRpcHandler::register_session_transcript_tools() {
    tools_.push_back({{"name","ledger_op"},{"description","Daemon-owned task ledger operation; list pages capped at 100"},
        {"inputSchema",{{"type","object"},{"properties",{
            {"op",{{"type","string"}}},{"args",{{"type","object"}}}
        }},{"required",json::array({"op"})}}}});
    handlers_["ledger_op"] = [this](const json& p) { return tool_ledger_op(p); };

    tools_.push_back({{"name","transcript_register"},{"description","Register transcript for distillation"},
        {"inputSchema",{{"type","object"},{"properties",{
            {"session_id",{{"type","string"}}},{"transcript_path",{{"type","string"}}},
            {"realm",{{"type","string"}}}
        }},{"required",{"session_id","transcript_path"}}}}
    });
    handlers_["transcript_register"] = [this](const json& p) { return tool_transcript_register(p); };

    tools_.push_back({{"name","transcript_get"},{"description","Get transcript state"},
        {"inputSchema",{{"type","object"},{"properties",{
            {"session_id",{{"type","string"}}}
        }},{"required",{"session_id"}}}}
    });
    handlers_["transcript_get"] = [this](const json& p) { return tool_transcript_get(p); };

    tools_.push_back({{"name","transcript_list"},{"description","List registered transcripts"},
        {"inputSchema",{{"type","object"},{"properties",json::object()}}}
    });
    handlers_["transcript_list"] = [this](const json& p) { return tool_transcript_list(p); };

    tools_.push_back({{"name","transcript_update"},{"description","Update transcript progress"},
        {"inputSchema",{{"type","object"},{"properties",{
            {"session_id",{{"type","string"}}},{"last_line",{{"type","integer"}}}
        }},{"required",{"session_id","last_line"}}}}
    });
    handlers_["transcript_update"] = [this](const json& p) { return tool_transcript_update(p); };

    tools_.push_back({{"name","transcript_remove"},{"description","Remove transcript from tracking"},
        {"inputSchema",{{"type","object"},{"properties",{
            {"session_id",{{"type","string"}}}
        }},{"required",{"session_id"}}}}
    });
    handlers_["transcript_remove"] = [this](const json& p) { return tool_transcript_remove(p); };

    tools_.push_back({{"name","transcript_parse"},{"description","Parse new turns from transcript"},
        {"inputSchema",{{"type","object"},{"properties",{
            {"session_id",{{"type","string"}}},{"min_turns",{{"type","integer"}}}
        }},{"required",{"session_id"}}}}
    });
    handlers_["transcript_parse"] = [this](const json& p) { return tool_transcript_parse(p); };

    tools_.push_back({{"name","transcript_search"},{"description","Semantic search across transcript content"},
        {"inputSchema",{{"type","object"},{"properties",{
            {"query",{{"type","string"}}},{"session_id",{{"type","string"}}},
            {"limit",{{"type","integer"}}},{"min_similarity",{{"type","number"}}},
            {"keyword_only",{{"type","boolean"}}}
        }},{"required",{"query"}}}}
    });
    handlers_["transcript_search"] = [this](const json& p) { return tool_transcript_search(p); };

    tools_.push_back({{"name","read_transcript"},{"description","Read JSONL transcript with pagination"},
        {"inputSchema",{{"type","object"},{"properties",{
            {"path",{{"type","string"}}},{"session_id",{{"type","string"}}},
            {"start_turn",{{"type","integer"}}},{"limit",{{"type","integer"}}},
            {"max_chars_per_turn",{{"type","integer"}}},{"role_filter",{{"type","string"}}},
            {"keyword",{{"type","string"}}},{"metadata_only",{{"type","boolean"}}}
        }}}}
    });
    handlers_["read_transcript"] = [this](const json& p) { return tool_read_transcript(p); };

    tools_.push_back({{"name","get_turns"},{"description","Get conversation turns for a session"},
        {"inputSchema",{{"type","object"},{"properties",{
            {"session_id",{{"type","string"}}},{"start_index",{{"type","integer"}}},
            {"limit",{{"type","integer"}}}
        }}}}
    });
    handlers_["get_turns"] = [this](const json& p) { return tool_get_turns(p); };

    tools_.push_back({{"name","create_episode"},{"description","Create dialogue episode for conversation tracking"},
        {"inputSchema",{{"type","object"},{"properties",{
            {"session_id",{{"type","string"}}},{"title",{{"type","string"}}},
            {"start_turn",{{"type","integer"}}},{"end_turn",{{"type","integer"}}},
            {"episode_type",{{"type","string"}}},{"realm",{{"type","string"}}}
        }},{"required",{"session_id","title","start_turn"}}}}
    });
    handlers_["create_episode"] = [this](const json& p) { return tool_create_episode(p); };

    // Messaging
    tools_.push_back({{"name","msg_send"},{"description","Send message to another session"},
        {"inputSchema",{{"type","object"},{"properties",{
            {"target",{{"type","string"}}},{"content",{{"type","string"}}},
            {"target_type",{{"type","string"}}},{"priority",{{"type","integer"}}},
            {"content_type",{{"type","string"}}},{"ttl",{{"type","integer"}}},
            {"session_id",{{"type","string"}}}
        }},{"required",{"target","content"}}}}
    });
    handlers_["msg_send"] = [this](const json& p) { return tool_msg_send(p); };

    tools_.push_back({{"name","msg_inbox"},{"description","Check unread messages"},
        {"inputSchema",{{"type","object"},{"properties",{
            {"session_id",{{"type","string"}}},{"limit",{{"type","integer"}}},
            {"min_priority",{{"type","integer"}}},{"auto_ack",{{"type","boolean"}}}
        }}}}
    });
    handlers_["msg_inbox"] = [this](const json& p) { return tool_msg_inbox(p); };

    tools_.push_back({{"name","msg_ack"},{"description","Acknowledge a message"},
        {"inputSchema",{{"type","object"},{"properties",{
            {"message_id",{{"type","integer"}}},{"session_id",{{"type","string"}}}
        }},{"required",{"message_id"}}}}
    });
    handlers_["msg_ack"] = [this](const json& p) { return tool_msg_ack(p); };

    tools_.push_back({{"name","msg_ack_all"},{"description","Acknowledge all messages"},
        {"inputSchema",{{"type","object"},{"properties",{{"session_id",{{"type","string"}}}}}}}
    });
    handlers_["msg_ack_all"] = [this](const json& p) { return tool_msg_ack_all(p); };

    tools_.push_back({{"name","msg_respond"},{"description","Reply to a message using the original sender/target from the event"},
        {"inputSchema",{{"type","object"},{"properties",{
            {"message_id",{{"type","integer"},{"description","Event ID of the message to reply to"}}},
            {"content",{{"type","string"},{"description","Reply content"}}},
            {"session_id",{{"type","string"},{"description","Override sender session_id (defaults to original target)"}}}
        }},{"required",{"message_id","content"}}}}
    });
    handlers_["msg_respond"] = [this](const json& p) { return tool_msg_respond(p); };

    tools_.push_back({{"name","msg_history"},{"description","Get message history"},
        {"inputSchema",{{"type","object"},{"properties",{
            {"session_id",{{"type","string"}}},{"limit",{{"type","integer"}}}
        }}}}
    });
    handlers_["msg_history"] = [this](const json& p) { return tool_msg_history(p); };

    tools_.push_back({{"name","session_register"},{"description","Register session"},
        {"inputSchema",{{"type","object"},{"properties",{
            {"session_id",{{"type","string"}}},{"realm",{{"type","string"}}},
            {"pid",{{"type","integer"}}},{"transcript_path",{{"type","string"}}},
            {"project_dir",{{"type","string"}}},{"metadata",{{"type","string"}}},
            {"client",{{"type","string"}}}
        }}}}
    });
    handlers_["session_register"] = [this](const json& p) { return tool_session_register(p); };

    tools_.push_back({{"name","session_heartbeat"},{"description","Send heartbeat"},
        {"inputSchema",{{"type","object"},{"properties",{
            {"session_id",{{"type","string"}}},{"metadata",{{"type","string"}}}
        }}}}
    });
    handlers_["session_heartbeat"] = [this](const json& p) { return tool_session_heartbeat(p); };

    tools_.push_back({{"name","session_list"},{"description","List active sessions"},
        {"inputSchema",{{"type","object"},{"properties",{
            {"realm",{{"type","string"}}},{"status",{{"type","string"}}},
            {"active_only",{{"type","boolean"}}},{"ttl_seconds",{{"type","integer"}}}
        }}}}
    });
    handlers_["session_list"] = [this](const json& p) { return tool_session_list(p); };

    tools_.push_back({{"name","session_deregister"},{"description","Deregister session"},
        {"inputSchema",{{"type","object"},{"properties",{{"session_id",{{"type","string"}}}}}}}
    });
    handlers_["session_deregister"] = [this](const json& p) { return tool_session_deregister(p); };

    tools_.push_back({{"name","session_sync"},{"description","Sync session registry"},
        {"inputSchema",{{"type","object"},{"properties",{{"projects_dir",{{"type","string"}}}}}}}
    });
    handlers_["session_sync"] = [this](const json& p) { return tool_session_sync(p); };

    // ── Distill tools ───────────────────────────────────────────────────
    tools_.push_back({{"name","repl_session_get"},{"description","Get persisted Soul REPL session namespace by ID"},
        {"inputSchema",{{"type","object"},{"properties",{
            {"session_id",{{"type","string"},{"description","Session ID"}}}
        }},{"required",{"session_id"}}}}
    });
    handlers_["repl_session_get"] = [this](const json& p) { return tool_repl_session_get(p); };

    tools_.push_back({{"name","repl_session_set"},{"description","Persist a Soul REPL session namespace"},
        {"inputSchema",{{"type","object"},{"properties",{
            {"session_id",{{"type","string"},{"description","Session ID"}}},
            {"namespace_json",{{"type","string"},{"description","Serialized namespace as JSON"}}}
        }},{"required",{"session_id","namespace_json"}}}}
    });
    handlers_["repl_session_set"] = [this](const json& p) { return tool_repl_session_set(p); };

    tools_.push_back({{"name","repl_session_delete"},{"description","Delete a Soul REPL session"},
        {"inputSchema",{{"type","object"},{"properties",{
            {"session_id",{{"type","string"},{"description","Session ID"}}}
        }},{"required",{"session_id"}}}}
    });
    handlers_["repl_session_delete"] = [this](const json& p) { return tool_repl_session_delete(p); };

    tools_.push_back({{"name","repl_session_list"},{"description","List all active Soul REPL sessions"},
        {"inputSchema",{{"type","object"},{"properties",{}}}}
    });
    handlers_["repl_session_list"] = [this](const json&) { return tool_repl_session_list(); };

    tools_.push_back({{"name","repl_execute"},{"description","Execute Python code in the Soul REPL sandbox. Atomically restores session namespace, runs code, persists updated namespace. Returns output, errors, and soul.* trajectory."},
        {"inputSchema",{{"type","object"},{"properties",{
            {"session_id",{{"type","string"},{"description","Session ID for namespace persistence"}}},
            {"code",{{"type","string"},{"description","Python code to execute"}}},
            {"reset",{{"type","boolean"},{"description","Clear session state before executing"}}},
            {"max_output",{{"type","integer"},{"description","Maximum output chars (default 10000)"}}}
        }},{"required",{"session_id","code"}}}}
    });
    handlers_["repl_execute"] = [this](const json& p) { return tool_repl_execute(p); };

    classify_tools();
}

} // namespace chitta
