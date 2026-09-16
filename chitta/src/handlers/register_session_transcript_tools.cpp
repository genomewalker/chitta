// register_session_transcript_tools — chunk extracted from register_tools() so editing
// tool metadata only retemplates one chunk at a time.

#include "../../include/chitta/rpc/field_handler.hpp"

namespace chitta {

void FieldRpcHandler::register_session_transcript_tools() {
    register_tool_table({
        {"ledger_op", "Daemon-owned task ledger operation; list pages capped at 100",
            {{"type","object"},{"properties",{
                {"op",{{"type","string"}}},{"args",{{"type","object"}}}
            }},{"required",json::array({"op"})}},
            &FieldRpcHandler::tool_ledger_op, handlers_["ledger_op"]},

        {"transcript_register", "Register transcript for distillation",
            {{"type","object"},{"properties",{
                {"session_id",{{"type","string"}}},{"transcript_path",{{"type","string"}}},
                {"realm",{{"type","string"}}}
            }},{"required",{"session_id","transcript_path"}}},
            &FieldRpcHandler::tool_transcript_register, handlers_["transcript_register"]},

        {"transcript_get", "Get transcript state",
            {{"type","object"},{"properties",{
                {"session_id",{{"type","string"}}}
            }},{"required",{"session_id"}}},
            &FieldRpcHandler::tool_transcript_get, handlers_["transcript_get"]},

        {"transcript_list", "List registered transcripts",
            {{"type","object"},{"properties",json::object()}},
            &FieldRpcHandler::tool_transcript_list, handlers_["transcript_list"]},

        {"transcript_update", "Update transcript progress",
            {{"type","object"},{"properties",{
                {"session_id",{{"type","string"}}},{"last_line",{{"type","integer"}}}
            }},{"required",{"session_id","last_line"}}},
            &FieldRpcHandler::tool_transcript_update, handlers_["transcript_update"]},

        {"transcript_remove", "Remove transcript from tracking",
            {{"type","object"},{"properties",{
                {"session_id",{{"type","string"}}}
            }},{"required",{"session_id"}}},
            &FieldRpcHandler::tool_transcript_remove, handlers_["transcript_remove"]},

        {"transcript_parse", "Parse new turns from transcript",
            {{"type","object"},{"properties",{
                {"session_id",{{"type","string"}}},{"min_turns",{{"type","integer"}}}
            }},{"required",{"session_id"}}},
            &FieldRpcHandler::tool_transcript_parse, handlers_["transcript_parse"]},

        {"transcript_search", "Semantic search across transcript content",
            {{"type","object"},{"properties",{
                {"query",{{"type","string"}}},{"session_id",{{"type","string"}}},
                {"limit",{{"type","integer"}}},{"min_similarity",{{"type","number"}}},
                {"keyword_only",{{"type","boolean"}}}
            }},{"required",{"query"}}},
            &FieldRpcHandler::tool_transcript_search, handlers_["transcript_search"]},

        {"read_transcript", "Read JSONL transcript with pagination",
            {{"type","object"},{"properties",{
                {"path",{{"type","string"}}},{"session_id",{{"type","string"}}},
                {"start_turn",{{"type","integer"}}},{"limit",{{"type","integer"}}},
                {"max_chars_per_turn",{{"type","integer"}}},{"role_filter",{{"type","string"}}},
                {"keyword",{{"type","string"}}},{"metadata_only",{{"type","boolean"}}}
            }}},
            &FieldRpcHandler::tool_read_transcript, handlers_["read_transcript"]},

        {"get_turns", "Get conversation turns for a session",
            {{"type","object"},{"properties",{
                {"session_id",{{"type","string"}}},{"start_index",{{"type","integer"}}},
                {"limit",{{"type","integer"}}}
            }}},
            &FieldRpcHandler::tool_get_turns, handlers_["get_turns"]},

        {"create_episode", "Create dialogue episode for conversation tracking",
            {{"type","object"},{"properties",{
                {"session_id",{{"type","string"}}},{"title",{{"type","string"}}},
                {"start_turn",{{"type","integer"}}},{"end_turn",{{"type","integer"}}},
                {"episode_type",{{"type","string"}}},{"realm",{{"type","string"}}}
            }},{"required",{"session_id","title","start_turn"}}},
            &FieldRpcHandler::tool_create_episode, handlers_["create_episode"]},

        // Messaging
        {"msg_send", "Send message to another session",
            {{"type","object"},{"properties",{
                {"target",{{"type","string"}}},{"content",{{"type","string"}}},
                {"target_type",{{"type","string"}}},{"priority",{{"type","integer"}}},
                {"content_type",{{"type","string"}}},{"ttl",{{"type","integer"}}},
                {"session_id",{{"type","string"}}}
            }},{"required",{"target","content"}}},
            &FieldRpcHandler::tool_msg_send, handlers_["msg_send"]},

        {"msg_inbox", "Check unread messages",
            {{"type","object"},{"properties",{
                {"session_id",{{"type","string"}}},{"limit",{{"type","integer"}}},
                {"min_priority",{{"type","integer"}}},{"auto_ack",{{"type","boolean"}}}
            }}},
            &FieldRpcHandler::tool_msg_inbox, handlers_["msg_inbox"]},

        {"msg_ack", "Acknowledge a message",
            {{"type","object"},{"properties",{
                {"message_id",{{"type","integer"}}},{"session_id",{{"type","string"}}}
            }},{"required",{"message_id"}}},
            &FieldRpcHandler::tool_msg_ack, handlers_["msg_ack"]},

        {"msg_ack_all", "Acknowledge all messages",
            {{"type","object"},{"properties",{{"session_id",{{"type","string"}}}}}},
            &FieldRpcHandler::tool_msg_ack_all, handlers_["msg_ack_all"]},

        {"msg_respond", "Reply to a message using the original sender/target from the event",
            {{"type","object"},{"properties",{
                {"message_id",{{"type","integer"},{"description","Event ID of the message to reply to"}}},
                {"content",{{"type","string"},{"description","Reply content"}}},
                {"session_id",{{"type","string"},{"description","Override sender session_id (defaults to original target)"}}}
            }},{"required",{"message_id","content"}}},
            &FieldRpcHandler::tool_msg_respond, handlers_["msg_respond"]},

        {"msg_history", "Get message history",
            {{"type","object"},{"properties",{
                {"session_id",{{"type","string"}}},{"limit",{{"type","integer"}}}
            }}},
            &FieldRpcHandler::tool_msg_history, handlers_["msg_history"]},

        {"session_register", "Register session",
            {{"type","object"},{"properties",{
                {"session_id",{{"type","string"}}},{"realm",{{"type","string"}}},
                {"pid",{{"type","integer"}}},{"transcript_path",{{"type","string"}}},
                {"project_dir",{{"type","string"}}},{"metadata",{{"type","string"}}},
                {"client",{{"type","string"}}}
            }}},
            &FieldRpcHandler::tool_session_register, handlers_["session_register"]},

        {"session_heartbeat", "Send heartbeat",
            {{"type","object"},{"properties",{
                {"session_id",{{"type","string"}}},{"metadata",{{"type","string"}}}
            }}},
            &FieldRpcHandler::tool_session_heartbeat, handlers_["session_heartbeat"]},

        {"session_list", "List active sessions",
            {{"type","object"},{"properties",{
                {"realm",{{"type","string"}}},{"status",{{"type","string"}}},
                {"active_only",{{"type","boolean"}}},{"ttl_seconds",{{"type","integer"}}}
            }}},
            &FieldRpcHandler::tool_session_list, handlers_["session_list"]},

        {"session_deregister", "Deregister session",
            {{"type","object"},{"properties",{{"session_id",{{"type","string"}}}}}},
            &FieldRpcHandler::tool_session_deregister, handlers_["session_deregister"]},

        {"session_sync", "Sync session registry",
            {{"type","object"},{"properties",{{"projects_dir",{{"type","string"}}}}}},
            &FieldRpcHandler::tool_session_sync, handlers_["session_sync"]},

        // ── Distill tools ───────────────────────────────────────────────────
        {"repl_session_get", "Get persisted Soul REPL session namespace by ID",
            {{"type","object"},{"properties",{
                {"session_id",{{"type","string"},{"description","Session ID"}}}
            }},{"required",{"session_id"}}},
            &FieldRpcHandler::tool_repl_session_get, handlers_["repl_session_get"]},

        {"repl_session_set", "Persist a Soul REPL session namespace",
            {{"type","object"},{"properties",{
                {"session_id",{{"type","string"},{"description","Session ID"}}},
                {"namespace_json",{{"type","string"},{"description","Serialized namespace as JSON"}}}
            }},{"required",{"session_id","namespace_json"}}},
            &FieldRpcHandler::tool_repl_session_set, handlers_["repl_session_set"]},

        {"repl_session_delete", "Delete a Soul REPL session",
            {{"type","object"},{"properties",{
                {"session_id",{{"type","string"},{"description","Session ID"}}}
            }},{"required",{"session_id"}}},
            &FieldRpcHandler::tool_repl_session_delete, handlers_["repl_session_delete"]},

        {"repl_session_list", "List all active Soul REPL sessions",
            {{"type","object"},{"properties",{}}},
            &FieldRpcHandler::registered_repl_session_list, handlers_["repl_session_list"]},

        {"repl_execute", "Execute Python code in the Soul REPL sandbox. Atomically restores session namespace, runs code, persists updated namespace. Returns output, errors, and soul.* trajectory.",
            {{"type","object"},{"properties",{
                {"session_id",{{"type","string"},{"description","Session ID for namespace persistence"}}},
                {"code",{{"type","string"},{"description","Python code to execute"}}},
                {"reset",{{"type","boolean"},{"description","Clear session state before executing"}}},
                {"max_output",{{"type","integer"},{"description","Maximum output chars (default 10000)"}}}
            }},{"required",{"session_id","code"}}},
            &FieldRpcHandler::tool_repl_execute, handlers_["repl_execute"]},

    });
    classify_tools();
}

ToolResult FieldRpcHandler::registered_repl_session_list(const json&) { return tool_repl_session_list(); }

} // namespace chitta
