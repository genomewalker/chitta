"""Behavior gates for screening isolation and prospective cohort policy."""

from __future__ import annotations

import json
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from audit import CAVEAT, TOOLS, audit_access, audit_trial, deny_rules
from freeze import classify
from isolation import probe
from runner import aggregate, prepare_hooks, private_environment


class AuditTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.work = self.root / "work"
        self.work.mkdir()
        self.policy = {
            "work": str(self.work),
            "roots": [str(self.work)],
            "home": str(self.root / "home"),
            "live_targets": ["/run/live.sock", "/home/kbd606/.claude", ":9481"],
        }

    def payload(self, tool, **args):
        return {"tool_name": tool, "tool_input": args, "cwd": str(self.work)}

    def test_fabricated_read_voids_trial(self):
        p = self.payload("Read", file_path="/home/kbd606/.claude/projects/a.jsonl")
        reason = audit_trial([{"event": "PreToolUse", "payload": p}], [], self.policy)
        self.assertIn("out-of-root Read", reason)

    def test_relative_symlink_glob_and_bash_escape(self):
        (self.work / "alias").symlink_to("/home/kbd606/.claude")
        for p in (
            self.payload("Read", file_path="../secret"),
            self.payload("Edit", file_path="alias/settings.json"),
            self.payload("Glob", pattern="/home/**"),
            self.payload("Bash", command="cat /etc/passwd"),
            self.payload("Bash", command="cat ../../secret"),
        ):
            with self.subTest(p=p):
                self.assertIsNotNone(audit_access(p, self.policy))
        self.assertIsNone(audit_access(self.payload("Read", file_path="answer"), self.policy))
        self.assertIsNone(audit_access(self.payload("Bash", command="cat answer"), self.policy))

    def test_live_socket_port_and_opaque_commands_void(self):
        for command in (
            "curl http://127.0.0.1:9481",
            "nc -U /run/live.sock",
            'python -c "import socket"',
            "cat $SECRET",
            "cd /tmp; cat secret",
        ):
            self.assertIsNotNone(audit_access(self.payload("Bash", command=command), self.policy))

    def test_missing_shadow_or_history_voids(self):
        p = self.payload("Bash", command="pwd")
        pre = {"event": "PreToolUse", "payload": p}
        post = {"event": "PostToolUse", "payload": p}
        self.assertIsNone(audit_trial([pre, post], ["pwd"], self.policy))
        self.assertIsNotNone(audit_trial([pre, post], [], self.policy))
        self.assertIsNotNone(audit_trial([post], ["pwd"], self.policy))

    def test_strict_refuses_without_bwrap(self):
        with patch("isolation.Path.is_file", return_value=False), patch("isolation.command") as cmd:
            self.assertIn("bubblewrap", probe())
            cmd.assert_not_called()

    def test_settings_and_environment(self):
        trial = self.root / "trial"
        live = {
            "home": "/home/kbd606",
            "mind": "/home/kbd606/.claude/mind",
            "socket": "/run/live.sock",
        }
        config = {
            "chitta_bin": "/bin/true",
            "chittad_bin": "/bin/true",
            "embed_model": "/bin/true",
            "isolation": "home-audit",
            "allowed_tools": TOOLS,
            "permission_mode": "dontAsk",
        }
        env = private_environment(trial, live, config)
        work = trial / "visible/work"
        work.mkdir()
        settings = prepare_hooks(trial / "visible", env, work, live, config)
        data = json.loads(settings.read_text())
        self.assertEqual(settings.parent, Path(env["CLAUDE_CONFIG_DIR"]))
        self.assertEqual(data["permissions"]["defaultMode"], "dontAsk")
        self.assertIn("Read(//home/kbd606/.claude/**)", data["permissions"]["deny"])
        for tool in TOOLS:
            self.assertTrue(
                any(rule.startswith(tool + "(") for rule in data["permissions"]["deny"])
            )
        self.assertIn("PreToolUse", data["hooks"])
        self.assertEqual(data["hooks"]["PostToolUse"][0]["matcher"], "*")
        mcp = json.loads((trial / "visible/mcp.json").read_text())["mcpServers"]
        self.assertEqual(list(mcp), ["chitta"])
        self.assertEqual(mcp["chitta"]["env"]["CHITTA_SOCKET_PATH"], env["CHITTA_SOCKET_PATH"])
        self.assertNotEqual(env["CHITTA_SOCKET_PATH"], live["socket"])
        self.assertTrue(deny_rules([str(work)], [live["mind"]]))

    def test_home_audit_invokes_model_without_bwrap_and_pins_permissions(self):
        import subprocess

        from runner import execute_agent

        visible = self.root / "visible"
        visible.mkdir()
        env = {"ANTHROPIC_API_KEY": "fixture-only", "CHITTA_BIN": "/bin/true"}
        config = {
            "isolation": "home-audit",
            "claude_bin": "/bin/true",
            "chitta_bin": "/bin/true",
            "model": "test-model",
            "max_turns": 2,
            "budget_usd": 1,
            "timeout_s": 2,
            "permission_mode": "dontAsk",
            "allowed_tools": TOOLS,
        }
        result = subprocess.CompletedProcess([], 0, "{}", "")
        with (
            patch("runner.command", return_value=result) as cmd,
            patch("runner.sandbox_command") as sb,
        ):
            execute_agent(
                {"prompt": "edit answer"},
                1,
                "A",
                self.work,
                visible,
                env,
                config,
                False,
                False,
                visible / "settings.json",
            )
            sb.assert_not_called()
            argv = cmd.call_args.args[0]
            self.assertEqual(argv[argv.index("--permission-mode") + 1], "dontAsk")
            self.assertEqual(argv[argv.index("--allowedTools") + 1], ",".join(TOOLS))
            self.assertIn("--strict-mcp-config", argv)
        with self.assertRaisesRegex(ValueError, "OS isolation"):
            execute_agent(
                {"prompt": "edit answer"},
                1,
                "A",
                self.work,
                visible,
                env,
                {**config, "isolation": "strict"},
                False,
                False,
                visible / "settings.json",
            )

    def test_audit_controls_and_wildcard_symlinks_are_rejected(self):
        self.policy["protected"] = [str(self.work / "settings.json")]
        p = self.payload("Write", file_path="settings.json", content="{}")
        self.assertIn("audit control", audit_access(p, self.policy))
        (self.work / "escape").symlink_to("/etc/passwd")
        self.assertIsNotNone(audit_access(self.payload("Bash", command="cat *"), self.policy))
        self.assertIsNotNone(audit_access(self.payload("Glob", pattern="*"), self.policy))

    def test_voided_is_missing_not_failure(self):
        events = [
            {
                "task": "t",
                "arm": arm,
                "trial": n,
                "success": 1,
                "exclusion_verified": True,
                "telemetry": {"complete": True, "cohort_injections": int(arm == "A")},
            }
            for arm in ("A", "B")
            for n in (1, 2)
        ]
        events[0]["voided_reason"] = "fabricated Read"
        result = aggregate([{"id": "t"}], events, 2, isolation="home-audit")
        self.assertEqual(result["rows"][0]["a"], 1)
        self.assertEqual(result["rows"][0]["a_observed"], 1)
        self.assertEqual(result["rows"][0]["a_missing"], 1)
        self.assertIsNone(result["delta"])
        self.assertIn(CAVEAT, result["verdict"])


class PopulationTests(unittest.TestCase):
    def test_explicit_signals_are_preserved_but_unlabelled_wisdom_is_not(self):
        for marker in ("artifact", "done"):
            row = classify({"id": "1", "kind": "signal", "content": f"[{marker}] saved"}, [], {})
            self.assertEqual(row["classification"], "excluded")
        row = classify({"id": "1", "kind": "wisdom"}, [], {})
        self.assertEqual(row["classification"], "unlabelled")

    def test_unexpected_native_kind_included_and_conflict_blocks(self):
        derived = {"subject": "1", "predicate": "derived_from", "object": "2"}
        row = classify({"id": "1", "kind": "signal"}, [derived], {"2": {"type": "episode"}})
        self.assertEqual(row["classification"], "included")
        self.assertEqual(row["writer"], "native_learning_unexpected_kind")
        row = classify({"id": "1", "kind": "signal"}, [derived], {"2": {"type": "wisdom"}})
        self.assertEqual(row["classification"], "unresolved")
