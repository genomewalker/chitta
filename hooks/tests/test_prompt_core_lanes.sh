#!/bin/bash
# Test hooks/prompt-core.sh lane behavior: CHITTA_ABLATE_LANES ablation
# (CC_SOUL_ABLATE_LANES still honored), smart_recall keyword-route
# retagging, and C2 small-realm relaxation.
#
# Drives the real hook end-to-end (not a unit test of one function) against a
# stub $CHITTA_BIN so recall-lane content is deterministic instead of subject
# to live-daemon embedding calibration. daemon_available() only checks that
# the mind-path socket file exists (-S), so a bound-but-unlistened UNIX socket
# satisfies the gate without a real chittad.
set -u
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
FAIL=0
assert() { if ! eval "$2"; then echo "FAIL: $1"; FAIL=1; else echo "ok: $1"; fi; }

T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT

STUB="$T/chitta"
cat > "$STUB" <<'STUBEOF'
#!/bin/bash
# Fake chitta CLI: dispatches on subcommand (+ --limit, for the two
# smart_recall call sites) to canned fixture files set via env vars.
sub="$1"; shift
if [[ -n "${STUB_CALL_LOG:-}" ]]; then
    printf '%s %s\n' "$sub" "$*" >> "$STUB_CALL_LOG"
fi
get() { local flag="$1" a p; shift; for a in "$@"; do [[ "$p" == "$flag" ]] && { echo "$a"; return; }; p="$a"; done; }
case "$sub" in
    prompt_context)
        state=$(get --state "$@")
        if [[ "$state" == *'"retrieval":'* ]]; then
            [[ -n "${STUB_OVERLAP_DIR:-}" ]] && touch "$STUB_OVERLAP_DIR/rpc-started"
            [[ "${STUB_PIPELINE_TIMEOUT:-0}" == 1 ]] && exit 124
        fi
        [[ -n "${STUB_POLICY_BIN:-}" ]] || exit 1
        if [[ "${1:-}" != --local && "${STUB_POLICY_MODE:-}" == timeout ]]; then
            sleep 1
            exit 1
        fi
        printf '%s' "$(get --state "$@")" | "$STUB_POLICY_BIN" --stdin
        ;;
    session_heartbeat)
        [[ "${STUB_HEARTBEAT_FAIL:-0}" == 0 ]] || exit 1
        if [[ -n "${STUB_OVERLAP_DIR:-}" ]]; then
            touch "$STUB_OVERLAP_DIR/heartbeat-started"
            for ((attempt=0; attempt<80; attempt++)); do
                if [[ -f "$STUB_OVERLAP_DIR/rpc-started" ]]; then
                    touch "$STUB_OVERLAP_DIR/heartbeat-overlapped"
                    break
                fi
                sleep 0.01
            done
        fi
        ;;
    queue_write) exit 1 ;; # Exercise the durable file fallback.
    recall_lanes)
        if [[ -n "${STUB_OVERLAP_DIR:-}" ]]; then
            touch "$STUB_OVERLAP_DIR/rpc-started"
            for ((attempt=0; attempt<80; attempt++)); do
                if [[ -f "$STUB_OVERLAP_DIR/session-started" && -f "$STUB_OVERLAP_DIR/heartbeat-started" ]]; then
                    touch "$STUB_OVERLAP_DIR/overlapped"
                    break
                fi
                sleep 0.01
            done
        fi
        [[ "${STUB_RPC_MODE:-ok}" == "fail" ]] && exit 1
        cat "${STUB_RPC_FILE:-/dev/null}"
        ;;
    smart_recall)
        limit=$(get --limit "$@")
        if [[ "$limit" == "6" ]]; then cat "${STUB_SEM_FILE:-/dev/null}"
        elif [[ "$limit" == "4" ]]; then cat "${STUB_CTX_FILE:-/dev/null}"
        fi
        ;;
    recall)
        strategy=$(get --strategy "$@")
        tag=$(get --tag "$@")
        query=$(get --query "$@")
        if [[ "$query" == "session_summary" ]]; then
            if [[ -n "${STUB_OVERLAP_DIR:-}" ]]; then
                touch "$STUB_OVERLAP_DIR/session-started"
                for ((attempt=0; attempt<80; attempt++)); do
                    [[ -f "$STUB_OVERLAP_DIR/rpc-started" ]] && break
                    sleep 0.01
                done
            fi
            cat "${STUB_SESSION_FILE:-/dev/null}"
        elif [[ "$strategy" == "hybrid" ]]; then cat "${STUB_HYB_FILE:-/dev/null}"
        elif [[ "$strategy" == "keyword" ]]; then cat "${STUB_KW_FILE:-/dev/null}"
        elif [[ -n "$tag" ]]; then cat "${STUB_CORR_FILE:-/dev/null}"
        fi
        ;;
    correction_check) cat "${STUB_CORRK_FILE:-/dev/null}" ;;
    *) : ;;  # queue_write, log_event, predicate_list, predicate_run, realm_detect (unused: CHITTA_REALM set)
