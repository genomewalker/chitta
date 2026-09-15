"""Behavioral tests for provenance, freezes, exclusions, telemetry and isolation."""

from __future__ import annotations

import sys
import tempfile
import unittest
from copy import deepcopy
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from audit import TOOLS
from common import digest, git, initial_hashes, seal, task_worktree
from freeze import (
    classify,
    cohort_report,
    enumerate_memories,
    task_membership,
    task_payload,
    validate_freeze,
    validate_task,
)
from runner import aggregate, guard_environment, prepare_task_arm, telemetry, verify_exclusion


def triplet(mid, predicate, obj):
    return {"subject": str(mid), "predicate": predicate, "object": str(obj)}


class ClassificationTests(unittest.TestCase):
    def classify(self, kind="wisdom", trips=(), parents=None):
        return classify({"id": "18446744073709551614", "kind": kind}, list(trips), parents or {})

    def test_queue_requires_source_not_tag(self):
        mid = "18446744073709551614"
        row = self.classify(trips=[triplet(mid, "source", "distillation")])
        self.assertEqual((row["classification"], row["writer"]), ("included", "queue_distillation"))
        self.assertEqual(
            self.classify(trips=[triplet(mid, "source", "")])["classification"], "unlabelled"
        )
        row = self.classify(trips=[triplet(mid, "tagged", "distillation")])
        self.assertEqual(row["classification"], "unlabelled")

    def test_labelled_bash_distillation_with_episode_lineage_is_automatic(self):
        mid = "18446744073709551614"
        row = self.classify(
            trips=[triplet(mid, "source", "distillation"), triplet(mid, "derived_from", "20")],
            parents={"20": {"type": "episode"}},
        )
        self.assertEqual((row["classification"], row["writer"]), ("included", "queue_distillation"))

    def test_native_all_kinds_and_value_facts(self):
        mid = "18446744073709551614"
        for kind in ("wisdom", "belief", "preference", "milestone", "operational"):
            with self.subTest(kind=kind):
                row = self.classify(
                    kind, [triplet(mid, "derived_from", "20")], {"20": {"type": "episode"}}
                )
                self.assertEqual(row["classification"], "included")
                self.assertTrue(row["parents"])
        self.assertEqual(self.classify("operational")["classification"], "unlabelled")

    def test_preserved_corrections_episodes_and_explicit(self):
        mid = "18446744073709551614"
        for kind in ("correction", "episode"):
            self.assertEqual(
                self.classify(kind, [triplet(mid, "source", "distillation")])["classification"],
                "excluded",
            )
        self.assertEqual(
            self.classify(trips=[triplet(mid, "source", "mcp_tool")])["classification"], "excluded"
        )

    def test_ingester_missing_parent_and_conflicts(self):
        mid = "18446744073709551614"
        derived = triplet(mid, "derived_from", 20)
        self.assertEqual(self.classify(trips=[derived])["classification"], "unresolved")
        row = self.classify(
            trips=[derived, triplet(mid, "source", "mcp_tool")], parents={"20": {"type": "episode"}}
        )
        self.assertEqual(row["classification"], "unresolved")
        row = self.classify(trips=[derived, triplet(mid, "ingested_from", "readme")])
        self.assertEqual(row["writer"], "ingester")

    def test_capped_pagination_runs_to_empty(self):
        class RPC:
            def call(self, name, **args):
                return {
                    "memories": [
                        {"id": args["offset"] + i} for i in range(min(100, 203 - args["offset"]))
                    ]
                }

        self.assertEqual(len(enumerate_memories(RPC(), "project:cc-soul")), 203)


