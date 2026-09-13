#!/bin/bash
# Reparse Claude transcripts to backfill conversation_turn table
# Extracts both user and assistant turns from JSONL files

set -e

CHITTA_BIN="${CHITTA_BIN:-$HOME/.claude/bin/chitta}"
MIND_PATH="${MIND_PATH:-${CHITTA_DB_PATH:-$HOME/.claude/mind}}"
# Same derivation as queue_path in lib.sh: the queue lives with the mind.
QUEUE_FILE="${CHITTA_QUEUE:-${CHITTA_QUEUE_PATH:-${MIND_PATH%/}/queue.jsonl}}"
MAX_CONTENT=5000  # Truncate content to avoid huge entries

# Parse a single transcript file
parse_transcript() {
    local file="$1"
    local session_id

    # Extract session ID from filename (UUID format)
    session_id=$(basename "$file" .jsonl)

    # Skip if not a UUID
    if ! [[ "$session_id" =~ ^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$ ]]; then
        return 0
    fi

    echo "  Processing: $session_id" >&2

    local turn_index=0
    local user_count=0
    local assistant_count=0

    # Parse each line - Claude transcripts use "type" field for message type
    while IFS= read -r line; do
        [[ -z "$line" ]] && continue

        local msg_type
        msg_type=$(echo "$line" | jq -r '.type // empty' 2>/dev/null)

        local content=""
        local role=""

        if [[ "$msg_type" == "user" ]]; then
            role="user"
            # User message content is in .message.content (string or array)
            content=$(echo "$line" | jq -r '
                .message.content |
                if type == "string" then .
                elif type == "array" then map(select(.type == "text") | .text) | join("\n")
                else ""
                end
            ' 2>/dev/null | head -c "$MAX_CONTENT")
            ((user_count++))
            ((turn_index++))

        elif [[ "$msg_type" == "assistant" ]]; then
            role="assistant"
            # Assistant message content is in .message.content array
            content=$(echo "$line" | jq -r '
                .message.content |
                if type == "array" then
                    map(select(.type == "text") | .text) | join("\n")
                elif type == "string" then .
                else ""
                end
            ' 2>/dev/null | head -c "$MAX_CONTENT")
            ((assistant_count++))
            ((turn_index++))
        else
            continue
        fi

        # Skip empty content
        [[ -z "$content" || ${#content} -lt 3 ]] && continue

        # Queue the turn for storage
        local json_content
        json_content=$(echo "$content" | jq -Rs .)

        echo "{\"tool\":\"store_turn\",\"args\":{\"session_id\":\"$session_id\",\"role\":\"$role\",\"content\":$json_content,\"turn_index\":$turn_index}}" >> "$QUEUE_FILE"

    done < "$file"

    [[ $user_count -gt 0 || $assistant_count -gt 0 ]] && \
        echo "    $user_count user + $assistant_count assistant turns" >&2
}

# Main
echo "=== Transcript Reparser ===" >&2
echo "Queue file: $QUEUE_FILE" >&2
echo "" >&2

# Clear existing queue to avoid duplicates
: > "$QUEUE_FILE"

# Find all transcript files
total_files=0
total_user=0
total_assistant=0

for dir in "$HOME/.claude/projects"/*; do
    [[ -d "$dir" ]] || continue

    while IFS= read -r file; do
        [[ -f "$file" ]] || continue
        parse_transcript "$file"
        ((total_files++))
    done < <(find "$dir" -maxdepth 2 -name "*.jsonl" -type f 2>/dev/null | grep -E '[0-9a-f]{8}-[0-9a-f]{4}' | head -100)
done

echo "" >&2
echo "Processed $total_files transcript files" >&2
queue_entries=$(wc -l < "$QUEUE_FILE" 2>/dev/null || echo 0)
echo "Queue entries: $queue_entries" >&2
echo "" >&2
echo "Daemon will process the queue automatically." >&2