esac
exit 0
STUBEOF
chmod +x "$STUB"

MIND="$T/mind"
mkdir -p "$MIND"
export CHITTA_BIN="$STUB"
export CHITTA_DB_PATH="$MIND"
export CHITTA_QUEUE="$T/queue.jsonl"
export CHITTA_REALM="project:stubrealm"   # skips realm_detect entirely
export CC_SOUL_ADMIT_DEBUG=1
export CC_SOUL_CTX_LANE=0                 # keep fixtures to sem/hyb/kw/corr; ctx covered by the same code path as sem
unset CHITTA_RECALL_LANES_RPC CC_SOUL_RECALL_LANES_RPC

# Bind (not listen) a UNIX socket at the path daemon_available() checks —
# bind() alone creates the filesystem node, which is all `-S` requires.
source "$SCRIPT_DIR/lib.sh"
SOCK="$(get_socket_path)"
mkdir -p "$(dirname "$SOCK")"
python3 -c "import socket,sys; s=socket.socket(socket.AF_UNIX); s.bind(sys.argv[1])" "$SOCK"
assert "stub daemon socket exists" "[[ -S '$SOCK' ]]"

run_hook() {  # $1 = session_id  $2 = prompt
    printf '{"session_id":"%s","prompt":"%s","cwd":"/tmp"}' "$1" "$2" \
        | bash "$SCRIPT_DIR/prompt-core.sh" 2>"$T/stderr.$1" 1>"$T/stdout.$1"
    rc=$?
    if [[ -n "${HOOK_PARITY_CAPTURE:-}" ]]; then
        cp "$T/stderr.$1" "$HOOK_PARITY_CAPTURE/$1.stderr"
        cp "$T/stdout.$1" "$HOOK_PARITY_CAPTURE/$1.stdout"
        printf '%s\n' "$rc" > "$HOOK_PARITY_CAPTURE/$1.status"
    fi
    return "$rc"
}

# ============================================================
# Item 1: CC_SOUL_ABLATE_LANES skips the listed lanes' recall calls
# and their admit-debug trace, and hyb ablation withholds C2 (no
# UNKNOWN silencing) rather than measuring it via the degrade-retry.
# ============================================================
STUB_SEM_FILE="$T/sem1"; STUB_HYB_FILE="$T/hyb1"; STUB_KW_FILE="$T/kw1"
cat > "$STUB_SEM_FILE" <<'EOF'
Smart recall (semantic, ep=1): 1 results
#1 [90%] [wisdom] apricot fixture line for lane ablation test
EOF
cat > "$STUB_HYB_FILE" <<'EOF'
Found 1 results in realm 'project:stubrealm' (maxrel 90%):
#2 [85%] [wisdom] apricot hybrid fixture line
EOF
cat > "$STUB_KW_FILE" <<'EOF'
#3 [40%] [wisdom] apricot keyword fixture line
EOF
export STUB_SEM_FILE STUB_HYB_FILE STUB_KW_FILE
export CC_SOUL_ABLATE_LANES="hyb,kw"
run_hook "abl-1" "tell me about the apricot fixture"
assert "ablated hyb lane absent from admit-debug" "! grep -q 'lane=hyb' '$T/stderr.abl-1'"
assert "ablated kw lane absent from admit-debug" "! grep -q 'lane=kw' '$T/stderr.abl-1'"
assert "non-ablated sem lane still present" "grep -q 'lane=sem' '$T/stderr.abl-1'"
assert "admit-debug c2 unmeasured (hyb ablated, no degrade-retry substitute)" \
    "grep 'lane=sem' '$T/stderr.abl-1' | grep -q 'c2=none'"