class CutInspectionTests(unittest.TestCase):
    def test_cut_has_manifest_and_never_enumerates_or_classifies(self):
        store = {"selection": {}, "files": {}, "fully_hashed": False}
        with (
            patch("freeze.RPC") as rpc,
            patch("freeze.family", return_value=store),
            patch.object(Path, "read_bytes", return_value=b"daemon\0--path\0/tmp/probe\0"),
        ):
            rpc.return_value.call.return_value = {"pid": 1}
            with patch("freeze.now_ms", return_value=100):
                result = cohort_report("/tmp/socket", "project:cc-soul", preview=True)
            self.assertEqual(result["cut_timestamp_ms"], 100)
            self.assertEqual(result["cut_store"], store)
            self.assertNotIn("evidence", result)
            rpc.return_value.call.assert_called_once_with("health_check")

    def test_pre_cut_only_queries_timestamp_and_post_cut_requires_metadata(self):
        store = {"selection": {}, "files": {}}
        rows = {
            "1": {"id": "1", "kind": "wisdom", "content": "baseline"},
            "2": {"id": "2", "kind": "wisdom", "content": "unlabelled"},
        }

        def call(name, **args):
            if name == "health_check":
                return {"pid": 1}
            if name == "memory_provenance":
                return {"meta": {"id": args["id"], "created_at_ms": args["id"]}}
            self.assertEqual((name, args), ("query_graph", {"subject": "2"}))
            return {"triplets": []}

        with (
            patch("freeze.RPC") as rpc,
            patch("freeze.family", return_value=store),
            patch("freeze.enumerate_memories", return_value=rows) as inventory,
            patch.object(Path, "is_file", return_value=True),
            patch.object(
                Path, "read_bytes", return_value=b"daemon\0--path\0/tmp/probe\0--no-distill\0"
            ),
        ):
            rpc.return_value.call.side_effect = call
            result = cohort_report(
                "/tmp/socket",
                "project:cc-soul",
                preview=False,
                cut={"cut_timestamp_ms": 1, "cut_store": store},
            )
            self.assertEqual(result["inventory_scope"], "all")
            self.assertTrue(all(call.args[1] == "" for call in inventory.call_args_list))
            self.assertEqual(result["baseline_count"], 1)
            self.assertNotIn("classification", result["evidence"][0])
            self.assertEqual(result["unlabelled"], [{"id": "2", "kind": "wisdom"}])
            rpc.return_value.call.side_effect = (
                lambda name, **args: {"pid": 1}
                if name == "health_check"
                else {"meta": {"id": args["id"]}}
            )
            with self.assertRaisesRegex(ValueError, "missing creation timestamp"):
                cohort_report(
                    "/tmp/socket",
                    "project:cc-soul",
                    preview=False,
                    cut={"cut_timestamp_ms": 1, "cut_store": store},
                )


class TaskTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="learning-test-")
        self.root = Path(self.tmp.name)
        self.repo = self.root / "repo"
        self.repo.mkdir()
        git(self.repo, "init", "--quiet", "-b", "fixture")
        git(self.repo, "config", "user.email", "test@example.invalid")
        git(self.repo, "config", "user.name", "Test")
        (self.repo / "answer").write_text("bad")
        git(self.repo, "add", ".")
        git(self.repo, "commit", "-qm", "initial")
        initial = git(self.repo, "rev-parse", "HEAD")
        (self.repo / "answer").write_text("good")
        git(self.repo, "commit", "-qam", "good")
        good = git(self.repo, "rev-parse", "HEAD")
        self.task = {
            "id": "task1",
            "prompt": "Repair answer",
            "repo": str(self.repo),
            "cwd_sha": initial,
            "known_good": good,
            "initial_state_hashes": initial_hashes(self.repo, initial),
            "grader": {
                "command": ["/bin/sh", ".learning-grader/check.sh"],
                "files": {"check.sh": 'test "$(cat answer)" = good\n'},
            },
            "transcript_sha256": "a" * 64,
            "prompt_timestamp_ms": 2,
            "selection_note": "next self-contained graded prompt",
            "selection_rule": "next_eligible_graded_prompt",
            "recall_inspected": False,
            "dependencies": [{"path": "/bin/sh", "sha256": digest("/bin/sh")}],
        }
        self.task["validation"] = validate_task(self.task)

    def tearDown(self):
        self.tmp.cleanup()

    def panel(self):
        tasks = [deepcopy(self.task) for _ in range(20)]
        for n, task in enumerate(tasks):
            task["id"] = f"t{n}"
            task["prompt_timestamp_ms"] += n
            task["validation"]["payload_sha256"] = seal(task_payload(task))
        cohort = {
            "schema": 2,
            "inventory_scope": "all",
            "evidence": [],
            "cut_store": {"fully_hashed": True},
            "unresolved": [],
            "preview": False,
            "enumeration_stable": True,
            "realm": "project:cc-soul",
            "store": {"fully_hashed": True},
            "cut_timestamp_ms": 1,
        }
        config = {
            "model": "pinned-model",
            "claude_version": "1",
            "max_turns": 1,
            "budget_usd": 1,
            "timeout_s": 1,
            "claude_bin": "/bin/true",
            "chitta_bin": "/bin/true",
            "chittad_bin": "/bin/true",
            "embed_model": "/bin/true",
            "seed": 1,
            "allowed_tools": TOOLS,
            "permission_mode": "dontAsk",
        }
        return tasks, cohort, config

    def test_freeze_rejects_unlabelled_postcut_by_id_kind(self):
        tasks, cohort, config = self.panel()
        unknown = classify({"id": "8", "kind": "wisdom", "created_at_ms": 2}, [], {})
        derived = [triplet("9", "derived_from", "10")]
        included = classify(
            {"id": "9", "kind": "alias", "created_at_ms": 3}, derived, {"10": {"kind": "episode"}}
        )
        cohort.update(evidence=[unknown, included])
        with self.assertRaisesRegex(ValueError, r"8 \(wisdom\)"):
            validate_freeze(tasks, cohort, config)
        cohort["evidence"][0] = {"id": "8", "kind": "wisdom", "created_at_ms": 1}
        validate_freeze(tasks, cohort, config)
        included["parents"]["10"]["kind"] = "wisdom"
        with self.assertRaisesRegex(ValueError, "unresolved or inconsistent"):
            validate_freeze(tasks, cohort, config)

    def test_per_task_boundaries_and_tampered_membership(self):
        tasks, cohort, config = self.panel()
        cohort["evidence"] = [{"id": "1", "kind": "wisdom", "created_at_ms": 1}]
        for mid, ts, source in (
            ("2", 2, "distillation"),
            ("3", 3, "distillation"),
            ("4", 3, "mcp_tool"),
        ):
            cohort["evidence"].append(
                classify(
                    {"id": mid, "kind": "wisdom", "created_at_ms": ts},
                    [triplet(mid, "source", source)],
                    {},
                )
            )
        self.assertEqual(
            task_membership(tasks[0], cohort),
            {"eligible_cohort_ids": ["2"], "future_ids": ["3", "4"]},
        )
        self.assertEqual(
            task_membership(tasks[1], cohort), {"eligible_cohort_ids": ["2", "3"], "future_ids": []}
        )
        for key in ("eligible_cohort_ids", "future_ids"):
            tasks[0][key] = []
            with self.assertRaisesRegex(ValueError, key + " mismatch"):
                validate_freeze(tasks, cohort, config)
            tasks[0].pop(key)
        for ts in (0, 1):
            with self.assertRaisesRegex(ValueError, "after cut"):
                task_membership({"id": "t", "prompt_timestamp_ms": ts}, cohort)

    def test_real_pre_fail_post_pass(self):
        self.assertNotEqual(self.task["validation"]["initial"]["exit_code"], 0)
        self.assertEqual(self.task["validation"]["known_good"]["exit_code"], 0)

    def test_always_pass_or_fail_grader_refused(self):
        for body in ("exit 0", "exit 1"):
            task = deepcopy(self.task)
            task["grader"]["files"]["check.sh"] = body
            with self.assertRaises(ValueError):
                validate_task(task)

    def test_missing_known_good_refused(self):
        task = deepcopy(self.task)
        task.pop("known_good")
        with self.assertRaisesRegex(ValueError, "known_good"):
            validate_task(task)

    def test_freeze_refusals(self):
        tasks, cohort, config = self.panel()
        validate_freeze(tasks, cohort, config)
        cases = [
            ("all-store", lambda t, c, p: c.update(inventory_scope="project:cc-soul")),
            ("unresolved", lambda t, c, p: c.update(unresolved=["99"])),
            ("preview", lambda t, c, p: c.update(preview=True)),
            ("prospective", lambda t, c, p: c.update(cut_timestamp_ms=10)),
            ("validation", lambda t, c, p: t[0].pop("validation")),
            ("validation", lambda t, c, p: t[0].update(prompt="Changed after validation")),
            ("runner pin", lambda t, c, p: p.pop("model")),
        ]
        for reason, mutate in cases:
            with self.subTest(reason=reason):
                ts, cs, ps = deepcopy((tasks, cohort, config))
                mutate(ts, cs, ps)
                with self.assertRaisesRegex(ValueError, reason):
                    validate_freeze(ts, cs, ps)

    def test_no_future_git_objects_and_grader_absent(self):
        parent = self.root / "trial"
        parent.mkdir()
        with task_worktree(self.task, parent) as work:
            self.assertFalse((work / ".learning-grader").exists())
            self.assertFalse((parent / "objects.git/objects/info/alternates").exists())
            from common import command

            probe = command(
                ["git", "-C", work, "cat-file", "-e", self.task["known_good"]], check=False
            )
            self.assertNotEqual(probe.returncode, 0)
            self.assertEqual(git(work, "rev-parse", "HEAD"), self.task["cwd_sha"])


