// register_protocol_tools — chunk extracted from register_tools() so editing
// tool metadata only retemplates one chunk at a time.

#include "../../include/chitta/rpc/field_handler.hpp"

namespace chitta {

void FieldRpcHandler::register_protocol_tools() {
    register_tool_table({
        {"assert_fact", "Assert a constraint fact (subject-predicate-object) with provenance and scope. Auto-detects conflicts and creates rival branches.",
            {{"type","object"},{"properties",{
                {"subject",{{"type","string"},{"description","Entity (e.g. 'user', 'project-X')"}}},
                {"predicate",{{"type","string"},{"description","Relation (e.g. 'prefers', 'uses', 'located-in')"}}},
                {"object",{{"type","string"},{"description","Value (e.g. 'Rust', 'vim', 'Copenhagen')"}}},
                {"confidence",{{"type","number"},{"description","Confidence 0-1 (default 0.8)"}}},
                {"scope",{{"type","string"},{"description","Scope: global, realm name, or session (default: global)"}}},
                {"branch_id",{{"type","integer"},{"description","Branch to assert into (0=trunk)"}}},
                {"provenance_source",{{"type","string"},{"description","Source: user, tool, distillation, inference"}}},
                {"confidence_basis",{{"type","string"},{"description","Basis: stated, observed, derived, corrected"}}}
            }},{"required",{"subject","predicate","object"}}},
            &FieldRpcHandler::tool_assert_fact, handlers_["assert_fact"]},

        {"retract_fact", "Soft-retract a constraint fact (preserves history)",
            {{"type","object"},{"properties",{
                {"fact_id",{{"type","integer"},{"description","Fact ID to retract"}}}
            }},{"required",{"fact_id"}}},
            &FieldRpcHandler::tool_retract_fact, handlers_["retract_fact"]},

        {"query_unify", "Pattern-match query against constraint store (unification with wildcards)",
            {{"type","object"},{"properties",{
                {"subject",{{"type","string"},{"description","Subject filter (omit for wildcard)"}}},
                {"predicate",{{"type","string"},{"description","Predicate filter"}}},
                {"object",{{"type","string"},{"description","Object filter"}}},
                {"scope",{{"type","string"},{"description","Scope filter"}}}
            }}},
            &FieldRpcHandler::tool_query_unify, handlers_["query_unify"]},

        {"query_chain", "Follow predicate chain: A→B→C through constraint facts",
            {{"type","object"},{"properties",{
                {"subject",{{"type","string"},{"description","Starting entity"}}},
                {"predicates",{{"type","array"},{"items",{{"type","string"}}},{"description","Ordered list of predicates to follow"}}}
            }},{"required",{"subject","predicates"}}},
            &FieldRpcHandler::tool_query_chain, handlers_["query_chain"]},

        {"explain_fact", "Explain a fact: provenance chain + supporting/conflicting facts",
            {{"type","object"},{"properties",{
                {"fact_id",{{"type","integer"},{"description","Fact ID to explain"}}}
            }},{"required",{"fact_id"}}},
            &FieldRpcHandler::tool_explain_fact, handlers_["explain_fact"]},

        {"branch_create", "Fork a rival branch for conflicting interpretations",
            {{"type","object"},{"properties",{
                {"parent_id",{{"type","integer"},{"description","Parent branch ID (0=trunk)"}}},
                {"scope",{{"type","string"},{"description","Branch scope (default: global)"}}}
            }}},
            &FieldRpcHandler::tool_branch_create, handlers_["branch_create"]},

        {"branch_resolve", "Resolve a branch conflict: winner stays, loser abandoned",
            {{"type","object"},{"properties",{
                {"winner_id",{{"type","integer"},{"description","Branch ID that wins"}}},
                {"loser_id",{{"type","integer"},{"description","Branch ID to abandon"}}}
            }},{"required",{"winner_id","loser_id"}}},
            &FieldRpcHandler::tool_branch_resolve, handlers_["branch_resolve"]},

        // ── Layer 2: Trigger Tissue ─────────────────────────────────────────
        {"trigger_add", "Create a trigger automaton (prospective memory). Arms on creation, fires when conditions met or tension exceeds threshold.",
            {{"type","object"},{"properties",{
                {"name",{{"type","string"},{"description","Human-readable trigger name"}}},
                {"condition",{{"type","object"},{"description","Trigger condition (TimeAfter, ConstraintMatch, EventMatch, AllOf, AnyOf)"}}},
                {"action",{{"type","object"},{"description","Action on fire (Notify, InjectMemory, EmitEvent, RememberFact)"}}},
                {"deadline_ms",{{"type","integer"},{"description","Deadline timestamp ms (0=no deadline)"}}},
                {"tension_threshold",{{"type","number"},{"description","Tension level to auto-fire (default 0.8)"}}},
                {"gain",{{"type","number"},{"description","Emotional importance 0-1 (default 0.5)"}}},
                {"realm",{{"type","string"},{"description","Realm scope (default: global)"}}}
            }},{"required",{"name","condition","action"}}},
            &FieldRpcHandler::tool_trigger_add, handlers_["trigger_add"]},

        {"trigger_list", "List all triggers with their status",
            {{"type","object"}},
            &FieldRpcHandler::tool_trigger_list, handlers_["trigger_list"]},

        {"trigger_fire", "Manually fire a trigger",
            {{"type","object"},{"properties",{
                {"trigger_id",{{"type","integer"},{"description","Trigger ID to fire"}}}
            }},{"required",{"trigger_id"}}},
            &FieldRpcHandler::tool_trigger_fire, handlers_["trigger_fire"]},

        {"trigger_dismiss", "Expire/dismiss a trigger without firing",
            {{"type","object"},{"properties",{
                {"trigger_id",{{"type","integer"},{"description","Trigger ID to dismiss"}}}
            }},{"required",{"trigger_id"}}},
            &FieldRpcHandler::tool_trigger_dismiss, handlers_["trigger_dismiss"]},

        // ── Layer 3: Predictive Memory ──────────────────────────────────────
        {"predict_needed", "Get predicted next-needed memories from the Markov chain access predictor",
            {{"type","object"},{"properties",{
                {"k",{{"type","integer"},{"description","Number of predictions (default 8)"}}}
            }}},
            &FieldRpcHandler::tool_predict_needed, handlers_["predict_needed"]},

        // ── Layer 4: Surprise Memory ─────────────────────────────────────────
        {"record_surprise", "Record a prediction error event — what was expected vs what actually happened",
            {{"type","object"},{"properties",{
                {"context_sketch",{{"type","string"},{"description","What was happening when the surprise occurred"}}},
                {"action",{{"type","string"},{"description","What action was taken"}}},
                {"expected",{{"type","string"},{"description","What was predicted/expected (optional)"}}},
                {"actual",{{"type","string"},{"description","What actually happened (required)"}}},
                {"surprise_magnitude",{{"type","number"},{"description","How surprising [0-1] (default 0.5)"}}},
                {"domain",{{"type","string"},{"description","Domain: recall, tool, user_correction, constraint (default general)"}}},
                {"realm",{{"type","string"},{"description","Realm filter (default global)"}}},
                {"session_id",{{"type","string"},{"description","Session ID (optional)"}}},
                {"source_memory_id",{{"type","integer"},{"description","Related memory ID (optional)"}}}
            }},{"required",{"actual"}}},
            &FieldRpcHandler::tool_record_surprise, handlers_["record_surprise"]},

        {"query_surprises", "Query recorded surprise/prediction-error events with filters",
            {{"type","object"},{"properties",{
                {"domain",{{"type","string"},{"description","Filter by domain"}}},
                {"realm",{{"type","string"},{"description","Filter by realm"}}},
                {"min_magnitude",{{"type","number"},{"description","Minimum surprise magnitude"}}},
                {"since_ms",{{"type","integer"},{"description","Only events after this timestamp (ms)"}}},
                {"limit",{{"type","integer"},{"description","Max results (default 50)"}}}
            }}},
            &FieldRpcHandler::tool_query_surprises, handlers_["query_surprises"]},

        {"get_blind_spots", "Identify recurring surprise patterns — domains/actions where predictions consistently fail",
            {{"type","object"},{"properties",{
                {"realm",{{"type","string"},{"description","Filter by realm"}}},
                {"limit",{{"type","integer"},{"description","Max blind spots (default 10)"}}}
            }}},
            &FieldRpcHandler::tool_get_blind_spots, handlers_["get_blind_spots"]},

        {"surprise_stats", "Summary statistics for surprise memory: counts, avg magnitude, domain breakdown",
            {{"type","object"}},
            &FieldRpcHandler::tool_surprise_stats, handlers_["surprise_stats"]},

        // ── Layer 5: Epistemic Debt ──────────────────────────────────────────
        {"register_debt", "Register an epistemic uncertainty — competing hypotheses that need resolution",
            {{"type","object"},{"properties",{
                {"pattern",{{"type","string"},{"description","The uncertain pattern/belief (required)"}}},
                {"competing_hypotheses",{{"type","array"},{"items",{{"type","string"}}},{"description","Competing explanations"}}},
                {"discriminating_test",{{"type","string"},{"description","How to distinguish between hypotheses"}}},
                {"fragility_score",{{"type","number"},{"description","How fragile this belief is [0-1] (default 0.5)"}}},
                {"domain",{{"type","string"},{"description","Domain (default general)"}}},
                {"realm",{{"type","string"},{"description","Realm (default global)"}}},
                {"session_id",{{"type","string"},{"description","Session ID (optional)"}}}
            }},{"required",{"pattern"}}},
            &FieldRpcHandler::tool_register_debt, handlers_["register_debt"]},

        {"resolve_debt", "Mark an epistemic debt as resolved with a resolution",
            {{"type","object"},{"properties",{
                {"debt_id",{{"type","integer"},{"description","Debt ID to resolve"}}},
                {"resolution",{{"type","string"},{"description","How the uncertainty was resolved"}}}
            }},{"required",{"debt_id","resolution"}}},
            &FieldRpcHandler::tool_resolve_debt, handlers_["resolve_debt"]},

        {"defer_debt", "Defer an epistemic debt for later investigation",
            {{"type","object"},{"properties",{
                {"debt_id",{{"type","integer"},{"description","Debt ID to defer"}}}
            }},{"required",{"debt_id"}}},
            &FieldRpcHandler::tool_defer_debt, handlers_["defer_debt"]},

        {"query_debts", "Query epistemic debts with filters",
            {{"type","object"},{"properties",{
                {"status",{{"type","string"},{"description","Filter: open, resolved, deferred"}}},
                {"domain",{{"type","string"},{"description","Filter by domain"}}},
                {"realm",{{"type","string"},{"description","Filter by realm"}}},
                {"min_fragility",{{"type","number"},{"description","Minimum fragility score"}}},
                {"limit",{{"type","integer"},{"description","Max results (default 50)"}}}
            }}},
            &FieldRpcHandler::tool_query_debts, handlers_["query_debts"]},

        {"get_fragile_decisions", "List open epistemic debts sorted by fragility — decisions most likely to be wrong",
            {{"type","object"},{"properties",{
                {"threshold",{{"type","number"},{"description","Minimum fragility threshold (default 0.5)"}}},
                {"limit",{{"type","integer"},{"description","Max results (default 20)"}}}
            }}},
            &FieldRpcHandler::tool_get_fragile_decisions, handlers_["get_fragile_decisions"]},

        {"debt_stats", "Summary statistics for epistemic debt: counts by status, avg fragility",
            {{"type","object"}},
            &FieldRpcHandler::tool_debt_stats, handlers_["debt_stats"]},

        // ── Layer 6: Integration Kernel ──────────────────────────────────────
        {"record_feedback", "Record whether a recall source was useful — updates learned source weights",
            {{"type","object"},{"properties",{
                {"query_domain",{{"type","string"},{"description","Domain of the query (default general)"}}},
                {"source",{{"type","string"},{"description","Source: semantic, keyword, temporal, artifact, association (required)"}}},
                {"was_useful",{{"type","boolean"},{"description","Whether the source's results were useful (default true)"}}}
            }},{"required",{"source"}}},
            &FieldRpcHandler::tool_record_feedback, handlers_["record_feedback"]},

        {"get_source_weights", "View learned recall source weights — how much each source is trusted per domain",
            {{"type","object"},{"properties",{
                {"domain",{{"type","string"},{"description","Filter by domain (omit for all)"}}}
            }}},
            &FieldRpcHandler::tool_get_source_weights, handlers_["get_source_weights"]},

        {"update_source_weight", "Manually override a recall source weight",
            {{"type","object"},{"properties",{
                {"source",{{"type","string"},{"description","Source name (required)"}}},
                {"domain",{{"type","string"},{"description","Domain (default general)"}}},
                {"weight",{{"type","number"},{"description","New weight [0-2] (default 1.0)"}}}
            }},{"required",{"source"}}},
            &FieldRpcHandler::tool_update_source_weight, handlers_["update_source_weight"]},

        {"integration_stats", "Per-source success rates and learned weights across all domains",
            {{"type","object"}},
            &FieldRpcHandler::tool_integration_stats, handlers_["integration_stats"]},

        // ── Autonomous Learning (Moves 1-6) ─────────────────────────────────
        {"surprise_learning_stats", "Rolling surprise credit stats — tracked memories, gates passed, strength adjustments",
            {{"type","object"}},
            &FieldRpcHandler::registered_surprise_learning_stats, handlers_["surprise_learning_stats"]},

        {"upsert_wisdom_candidate", "Create or update a wisdom candidate from clustered surprise patterns",
            {{"type","object"},{"properties",{
                {"cluster_key",{{"type","string"},{"description","Unique key for this pattern cluster (domain+action+sig)"}}},
                {"domain",{{"type","string"},{"description","Knowledge domain"}}},
                {"action",{{"type","string"},{"description","Action or behavior pattern"}}},
                {"summary",{{"type","string"},{"description","Human-readable summary of the wisdom"}}},
                {"episode_ids",{{"type","array"},{"items",{{"type","integer"}}},{"description","Surprise event IDs supporting this candidate"}}},
                {"debt_ids",{{"type","array"},{"items",{{"type","integer"}}},{"description","Resolved debt IDs linked to this candidate"}}},
                {"support_count",{{"type","integer"},{"description","Number of supporting episodes"}}},
                {"cross_session_count",{{"type","integer"},{"description","Number of distinct sessions with evidence"}}},
                {"mean_surprise",{{"type","number"},{"description","Average surprise magnitude across episodes"}}},
                {"promotion_score",{{"type","number"},{"description","Computed promotion readiness score 0-1"}}}
            }},{"required",{"cluster_key"}}},
            &FieldRpcHandler::registered_upsert_wisdom_candidate, handlers_["upsert_wisdom_candidate"]},

        {"update_wisdom_lifecycle", "Advance a wisdom candidate through lifecycle stages: candidate→provisional→trusted→demoted",
            {{"type","object"},{"properties",{
                {"candidate_id",{{"type","integer"},{"description","Wisdom candidate ID"}}},
                {"new_state",{{"type","integer"},{"description","0=candidate, 1=provisional, 2=trusted, 3=demoted"}}}
            }},{"required",{"candidate_id","new_state"}}},
            &FieldRpcHandler::registered_update_wisdom_lifecycle, handlers_["update_wisdom_lifecycle"]},

        {"query_wisdom_candidates", "Query wisdom candidates by lifecycle stage and/or domain",
            {{"type","object"},{"properties",{
                {"lifecycle",{{"type","integer"},{"description","Filter by lifecycle: 0=candidate, 1=provisional, 2=trusted, 3=demoted"}}},
                {"domain",{{"type","string"},{"description","Filter by domain"}}},
                {"limit",{{"type","integer"},{"description","Max results (default 50)"}}}
            }}},
            &FieldRpcHandler::registered_query_wisdom_candidates, handlers_["query_wisdom_candidates"]},

        {"wisdom_promotion_stats", "Overview of wisdom promotion pipeline — total candidates by lifecycle stage",
            {{"type","object"}},
            &FieldRpcHandler::registered_wisdom_promotion_stats, handlers_["wisdom_promotion_stats"]},

        {"attach_debt_evidence", "Attach supporting evidence to an epistemic debt — memory IDs + confidence",
            {{"type","object"},{"properties",{
                {"debt_id",{{"type","integer"},{"description","Epistemic debt ID"}}},
                {"memory_ids",{{"type","array"},{"items",{{"type","integer"}}},{"description","Memory IDs that serve as evidence"}}},
                {"confidence",{{"type","number"},{"description","Evidence confidence 0-1 (default 0.5)"}}},
                {"note",{{"type","string"},{"description","Optional note about the evidence"}}}
            }},{"required",{"debt_id"}}},
            &FieldRpcHandler::registered_attach_debt_evidence, handlers_["attach_debt_evidence"]},

        {"update_scorer_model", "Apply learned weight deltas to the scoring model from outcome calibration",
            {{"type","object"},{"properties",{
                {"weights",{{"type","object"},{"description","Factor name → {delta, min_delta, max_delta} learned adjustments"}}},
                {"model_version",{{"type","integer"},{"description","Monotonic version number"}}},
                {"mean_loss",{{"type","number"},{"description","EWMA loss from calibration"}}},
                {"outcome_count",{{"type","integer"},{"description","Total outcomes used for calibration"}}}
            }}},
            &FieldRpcHandler::registered_update_scorer_model, handlers_["update_scorer_model"]},

        {"learned_scorer_stats", "Current learned scoring model — version, factor count, loss, outcome count",
            {{"type","object"}},
            &FieldRpcHandler::registered_learned_scorer_stats, handlers_["learned_scorer_stats"]},

        {"effective_scorer_weights", "Show effective scoring weights — baseline + learned deltas for all factors",
            {{"type","object"}},
            &FieldRpcHandler::registered_effective_scorer_weights, handlers_["effective_scorer_weights"]},

        // ── Layer 7: Intervention Ledger ─────────────────────────────────────
        {"start_intervention", "Begin tracking an agent intervention — records intent, action, preconditions and expected observables before execution",
            {{"type","object"},{"properties",{
                {"realm",{{"type","string"},{"description","Realm: coding, research, planning (default: coding)"}}},
                {"session_id",{{"type","string"},{"description","Current session ID"}}},
                {"task_id",{{"type","integer"},{"description","Optional task ID"}}},
                {"agent_id",{{"type","string"},{"description","Agent performing the action"}}},
                {"domain",{{"type","string"},{"description","Domain area (e.g. git, filesystem, testing, compiler)"}}},
                {"intent",{{"type","string"},{"description","What the agent intends to achieve"}}},
                {"action_type",{{"type","integer"},{"description","0=ToolCall 1=MultiStepPlan 2=Delegation 3=Edit 4=Command"}}},
                {"action_ref",{{"type","string"},{"description","Reference to the action (tool name, file path, command)"}}},
                {"preconditions",{{"type","array"},{"items",{{"type","string"}}},{"description","Known preconditions"}}},
                {"expected_observables",{{"type","array"},{"items",{{"type","string"}}},{"description","What success looks like"}}},
                {"reversal_cost",{{"type","integer"},{"description","0=None 1=Low 2=Medium 3=High"}}}
            }},{"required",{"intent","action_ref"}}},
            &FieldRpcHandler::registered_start_intervention, handlers_["start_intervention"]},

        {"add_observation", "Record an observation during an open intervention (stdout, test result, file diff, etc.)",
            {{"type","object"},{"properties",{
                {"intervention_id",{{"type","integer"},{"description","Intervention ID from start_intervention"}}},
                {"kind",{{"type","integer"},{"description","0=Stdout 1=Stderr 2=FileDiff 3=TestResult 4=EnvState 5=UserFeedback"}}},
                {"summary",{{"type","string"},{"description","Human-readable observation summary"}}},
                {"confidence",{{"type","number"},{"description","Confidence in this observation (0.0-1.0)"}}},
                {"evidence_refs",{{"type","array"},{"items",{{"type","integer"}}},{"description","Memory IDs that constitute evidence"}}}
            }},{"required",{"intervention_id","summary"}}},
            &FieldRpcHandler::registered_add_observation, handlers_["add_observation"]},

        {"close_intervention", "Close an intervention with its final outcome status",
            {{"type","object"},{"properties",{
                {"intervention_id",{{"type","integer"},{"description","Intervention ID to close"}}},
                {"status",{{"type","integer"},{"description","0=Open 1=Succeeded 2=Failed 3=Partial 4=Aborted"}}}
            }},{"required",{"intervention_id","status"}}},
            &FieldRpcHandler::registered_close_intervention, handlers_["close_intervention"]},

        {"record_attribution", "Attribute a closed intervention to a causal class — routes feedback to the appropriate learning subsystem",
            {{"type","object"},{"properties",{
                {"intervention_id",{{"type","integer"},{"description","Intervention ID"}}},
                {"primary_class",{{"type","integer"},{"description","0=MemoryRecallError 1=SourceTrustError 2=ProcedureError 3=ToolExecutionError 4=EnvironmentShift 5=HiddenPrecondition 6=AmbiguousState 7=GoalSpecError 8=UserOverride 9=ExternalNondeterminism"}}},
                {"secondary_class",{{"type","integer"},{"description","Optional secondary attribution class (same enum)"}}},
                {"confidence_delta",{{"type","number"},{"description","Magnitude of the learning signal (0.0-1.0)"}}},
                {"surprise_id",{{"type","integer"},{"description","Linked surprise event ID if available"}}},
                {"debt_ids",{{"type","array"},{"items",{{"type","integer"}}},{"description","Linked epistemic debt IDs"}}},
                {"source_memory_ids",{{"type","array"},{"items",{{"type","integer"}}},{"description","Memory IDs that contributed to this outcome"}}},
                {"skill_memory_ids",{{"type","array"},{"items",{{"type","integer"}}},{"description","Skill memory IDs that were applied"}}},
                {"note",{{"type","string"},{"description","Optional human-readable note"}}}
            }},{"required",{"intervention_id","primary_class"}}},
            &FieldRpcHandler::registered_record_attribution, handlers_["record_attribution"]},

        {"query_interventions", "Query the intervention ledger with optional filters",
            {{"type","object"},{"properties",{
                {"realm",{{"type","string"},{"description","Filter by realm"}}},
                {"session_id",{{"type","string"},{"description","Filter by session ID"}}},
                {"status",{{"type","integer"},{"description","Filter by status (0-4)"}}},
                {"limit",{{"type","integer"},{"description","Max results (default 50)"}}}
            }}},
            &FieldRpcHandler::registered_query_interventions, handlers_["query_interventions"]},

        {"get_intervention", "Get a single intervention record by ID",
            {{"type","object"},{"properties",{
                {"intervention_id",{{"type","integer"},{"description","Intervention ID"}}}
            }},{"required",{"intervention_id"}}},
            &FieldRpcHandler::registered_get_intervention, handlers_["get_intervention"]},

        {"intervention_stats", "Show intervention ledger statistics — total, open, succeeded, failed, aborted counts",
            {{"type","object"}},
            &FieldRpcHandler::registered_intervention_stats, handlers_["intervention_stats"]},

        {"list_open_interventions", "List all currently open (in-progress) interventions",
            {{"type","object"}},
            &FieldRpcHandler::registered_list_open_interventions, handlers_["list_open_interventions"]},

        // ── Layer 8: Agent Protocol Memory ───────────────────────────────────
        {"register_task", "Register a task contract — records goal, constraints, acceptance criteria, priority, and optional deadline for an ongoing agent task",
            {{"type","object"},{"properties",{
                {"session_id",{{"type","string"},{"description","Session this task belongs to"}}},
                {"realm",{{"type","string"},{"description","Realm: coding, research, planning (default: coding)"}}},
                {"goal",{{"type","string"},{"description","Task goal description"}}},
                {"constraints",{{"type","array"},{"items",{{"type","string"}}},{"description","Constraints that must be respected"}}},
                {"acceptance_criteria",{{"type","array"},{"items",{{"type","string"}}},{"description","Criteria for task completion"}}},
                {"priority",{{"type","integer"},{"description","Priority 1-10 (default 5)"}}},
                {"parent_task_id",{{"type","integer"},{"description","Parent task ID for subtasks"}}},
                {"tags",{{"type","array"},{"items",{{"type","string"}}},{"description","Optional tags"}}},
                {"deadline_ms",{{"type","integer"},{"description","Optional deadline as Unix ms timestamp"}}}
            }},{"required",{"goal"}}},
            &FieldRpcHandler::registered_register_task, handlers_["register_task"]},

        {"update_task", "Update task status (Active=0, Blocked=1, Completed=2, Failed=3, Abandoned=4), optionally attach an intervention ID or add a tag",
            {{"type","object"},{"properties",{
                {"task_id",{{"type","integer"},{"description","Task ID"}}},
                {"status",{{"type","integer"},{"description","0=Active 1=Blocked 2=Completed 3=Failed 4=Abandoned"}}},
                {"add_intervention_id",{{"type","integer"},{"description","Attach intervention to task"}}},
                {"add_tag",{{"type","string"},{"description","Add a tag to the task"}}}
            }},{"required",{"task_id","status"}}},
            &FieldRpcHandler::registered_update_task, handlers_["update_task"]},

        {"add_delegation", "Record a delegation edge — tracks which agent handed off to which, with optional handoff note",
            {{"type","object"},{"properties",{
                {"task_id",{{"type","integer"},{"description","Task ID"}}},
                {"from_agent",{{"type","string"},{"description","Delegating agent name"}}},
                {"to_agent",{{"type","string"},{"description","Receiving agent name"}}},
                {"handoff_note",{{"type","string"},{"description","Optional context passed at handoff"}}}
            }},{"required",{"task_id","from_agent","to_agent"}}},
            &FieldRpcHandler::registered_add_delegation, handlers_["add_delegation"]},

        {"link_evidence", "Link a memory to a task as evidence — records which agent produced it and evidence kind",
            {{"type","object"},{"properties",{
                {"task_id",{{"type","integer"},{"description","Task ID"}}},
                {"memory_id",{{"type","integer"},{"description","Memory ID to link"}}},
                {"produced_by",{{"type","string"},{"description","Agent that produced this evidence"}}},
                {"evidence_kind",{{"type","integer"},{"description","0=Observation 1=Artifact 2=Result 3=Analysis 4=UserFeedback"}}},
                {"relevance",{{"type","number"},{"description","Relevance score 0-1 (default 1.0)"}}}
            }},{"required",{"task_id","memory_id"}}},
            &FieldRpcHandler::registered_link_evidence, handlers_["link_evidence"]},

        {"add_probe", "Add a pending probe — an open question that must be answered to unblock or complete a task",
            {{"type","object"},{"properties",{
                {"task_id",{{"type","integer"},{"description","Task ID"}}},
                {"question",{{"type","string"},{"description","Open question to be answered"}}},
                {"expected_answerer",{{"type","string"},{"description","Agent or role expected to answer"}}},
                {"priority",{{"type","integer"},{"description","Priority 1-10 (default 5)"}}}
            }},{"required",{"task_id","question"}}},
            &FieldRpcHandler::registered_add_probe, handlers_["add_probe"]},

        {"resolve_probe", "Resolve a pending probe — mark as Answered (1) or Dismissed (2) and optionally record the answer",
            {{"type","object"},{"properties",{
                {"probe_id",{{"type","integer"},{"description","Probe ID"}}},
                {"status",{{"type","integer"},{"description","1=Answered 2=Dismissed"}}},
                {"answer",{{"type","string"},{"description","Optional answer text"}}}
            }},{"required",{"probe_id","status"}}},
            &FieldRpcHandler::registered_resolve_probe, handlers_["resolve_probe"]},

        {"set_criterion", "Upsert a completion criterion for a task — creates if new, updates if existing criterion text matches",
            {{"type","object"},{"properties",{
                {"task_id",{{"type","integer"},{"description","Task ID"}}},
                {"criterion",{{"type","string"},{"description","Criterion description"}}},
                {"is_met",{{"type","boolean"},{"description","Whether criterion is met (default false)"}}},
                {"evidence_note",{{"type","string"},{"description","Optional evidence supporting the criterion check"}}}
            }},{"required",{"task_id","criterion"}}},
            &FieldRpcHandler::registered_set_criterion, handlers_["set_criterion"]},

        {"get_task", "Get full task view — contract, delegations, evidence links, pending probes, and completion criteria",
            {{"type","object"},{"properties",{
                {"task_id",{{"type","integer"},{"description","Task ID"}}}
            }},{"required",{"task_id"}}},
            &FieldRpcHandler::registered_get_task, handlers_["get_task"]},

        {"query_tasks", "Query task contracts — filter by realm, session, status, or tag",
            {{"type","object"},{"properties",{
                {"realm",{{"type","string"},{"description","Filter by realm"}}},
                {"session_id",{{"type","string"},{"description","Filter by session ID"}}},
                {"status",{{"type","integer"},{"description","Filter by status (0-4)"}}},
                {"tag",{{"type","string"},{"description","Filter by tag"}}},
                {"limit",{{"type","integer"},{"description","Max results (default 50)"}}}
            }}},
            &FieldRpcHandler::registered_query_tasks, handlers_["query_tasks"]},

        {"agent_protocol_stats", "Show agent protocol memory statistics — total tasks, delegations, evidence links, probes, criteria counts",
            {{"type","object"}},
            &FieldRpcHandler::registered_agent_protocol_stats, handlers_["agent_protocol_stats"]},

        // ── Wisdom Homeostasis (Layer 9) ─────────────────────────────────────
        {"enroll_wisdom_lineage", "Enroll a Trusted wisdom candidate into the Wisdom Homeostasis layer — creates a living WisdomLineage record that tracks belief integrity over time",
            {{"type","object"},{"properties",{
                {"wisdom_candidate_id",{{"type","integer"},{"description","ID of the WisdomCandidate to enroll"}}},
                {"claim",{{"type","string"},{"description","The claim this wisdom encodes"}}},
                {"envelope",{{"type","object"},{"description","Applicability envelope: {domain, action_types, preconditions, source_families}"}}},
                {"seed_episode_ids",{{"type","array"},{"items",{{"type","integer"}}},{"description","Episode IDs that seeded this wisdom"}}},
                {"seed_surprise_ids",{{"type","array"},{"items",{{"type","integer"}}},{"description","Surprise event IDs"}}},
                {"seed_intervention_ids",{{"type","array"},{"items",{{"type","integer"}}},{"description","Intervention IDs"}}},
                {"seed_debt_ids",{{"type","array"},{"items",{{"type","integer"}}},{"description","Epistemic debt IDs"}}},
                {"ancestor_lineage_id",{{"type","integer"},{"description","Parent lineage ID if this is a fork/split"}}},
                {"derivation_relation",{{"type","string"},{"description","Relation to ancestor: supersedes|branches_from|narrows|splits_from"}}}
            }},{"required",{"wisdom_candidate_id","claim","envelope"}}},
            &FieldRpcHandler::registered_enroll_wisdom_lineage, handlers_["enroll_wisdom_lineage"]},

        {"transition_wisdom_lineage", "Manually transition a wisdom lineage state (Trusted/Watch/Inflamed/Demoted). Normally automatic — use for overrides.",
            {{"type","object"},{"properties",{
                {"lineage_id",{{"type","integer"},{"description","Lineage ID"}}},
                {"new_state",{{"type","integer"},{"description","0=Trusted 1=Watch 2=Inflamed 3=Demoted"}}},
                {"reason",{{"type","string"},{"description","Why this transition is happening"}}},
                {"rederive_task_id",{{"type","integer"},{"description","Task contract ID if opening re-derivation"}}}
            }},{"required",{"lineage_id","new_state"}}},
            &FieldRpcHandler::registered_transition_wisdom_lineage, handlers_["transition_wisdom_lineage"]},

        {"close_rederive", "Close a re-derivation contract for an Inflamed wisdom lineage. Actions: reaffirm (0), narrow (1), split (2), demote (3).",
            {{"type","object"},{"properties",{
                {"lineage_id",{{"type","integer"},{"description","Lineage ID"}}},
                {"action",{{"type","integer"},{"description","0=reaffirm 1=narrow 2=split 3=demote"}}},
                {"new_envelope",{{"type","object"},{"description","Narrowed applicability envelope (for action=narrow/split)"}}},
                {"fork_claim",{{"type","string"},{"description","Claim for the forked lineage (action=split)"}}},
                {"fork_lineage_id",{{"type","integer"},{"description","Pre-enrolled fork lineage ID (action=split)"}}}
            }},{"required",{"lineage_id","action"}}},
            &FieldRpcHandler::registered_close_rederive, handlers_["close_rederive"]},

        {"query_wisdom_lineages", "List wisdom lineages filtered by state and/or domain",
            {{"type","object"},{"properties",{
                {"state",{{"type","string"},{"description","Filter: trusted|watch|inflamed|demoted"}}},
                {"domain",{{"type","string"},{"description","Filter by domain"}}},
                {"limit",{{"type","integer"},{"description","Max results (default 50)"}}}
            }}},
            &FieldRpcHandler::registered_query_wisdom_lineages, handlers_["query_wisdom_lineages"]},

        {"get_wisdom_lineage", "Get full details of a wisdom lineage by ID, including challenger evidence and state history",
            {{"type","object"},{"properties",{
                {"lineage_id",{{"type","integer"},{"description","Lineage ID"}}}
            }},{"required",{"lineage_id"}}},
            &FieldRpcHandler::registered_get_wisdom_lineage, handlers_["get_wisdom_lineage"]},

        {"wisdom_lineage_stats", "Show wisdom homeostasis statistics — counts by state, mean staleness, support/contradiction mass totals",
            {{"type","object"}},
            &FieldRpcHandler::registered_wisdom_lineage_stats, handlers_["wisdom_lineage_stats"]},

        {"tick_lineage_staleness", "Manually trigger a staleness tick — grows staleness mass on lineages with no recent support. Normally called by the subconscious cycle.",
            {{"type","object"}},
            &FieldRpcHandler::registered_tick_lineage_staleness, handlers_["tick_lineage_staleness"]},

        {"lineage_expiry_check", "List Inflamed lineages whose re-derivation TTL has expired — these should be demoted or re-derived urgently",
            {{"type","object"}},
            &FieldRpcHandler::registered_lineage_expiry_check, handlers_["lineage_expiry_check"]},

        // ── Drift-memory tools ───────────────────────────────────────────────
    });
}

ToolResult FieldRpcHandler::registered_surprise_learning_stats(const json& p) { return tool_surprise_learning_stats(field_store_, p); }

ToolResult FieldRpcHandler::registered_upsert_wisdom_candidate(const json& p) { return tool_upsert_wisdom_candidate(field_store_, p); }

ToolResult FieldRpcHandler::registered_update_wisdom_lifecycle(const json& p) { return tool_update_wisdom_lifecycle(field_store_, p); }

ToolResult FieldRpcHandler::registered_query_wisdom_candidates(const json& p) { return tool_query_wisdom_candidates(field_store_, p); }

ToolResult FieldRpcHandler::registered_wisdom_promotion_stats(const json& p) { return tool_wisdom_promotion_stats(field_store_, p); }

ToolResult FieldRpcHandler::registered_attach_debt_evidence(const json& p) { return tool_attach_debt_evidence(field_store_, p); }

ToolResult FieldRpcHandler::registered_update_scorer_model(const json& p) { return tool_update_scorer_model(field_store_, p); }

ToolResult FieldRpcHandler::registered_learned_scorer_stats(const json& p) { return tool_learned_scorer_stats(field_store_, p); }

ToolResult FieldRpcHandler::registered_effective_scorer_weights(const json& p) { return tool_effective_scorer_weights(field_store_, p); }

ToolResult FieldRpcHandler::registered_start_intervention(const json& p) { return tool_start_intervention(field_store_, p); }

ToolResult FieldRpcHandler::registered_add_observation(const json& p) { return tool_add_observation(field_store_, p); }

ToolResult FieldRpcHandler::registered_close_intervention(const json& p) { return tool_close_intervention(field_store_, p); }

ToolResult FieldRpcHandler::registered_record_attribution(const json& p) { return tool_record_attribution(field_store_, p); }

ToolResult FieldRpcHandler::registered_query_interventions(const json& p) { return tool_query_interventions(field_store_, p); }

ToolResult FieldRpcHandler::registered_get_intervention(const json& p) { return tool_get_intervention(field_store_, p); }

ToolResult FieldRpcHandler::registered_intervention_stats(const json& p) { return tool_intervention_stats(field_store_, p); }

ToolResult FieldRpcHandler::registered_list_open_interventions(const json& p) { return tool_list_open_interventions(field_store_, p); }

ToolResult FieldRpcHandler::registered_register_task(const json& p) { return tool_register_task(field_store_, p); }

ToolResult FieldRpcHandler::registered_update_task(const json& p) { return tool_update_task(field_store_, p); }

ToolResult FieldRpcHandler::registered_add_delegation(const json& p) { return tool_add_delegation(field_store_, p); }

ToolResult FieldRpcHandler::registered_link_evidence(const json& p) { return tool_link_evidence(field_store_, p); }

ToolResult FieldRpcHandler::registered_add_probe(const json& p) { return tool_add_probe(field_store_, p); }

ToolResult FieldRpcHandler::registered_resolve_probe(const json& p) { return tool_resolve_probe(field_store_, p); }

ToolResult FieldRpcHandler::registered_set_criterion(const json& p) { return tool_set_criterion(field_store_, p); }

ToolResult FieldRpcHandler::registered_get_task(const json& p) { return tool_get_task(field_store_, p); }

ToolResult FieldRpcHandler::registered_query_tasks(const json& p) { return tool_query_tasks(field_store_, p); }

ToolResult FieldRpcHandler::registered_agent_protocol_stats(const json& p) { return tool_agent_protocol_stats(field_store_, p); }

ToolResult FieldRpcHandler::registered_enroll_wisdom_lineage(const json& p) { return tool_enroll_wisdom_lineage(field_store_, p); }

ToolResult FieldRpcHandler::registered_transition_wisdom_lineage(const json& p) { return tool_transition_wisdom_lineage(field_store_, p); }

ToolResult FieldRpcHandler::registered_close_rederive(const json& p) { return tool_close_rederive(field_store_, p); }

ToolResult FieldRpcHandler::registered_query_wisdom_lineages(const json& p) { return tool_query_wisdom_lineages(field_store_, p); }

ToolResult FieldRpcHandler::registered_get_wisdom_lineage(const json& p) { return tool_get_wisdom_lineage(field_store_, p); }

ToolResult FieldRpcHandler::registered_wisdom_lineage_stats(const json& p) { return tool_wisdom_lineage_stats(field_store_, p); }

ToolResult FieldRpcHandler::registered_tick_lineage_staleness(const json& p) { return tool_tick_lineage_staleness(field_store_, p); }

ToolResult FieldRpcHandler::registered_lineage_expiry_check(const json& p) { return tool_lineage_expiry_check(field_store_, p); }

} // namespace chitta