assert "admit line reports abl:hyb,kw" "grep -q 'abl:hyb,kw' '$T/stdout.abl-1'"
assert "admit line carries compact lane and total timings" \
    "grep -Eq 't:sem=[0-9]+.*total=[0-9]+' '$T/stdout.abl-1'"
INJECTED=$(jq -c 'select(.event == "injected" and .session_id == "abl-1")' "$MIND/outcome_ledger.jsonl")
assert "injected ledger event carries lane timing objects and hook total" \
    "printf '%s' '$INJECTED' | jq -e '.lane_ms.sem >= 0 and (.lane_timeout.sem == true or .lane_timeout.sem == false) and .hook_ms >= 0' >/dev/null"
unset CC_SOUL_ABLATE_LANES

# ============================================================
# Item 2: smart_recall routed to keyword (header "Smart recall
# (keyword, ep=N)") gets every result line re-tagged [kw] so the BM25
# floor (conf>=1) applies instead of the sem lane's MIN_CONFIDENCE=30,
# which would otherwise drop a 10% BM25-scale item outright.
# ============================================================
STUB_SEM_FILE="$T/sem2"; STUB_HYB_FILE="$T/hyb2"; STUB_KW_FILE="$T/kw2"
cat > "$STUB_SEM_FILE" <<'EOF'
Smart recall (keyword, ep=9): 1 results
#11 [10%] [wisdom] bluegill token appears only in this fixture
EOF
# Pin C2 to KNOWN so item 2's assertion isn't confounded by the UNKNOWN gate.
cat > "$STUB_HYB_FILE" <<'EOF'
Found 1 results in realm 'project:stubrealm' (maxrel 90%):
#12 [85%] [wisdom] bluegill hybrid fixture line
EOF
: > "$STUB_KW_FILE"
export STUB_SEM_FILE STUB_HYB_FILE STUB_KW_FILE
run_hook "kwroute-1" "what does the bluegill token do"
assert "keyword-routed sem line retagged to [kw] lane" "grep -q 'lane=kw.*#11' '$T/stderr.kwroute-1'"
assert "keyword-routed sem line no longer tagged [sem]" "! grep -q 'lane=sem.*#11' '$T/stderr.kwroute-1'"
assert "retagged item admitted despite 10% (BM25 floor, not MIN_CONFIDENCE=30)" \
    "grep -q '\[kw\]#11' '$T/stdout.kwroute-1'"

# ============================================================
# Item 3: C2 small-realm relaxation. hyb header shows few results in a
# scoped (non-brahman) realm with a confident top hit (maxrel < 81 so
# C2 reads UNKNOWN, but display_pct of the top hyb result >= 50) — sem
# candidates sharing a distinctive token with the query are admitted
# instead of unconditionally dropped. Toggle via CC_SOUL_C2_SMALL_REALM.
# ============================================================
STUB_SEM_FILE="$T/sem3"; STUB_HYB_FILE="$T/hyb3"; STUB_KW_FILE="$T/kw3"
cat > "$STUB_SEM_FILE" <<'EOF'
Smart recall (semantic, ep=5): 1 results
#21 [90%] [wisdom] cranberry topic discussion shares a token with the query
EOF
cat > "$STUB_HYB_FILE" <<'EOF'
Found 1 results in realm 'project:stubrealm' (maxrel 72%):
#22 [75%] [wisdom] cranberry hybrid fixture line
EOF
: > "$STUB_KW_FILE"
export STUB_SEM_FILE STUB_HYB_FILE STUB_KW_FILE