class ExclusionTests(unittest.TestCase):
    def lanes(self):
        return {
            "hybrid": {"results": []},
            "graph": {"results": []},
            "correction": {"results": []},
            "correction_check": {"matches": []},
        }

    def test_task_arms_preserve_baseline_explicit_and_exclude_all_future(self):
        class RPC:
            def __init__(self):
                self.ids = {"baseline", "explicit", "2", "3", "4"}

            def call(self, name, **args):
                if name == "get":
                    return {"id": args["id"]} if args["id"] in self.ids else None
                if name == "forget":
                    self.ids.remove(args["id"])
                    return {}
                return {"results": [{"id": mid} for mid in self.ids]}

        task = {"prompt": "repair", "eligible_cohort_ids": ["2"], "future_ids": ["3", "4"]}
        for arm, retained in (
            ("A", {"baseline", "explicit", "2"}),
            ("B", {"baseline", "explicit"}),
        ):
            with tempfile.TemporaryDirectory() as td:
                rpc = RPC()
                result = prepare_task_arm(rpc, task, arm, Path(td))
                self.assertTrue(result["future_exclusion_verified"])
                self.assertEqual(rpc.ids, retained)
                self.assertTrue((Path(td) / "future-exclusion.json").is_file())

    def test_all_lanes_and_by_id_required(self):
        verify_exclusion(["42"], {"42": None}, self.lanes())
        with self.assertRaisesRegex(ValueError, "still resolves"):
            verify_exclusion(["42"], {"42": {"id": "42"}}, self.lanes())
        for lane in self.lanes():
            lanes = self.lanes()
            lanes[lane] = {"matches": [{"memory_id": "42"}]}
            with self.subTest(lane=lane), self.assertRaisesRegex(ValueError, lane):
                verify_exclusion(["42"], {"42": None}, lanes)
        with self.assertRaisesRegex(ValueError, "incomplete"):
            verify_exclusion(["42"], {}, self.lanes())
        with self.assertRaisesRegex(ValueError, "missing"):
            verify_exclusion(["42"], {"42": None}, {})

    def test_transport_error_is_not_absence(self):
        with self.assertRaises(ValueError):
            verify_exclusion(["42"], {"42": {"error": "timeout"}}, self.lanes())


class OutcomeTests(unittest.TestCase):
    def data(self):
        tasks = [{"id": f"t{i}"} for i in range(20)]
        events = [
            {
                "task": t["id"],
                "trial": n,
                "arm": arm,
                "success": int(arm == "A" and i < 3),
                "exclusion_verified": arm == "B",
                "future_exclusion_verified": True,
                "telemetry": {
                    "complete": True,
                    "cohort_injections": int(arm == "A"),
                    "total_injections": 1,
                    "empty_turns": 0,
                    "recall_ms": [1, 3],
                    "hook_ms": [10],
                    "lane_failures": 0,
                },
            }
            for i, t in enumerate(tasks)
            for n in range(1, 4)
            for arm in ("A", "B")
        ]
        return tasks, events

    def test_exposure_is_required_for_each_task_and_future_in_neither_arm(self):
        tasks, events = self.data()
        for event in events:
            if event["task"] == "t7":
                event["telemetry"]["cohort_injections"] = 0
        self.assertIn(
            "task t7: A has no confirmed cohort exposure", aggregate(tasks, events, 3)["verdict"]
        )
        tasks, events = self.data()
        events[0]["future_telemetry"] = {"cohort_injections": 1}
        self.assertIn("future exclusion/exposure failed", aggregate(tasks, events, 3)["verdict"])

    def test_three_trial_threshold_nine_net_successes(self):
        tasks, events = self.data()
        summary = aggregate(tasks, events, 3)
        self.assertEqual(summary["delta"], 3)
        self.assertEqual(summary["net_successes"], 9)
        self.assertTrue(summary["verdict"].startswith("RETAIN"))
        events[0]["success"] = 0
        self.assertTrue(aggregate(tasks, events, 3)["verdict"].startswith("RETIRE"))

    def test_invalid_runs_never_verdict(self):
        tasks, events = self.data()
        variants = [events[:-1], events + [events[0]]]
        for field, value in (("error", "timeout"), ("success", None)):
            other = deepcopy(events)
            other[0][field] = value
            variants.append(other)
        other = deepcopy(events)
        other[1]["telemetry"]["cohort_injections"] = 1
        variants.append(other)
        other = deepcopy(events)
        for event in other:
            event["telemetry"]["cohort_injections"] = 0
        variants.append(other)
        for variant in variants:
            self.assertTrue(aggregate(tasks, variant, 3)["verdict"].startswith("NO VERDICT"))
        for args in ({"dry_run": True}, {"isolation_ok": False}, {"unresolved": ["1"]}):
            self.assertTrue(aggregate(tasks, events, 3, **args)["verdict"].startswith("NO VERDICT"))

    def test_hook_output_ledger_join_and_genuine_empty(self):
        hook = {
            "event": "UserPromptSubmit",
            "session_id": "s",
            "started_ms": 10,
            "ended_ms": 20,
            "exit_code": 0,
            "stdout": '{"context":"[kw]#42 [90%] wisdom"}',
        }
        ledger = {
            "event": "injected",
            "session_id": "s",
            "ts": 15,
            "ids": ["42"],
            "lane_ms": {"kw": 2},
            "lane_timeout": {"kw": False},
            "hook_ms": 3,
        }
        self.assertEqual(telemetry([hook], [ledger], ["42"])["cohort_injections"], 1)
        bad = deepcopy(ledger)
        bad["ids"] = ["41"]
        with self.assertRaisesRegex(ValueError, "disagree"):
            telemetry([hook], [bad], ["42"])
        with self.assertRaisesRegex(ValueError, "join"):
            telemetry([hook], [], ["42"])
        hook["stdout"] = "{}"
        ledger.update(event="recall_empty", ids=[])
        self.assertEqual(telemetry([hook], [ledger], ["42"])["empty_turns"], 1)


