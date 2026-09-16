#!/bin/bash
# The daemon owns transcript progress, inference, SSL parsing and storage.
# The historical staging-file envelope supplies only the registered session ID.
input=${1:-}
if [[ ! -f "$input" ]]; then
    printf '[distill] Error: No input file provided\n' >&2
    exit 0
fi
session='' realm='' line='' count=0
while IFS= read -r line && (( count++ < 5 )); do
    case "$line" in
        SESSION_ID=*) session=${line#SESSION_ID=} ;;
        REALM=*) realm=${line#REALM=} ;;
    esac
done < "$input"
cli=${CHITTA_BIN:-$HOME/.claude/bin/chitta}
reply=$(timeout "${CHITTA_MAX_WAIT:-${CC_SOUL_MAX_WAIT:-180}}" "$cli" distill_now \
    --session_id "$session" --realm "${realm:-brahman}" </dev/null 2>/dev/null) && {
    printf '%s\n' "$reply"
    exit 0
}
printf '[chitta] daemon unavailable; context not loaded.\n'
exit 0