CC_SOUL_C2_SMALL_REALM=1 run_hook "sr-on" "what is cranberry topic about"
assert "small-realm ON: shared-token sem candidate admitted" "grep -q '\[sem\]#21' '$T/stdout.sr-on'"
assert "small-realm ON: admit-debug shows sr=1" "grep -q 'lane=sem.*sr=1' '$T/stderr.sr-on'"
assert "small-realm ON: admit line reports sr:on" "grep -q ' sr:on' '$T/stdout.sr-on'"

CC_SOUL_C2_SMALL_REALM=0 run_hook "sr-off" "what is cranberry topic about"
assert "small-realm OFF: sem candidate dropped (default UNKNOWN silence)" "! grep -q '\[sem\]#21' '$T/stdout.sr-off'"
assert "small-realm OFF: admit-debug shows sr=0" "grep -q 'lane=sem.*sr=0' '$T/stderr.sr-off'"

# Negative control: same confident top hit, but N=8 (not a small realm) —
# relaxation must NOT fire even with the feature on.
STUB_HYB_FILE="$T/hyb3b"
cat > "$STUB_HYB_FILE" <<'EOF'
Found 8 results in realm 'project:stubrealm' (maxrel 72%):
#22 [75%] [wisdom] cranberry hybrid fixture line
EOF
export STUB_HYB_FILE
CC_SOUL_C2_SMALL_REALM=1 run_hook "sr-largen" "what is cranberry topic about"
assert "small-realm ON but N=8: sem candidate still dropped" "! grep -q '\[sem\]#21' '$T/stdout.sr-largen'"
assert "small-realm ON but N=8: admit-debug shows sr=0" "grep -q 'lane=sem.*sr=0' '$T/stderr.sr-largen'"

# ============================================================
# Item 4: the renamed CHITTA_ABLATE_LANES knob ablates the same way as
# CC_SOUL_ABLATE_LANES (rename compat shim in lib.sh) — replays item 1's
# fixtures with the new env var name and neither old one set.
# ============================================================
STUB_SEM_FILE="$T/sem1"; STUB_HYB_FILE="$T/hyb1"; STUB_KW_FILE="$T/kw1"
export STUB_SEM_FILE STUB_HYB_FILE STUB_KW_FILE
unset CC_SOUL_ABLATE_LANES
CHITTA_ABLATE_LANES="hyb,kw" run_hook "abl-chitta" "tell me about the apricot fixture"
assert "CHITTA_ABLATE_LANES: ablated hyb lane absent" "! grep -q 'lane=hyb' '$T/stderr.abl-chitta'"
assert "CHITTA_ABLATE_LANES: ablated kw lane absent" "! grep -q 'lane=kw' '$T/stderr.abl-chitta'"
assert "CHITTA_ABLATE_LANES: non-ablated sem lane present" "grep -q 'lane=sem' '$T/stderr.abl-chitta'"
assert "CHITTA_ABLATE_LANES: admit line reports abl:hyb,kw" "grep -q 'abl:hyb,kw' '$T/stdout.abl-chitta'"

# ============================================================
# Item 5: an all-empty fan-out exits quietly but records which lanes
# returned empty and how long each took. The scoped empty result also
# exercises the timed cross-realm fallback before the final empty event.
# ============================================================
STUB_SEM_FILE="$T/sem-empty"; STUB_HYB_FILE="$T/hyb-empty"
STUB_KW_FILE="$T/kw-empty"; STUB_CORR_FILE="$T/corr-empty"
STUB_CORRK_FILE="$T/corrk-empty"
: > "$STUB_SEM_FILE"; : > "$STUB_HYB_FILE"; : > "$STUB_KW_FILE"
: > "$STUB_CORR_FILE"; : > "$STUB_CORRK_FILE"
export STUB_SEM_FILE STUB_HYB_FILE STUB_KW_FILE STUB_CORR_FILE STUB_CORRK_FILE
run_hook "empty-1" "all recall lanes return empty fixtures"
assert "all-empty recall emits no hook context" "[[ ! -s '$T/stdout.empty-1' ]]"
EMPTY_EVENT=$(jq -c 'select(.event == "recall_empty" and .session_id == "empty-1")' "$MIND/outcome_ledger.jsonl")
assert "all-empty recall appends recall_empty" "[[ -n '$EMPTY_EVENT' ]]"
assert "recall_empty carries every attempted lane timing" \
    "printf '%s' '$EMPTY_EVENT' | jq -e '(.lane_ms | length) == 6 and .lane_ms.sem != null and .lane_ms.hyb != null and .lane_ms.kw != null and .lane_ms.corr != null and .lane_ms.corrk != null and .lane_ms.xr != null' >/dev/null"
