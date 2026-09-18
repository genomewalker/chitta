# LLM endpoint operations

The pool routes a role to an endpoint serving its exact required model. It never
substitutes a smaller model. The distiller uses its configured teacher model
(default `gemma4:26b`). Remote hint extraction is opt-in through
`CHITTA_ROLE_HINT_MODEL`; otherwise hints continue using local GGUF inference.
Other role model variables are listed in [HOOKS.md](HOOKS.md#environment-variables).

## Declare an endpoint

Place a JSON file in `~/.chitta-bridge/endpoints/`, for example the checked-in
[RTX declaration](endpoints/rtx-dandycomp06fl.json). Fields are `url`, `kind`
(`ollama`, `vllm`, or `openai`), `label`, `always_on`, `roles`, and `priority`.
An omitted/empty role list permits every role; omitted priority is 100.
Legacy `.url` files remain untagged, non-always-on endpoints. The pool also checks
node-local `/tmp/ollama-server-*.url`, localhost:11434, and running Slurm jobs.
JSON declarations override legacy declarations for the same URL. A trailing `/v1`
is normalized to the server root so OpenAI-style legacy URLs remain usable.

Eligible endpoints must advertise the exact required model through `/v1/models`
(Ollama also supports `/api/tags`). Routing never substitutes a different model.
Explicit endpoint configuration pins a request to that server, while still applying
model checks and admission limits. Unpinned requests can use every eligible server.
Inventory never starts jobs; the existing discovery/provisioning path can start a
Slurm server when the required model has no reachable candidate.

## Load and admission

A background worker probes endpoints every `CHITTA_ROUTER_PROBE_S` seconds
(default 20). Requests read the latest snapshot; initial discovery and an explicit
`--probe` wait for the worker. Model inventory refreshes every
`CHITTA_ENDPOINT_TTL_S` seconds (default 60), and on inference failure.

For vLLM, the worker reads running/waiting requests and GPU cache occupancy from
`/metrics`. For Ollama it reads `/api/ps` and times a one-token generation on the
smallest loaded model; with no loaded model it times `/api/ps` alone, avoiding a
model load. OpenAI-compatible servers use a transport-only model-list probe because
there is no portable load API. Legacy filenames containing `vllm` select that
backend; otherwise legacy declarations default to Ollama. Use JSON to specify kind.

The router retains an EWMA of probe latency and an idle baseline. Waiting vLLM
requests or a latency above three times the baseline mark an endpoint busy. Two
consecutive good probes are required to recover; three failures mark it down.
Changing between metadata and generation probes resets the baseline. A failed
inference excludes the endpoint for five seconds and requests a background refresh.

Interactive requests (hint, judge) choose the fastest eligible non-busy endpoint,
then a busy fallback if necessary. Batch requests (teacher, student) choose the
most spare capacity, using inflight slots, running requests and GPU cache occupancy.
Ties prefer always-on servers, lower numeric priority, then model-probe latency.
Batch waits within the caller's timeout when all candidates are busy or admission
limits are exhausted; missing models return immediately. A busy load probe blocks
batch admission for `CHITTA_ROUTER_BACKOFF_S` (default 120 seconds).

`CHITTA_ROUTER_MAX_INFLIGHT_<LABEL>` caps admission (default 4 for always-on
servers, otherwise `OLLAMA_NUM_PARALLEL`, falling back to 1).
`CHITTA_ROUTER_TPS_BUDGET_<LABEL>` sets the output-token budget (default 60 tokens/s
for always-on servers, 0/unlimited for other servers). Uppercase the label and
replace punctuation with underscores: the staged RTX label uses suffix
`RTX_DANDYCOMP06FL`. Admission reserves the request's maximum output tokens;
completion refunds unused tokens using reported usage. Missing usage conservatively
charges the reservation. A request can borrow against future tokens, but another
request waits until the balance recovers. Limits apply within each router process;
separate hint daemons and CLI jobs need their own budgets within the shared allowance.
They do not coordinate a cluster-wide quota. Running requests are not preempted.

`chitta endpoints` reads the daemon's router state when available, with a labeled
local fallback. `--local` or `CHITTA_ENDPOINT_DIR` selects local inventory;
`--distill-model MODEL` sets its teacher model. `--probe` forces background refresh,
and `--json` returns structured data. RPC `endpoint_list` accepts `{"probe":true}`
and uses the daemon's configured teacher model. Reports include reachability,
served models, state, model/load probe latencies, EWMA, baseline, inflight limits,
role winners and last routing decisions. Debug routing messages explain admission
or waiting decisions. An unset role model has no winner until a caller supplies it.
Advertised model support does not guarantee a model fits available VRAM.

## RTX tunnel and relay

The RTX machine at 10.75.203.7 is not routable from cluster nodes. Its reverse SSH
tunnel terminates on `dandycomp06fl:127.0.0.1:11435`. The user unit
`rtx-relay.service` on dandycomp06fl republishes that listener on port 11436,
producing `http://dandycomp06fl:11436` for compute and login nodes. The relay's
ConditionHost must remain restricted to dandycomp06fl. The owner runbook is
`/projects/caeg/scratch/kbd606/tmp/rtx-tunnel-for-bryant.md`.

The orchestrator publishes the RTX declaration after review; implementation
streams do not change home-directory configuration, install binaries, or restart
services. Keep Slurm Ollama/vLLM `.url` declarations in place for fallback.
Until the RTX box advertises `gemma4:26b`, it cannot win the default teacher role.
After installing a new model on the box, force a probe to refresh its eligibility.
A role whose model is available only there will select the box. A down tunnel or
relay makes the endpoint unreachable and excludes it until a successful probe.