class LeakageTests(unittest.TestCase):
    def test_live_home_socket_and_symlinks_refused(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            live = {
                "home": str(root / "live"),
                "socket": str(root / "live.sock"),
                "mind": str(root / "live/mind"),
            }
            private = root / "private"
            private.mkdir()
            env = {
                "HOME": str(private / "home"),
                "CHITTA_SOCKET_PATH": str(private / "sock"),
                "CHITTA_DB_PATH": str(private / "db"),
                "CHITTA_QUEUE": str(private / "queue"),
                "XDG_RUNTIME_DIR": str(private / "run"),
                "CHITTA_REALM": "project:cc-soul",
                "CHITTA_UTILITY_RECALL": "0",
            }
            for key in (
                "XDG_CONFIG_HOME",
                "XDG_DATA_HOME",
                "XDG_CACHE_HOME",
                "XDG_STATE_HOME",
                "CLAUDE_CONFIG_DIR",
                "TMPDIR",
            ):
                env[key] = str(private / key)
            guard_environment(env, live, private)
            for key, path in (("HOME", live["home"]), ("CHITTA_SOCKET_PATH", live["socket"])):
                bad = {**env, key: path}
                with self.assertRaises(ValueError):
                    guard_environment(bad, live, private)
                alias = private / ("alias-" + key)
                alias.symlink_to(path)
                bad[key] = str(alias)
                with self.assertRaises(ValueError):
                    guard_environment(bad, live, private)
            for alias in ("CHITTA_HEADLESS", "CC_SOUL_HEADLESS"):
                with self.assertRaises(ValueError):
                    guard_environment({**env, alias: "1"}, live, private)


class BrokerTests(unittest.TestCase):
    def test_host_file_and_execution_tools_denied(self):
        from broker import allowed_request

        for name in (
            "read_transcript",
            "predicate_run",
            "import_soul",
            "read_symbol",
            "compact_wal",
            "forget",
        ):
            self.assertFalse(allowed_request({"method": "tools/call", "params": {"name": name}}))
        self.assertTrue(allowed_request({"method": "tools/call", "params": {"name": "recall"}}))
        self.assertFalse(allowed_request({"method": "eval", "params": {"name": "recall"}}))

    def test_truncated_header_cannot_fabricate_an_id(self):
        hook = {
            "event": "UserPromptSubmit",
            "session_id": "s",
            "started_ms": 10,
            "ended_ms": 20,
            "exit_code": 0,
            "stdout": "[kw]#42 [90%] fact\n[hyb]#4",
        }
        row = {
            "event": "injected",
            "session_id": "s",
            "ts": 15,
            "ids": ["42"],
            "lane_ms": {"kw": 2},
            "lane_timeout": {"kw": False},
            "hook_ms": 3,
        }
        self.assertEqual(telemetry([hook], [row], ["42"])["cohort_injections"], 1)

    def test_runtime_cannot_mount_live_parent_or_future_repo(self):
        from common import validate_runtime_roots

        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            live = root / "live"
            live.mkdir()
            with self.assertRaises(ValueError):
                validate_runtime_roots([root], [live])