assert "recall_empty marks empty lane files with booleans" \
    "printf '%s' '$EMPTY_EVENT' | jq -e '.lane_timeout | all(.[]; . == true)' >/dev/null"
assert "recall_empty carries total hook milliseconds" \
    "printf '%s' '$EMPTY_EVENT' | jq -e '.hook_ms >= 0' >/dev/null"

# ============================================================
# Item 6: CHITTA_RECALL_LANES_RPC=1 makes one fan-in call, consumes
# its verbatim lane text, and carries daemon lane timings unchanged.
# ============================================================
STUB_SEM_FILE="$T/sem-rpc"; STUB_HYB_FILE="$T/hyb-rpc"; STUB_KW_FILE="$T/kw-rpc"
STUB_CORR_FILE="$T/corr-rpc"; STUB_RPC_FILE="$T/recall-lanes.json"
cat > "$STUB_SEM_FILE" <<'EOF'
Smart recall (semantic, ep=17): 1 results
#31 [91%] [wisdom] persimmon fan-in fixture line
EOF
cat > "$STUB_HYB_FILE" <<'EOF'
Found 1 results in realm 'project:stubrealm' (maxrel 90%):
#32 [89%] [wisdom] persimmon hybrid fixture line
EOF
cat > "$STUB_KW_FILE" <<'EOF'
Found 1 results in realm 'project:stubrealm' (maxrel 90%):
#33 [80%] [wisdom] persimmon keyword fixture line
EOF
: > "$STUB_CORR_FILE"
jq -n --rawfile sem "$STUB_SEM_FILE" --rawfile hyb "$STUB_HYB_FILE" \
      --rawfile kw "$STUB_KW_FILE" --rawfile corr "$STUB_CORR_FILE" '
  {lanes: {
    sem:   {text:$sem,  results:[], ms:12, timed_out:false},
    hyb:   {text:$hyb,  results:[], ms:34, timed_out:false},
    kw:    {text:$kw,   results:[], ms:5,  timed_out:false},
    corr:  {text:$corr, results:[], ms:3,  timed_out:false},
    corrk: {text:"NO CORRECTION — no stored [correction] trigger matches this turn", results:[], ms:2, timed_out:false}
  }, total_ms:35}' > "$STUB_RPC_FILE"
STUB_CALL_LOG="$T/calls-rpc"; : > "$STUB_CALL_LOG"
export STUB_SEM_FILE STUB_HYB_FILE STUB_KW_FILE STUB_CORR_FILE STUB_RPC_FILE STUB_CALL_LOG
STUB_RPC_MODE=ok CHITTA_RECALL_LANES_RPC=1 run_hook "rpc-on" "what does the persimmon fixture show"
assert "fan-in switch makes exactly one recall_lanes CLI call" \
    "[[ \$(grep -c '^recall_lanes ' '$STUB_CALL_LOG') -eq 1 ]]"
assert "fan-in success skips standalone recall processes" \
    "! grep -Eq '^(smart_recall|recall|correction_check) ' '$STUB_CALL_LOG'"
assert "fan-in lane text reaches normal admission" "grep -q '\[sem\]#31' '$T/stdout.rpc-on'"
_rpc_timing='t:sem=12,hyb=34,kw=5,corr=3,corrk=2,total='
_rpc_ablated_timing='t:sem=12,corr=3,corrk=2,total='
if [[ "${CHITTA_HOOK_NOW:-}" =~ ^[1-9][0-9]{12}$ ]]; then
    _rpc_timing='t:sem=0,hyb=0,kw=0,corr=0,corrk=0,total='
    _rpc_ablated_timing='t:sem=0,corr=0,corrk=0,total='
fi
assert "fan-in lane timings come from RPC response" \
    "grep -Eq '$_rpc_timing' '$T/stdout.rpc-on'"

STUB_RPC_FILE="$T/recall-lanes-ablated.json"
jq 'del(.lanes.hyb, .lanes.kw)' "$T/recall-lanes.json" > "$STUB_RPC_FILE"
export STUB_RPC_FILE
: > "$STUB_CALL_LOG"
STUB_RPC_MODE=ok CHITTA_RECALL_LANES_RPC=1 CHITTA_ABLATE_LANES=hyb,kw \
    run_hook "rpc-ablated" "what does the persimmon fixture show"
assert "fan-in request omits ablated lanes" \
    "grep -Fq -- '--lanes [\"sem\",\"corr\",\"corrk\"]' '$STUB_CALL_LOG'"
assert "fan-in ablation omits their timing entries" \
    "grep -Eq '$_rpc_ablated_timing' '$T/stdout.rpc-ablated'"

# ============================================================
# Item 7: a failed fan-in call falls back, for that same prompt, to
# the original standalone process fan-out.
# ============================================================
STUB_RPC_FILE="$T/recall-lanes.json"
export STUB_RPC_FILE
: > "$STUB_CALL_LOG"
STUB_RPC_MODE=fail CHITTA_RECALL_LANES_RPC=1 run_hook "rpc-fallback" "what does the persimmon fixture show"
assert "failed fan-in was attempted once" \
    "[[ \$(grep -c '^recall_lanes ' '$STUB_CALL_LOG') -eq 1 ]]"
assert "failed fan-in falls back to smart_recall" "grep -q '^smart_recall ' '$STUB_CALL_LOG'"
assert "failed fan-in falls back to recall lanes" "grep -q '^recall ' '$STUB_CALL_LOG'"
assert "failed fan-in falls back to correction_check" "grep -q '^correction_check ' '$STUB_CALL_LOG'"
assert "fallback lane output reaches normal admission" "grep -q '\[sem\]#31' '$T/stdout.rpc-fallback'"

# Session continuity must select a result line, never a recall summary or warning.
# Use a large output budget so truncation cannot hide a broken last-session block.
STUB_SESSION_FILE="$T/session-summary"
export STUB_SESSION_FILE
for arm in 0 1; do
    for fixture in header empty warning metadata populated; do
        case "$fixture" in
            header) printf 'Found 1 results (maxrel 80%%):\n' > "$STUB_SESSION_FILE" ;;
            empty) : > "$STUB_SESSION_FILE" ;;
            warning) printf '[weak: no strongly-relevant memory]\nFound 0 results:\n' > "$STUB_SESSION_FILE" ;;
            metadata) printf 'Found 1 results (maxrel 80%%):\n#99 [80%%] [wisdom]   \n' > "$STUB_SESSION_FILE" ;;
            populated)
                printf 'Found 1 results (maxrel 80%%):\n#99 [80%%] [wisdom] persimmon continuity body\npersimmon continuity body\n' > "$STUB_SESSION_FILE"
                ;;
        esac
        sid="session-$arm-$fixture"
        rm -f "$MIND/.session_active"
        CHITTA_RECALL_LANES_RPC="$arm" CHITTA_MAX_OUTPUT_CHARS=10000 \
            run_hook "$sid" "what does the persimmon fixture show"
        assert "$sid reaches recall output" "grep -q '\[soul\]' '$T/stdout.$sid'"
        if [[ "$fixture" == populated ]]; then
            assert "$sid includes the memory body" \
                "grep -q '\[last-session\] #99 \[80%\] \[wisdom\] persimmon continuity body' '$T/stdout.$sid'"
        else
            assert "$sid suppresses empty continuity" "! grep -q '\[last-session\]' '$T/stdout.$sid'"
        fi
        assert "$sid never injects a recall header" "! grep -q '\[last-session\] Found' '$T/stdout.$sid'"
    done
done

# Synchronization markers prove both independent tasks overlap recall. No
# wall-clock threshold: the old serial scheduling cannot satisfy this handshake.
mkdir -p "$T/overlap"
export STUB_OVERLAP_DIR="$T/overlap"
rm -f "$MIND/.session_active"
CHITTA_RECALL_LANES_RPC=1 CHITTA_MAX_OUTPUT_CHARS=10000 \
    run_hook "rpc-concurrent" "what does the persimmon fixture show"
assert "RPC overlaps session recall and heartbeat" "[[ -f '$T/overlap/overlapped' ]]"
assert "heartbeat proceeds alongside RPC" "[[ -f '$T/overlap/heartbeat-overlapped' ]]"
assert "concurrent continuity is joined before rendering" \
    "grep -q '\\[last-session\\] #99' '$T/stdout.rpc-concurrent'"
unset STUB_OVERLAP_DIR

# A pipeline timeout replaces the old batch wait; it must not add a second one.
: > "$STUB_CALL_LOG"
STUB_PIPELINE_TIMEOUT=1 CHITTA_PROMPT_CONTEXT=1 CHITTA_RECALL_LANES_RPC=1 \
    run_hook "pipeline-timeout" "what does the persimmon fixture show"
assert "pipeline timeout skips a second batch wait" "! grep -q '^recall_lanes ' '$STUB_CALL_LOG'"
assert "pipeline timeout retains standalone lane fallback" "grep -q '^smart_recall ' '$STUB_CALL_LOG'"
assert "pipeline timeout preserves admission" "grep -q '\\[sem\\]#31' '$T/stdout.pipeline-timeout'"

# Even with a model installed, ordinary turns must not start Python. Matching
# regex evidence still requests a model verdict before emitting a learning hint.
mkdir -p "$T/interpreters"
cat > "$T/interpreters/python3" <<'STUBPY'
#!/bin/bash
printf '%s\n' "$*" >> "$STUB_PYTHON_CALLS"
echo 'correction 0.990'
STUBPY
chmod +x "$T/interpreters/python3"
touch "$MIND/hook-classifier.bin"
export STUB_PYTHON_CALLS="$T/python-calls"
PATH="$T/interpreters:$PATH" run_hook "no-classifier" "what does the persimmon fixture show"
assert "ordinary turn skips installed classifier" "[[ ! -f '$STUB_PYTHON_CALLS' ]]"
PATH="$T/interpreters:$PATH" run_hook "with-classifier" "you are wrong about the persimmon fixture"
assert "regex evidence invokes classifier once" "[[ $(wc -l < "$STUB_PYTHON_CALLS") == 1 ]]"
assert "matching model and regex preserve correction hint" "grep -q 'CORRECTION detected' '$T/stdout.with-classifier'"
rm -f "$MIND/hook-classifier.bin"

# A stale socket and a missing socket both retain liveness in the durable queue.
for offline in failed-rpc missing-socket; do
    [[ "$offline" == missing-socket ]] && rm -f "$SOCK"
    STUB_HEARTBEAT_FAIL=1 run_hook "$offline" "what does the persimmon fixture show"
    # Heartbeat is asynchronous; wait for its success marker, with a bounded retry.
    for ((attempt=0; attempt<100; attempt++)); do
        [[ -f "$MIND/.hb_$offline" ]] && break
        sleep 0.01
    done
    assert "$offline queues heartbeat" \
        "jq -se 'any(.[]; .tool == \"session_heartbeat\" and .args.session_id == \"$offline\")' '$CHITTA_QUEUE' >/dev/null"
    assert "$offline marks heartbeat only after queue success" "[[ -f '$MIND/.hb_$offline' ]]"
done

exit $FAIL
