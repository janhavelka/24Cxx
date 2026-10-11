"""Offline tests for HIL report evidence and host provenance."""
from __future__ import annotations

import hashlib
from importlib import metadata
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

import hil_artifacts as artifacts


class ProvenanceTests(unittest.TestCase):
    def test_hashes_exact_sources_and_preserves_argument_boundaries(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / "tools").mkdir()
            sources = {"hil_runner.py": b"runner\r\n", "hil_artifacts.py": b"helper\n", "hil_identity.py": b"identity\n"}
            for name, payload in sources.items():
                (root / "tools" / name).write_bytes(payload)
            argv = ["hil_runner.py", "--board", "a board with spaces", "--operator", "\u017dlu\u0165ou\u010dk\u00fd"]
            with patch.object(artifacts, "ROOT", root), \
                    patch.object(artifacts, "_git_text", side_effect=["a" * 40, ""]), \
                    patch.object(artifacts.metadata, "version", return_value="3.5"):
                result = artifacts.collect_provenance(argv)
            self.assertEqual(result["argv"], argv)
            argv.append("later mutation")
            self.assertNotEqual(result["argv"], argv)
            self.assertEqual(result["repository"], {"commit": "a" * 40, "worktree": "clean"})
            self.assertEqual(result["pyserial_version"], "3.5")
            self.assertEqual(result["sources"], {name: hashlib.sha256(payload).hexdigest() for name, payload in sources.items()})
            self.assertIn("does not identify", result["scope"])
            self.assertTrue(result["python"]["version"])
            self.assertTrue(result["os"])

    def test_no_git_no_serial_and_missing_source_remain_unknown(self):
        with tempfile.TemporaryDirectory() as temporary, \
                patch.object(artifacts, "ROOT", Path(temporary)), \
                patch.object(artifacts.subprocess, "run", side_effect=FileNotFoundError("git unavailable")), \
                patch.object(artifacts.metadata, "version", side_effect=metadata.PackageNotFoundError("pyserial")):
            result = artifacts.collect_provenance([])
        self.assertEqual(result["repository"], {"commit": "unknown", "worktree": "unknown"})
        self.assertEqual(result["pyserial_version"], "not installed")
        self.assertEqual(set(result["sources"].values()), {"unavailable"})

    def test_dirty_checkout_does_not_get_labeled_clean(self):
        with patch.object(artifacts, "_git_text", side_effect=["invalid commit", "?? new.py"]):
            result = artifacts.collect_provenance([])
        self.assertEqual(result["repository"], {"commit": "unknown", "worktree": "dirty"})

    def test_git_has_explicit_root_timeout_and_failed_output_is_not_evidence(self):
        completed = subprocess.CompletedProcess([], 0, "a" * 40 + "\n", "")
        with patch.object(artifacts.subprocess, "run", return_value=completed) as run:
            self.assertEqual(artifacts._git_text("rev-parse", "HEAD"), "a" * 40)
        self.assertEqual(run.call_args.args[0], ["git", "rev-parse", "HEAD"])
        self.assertEqual(run.call_args.kwargs["cwd"], artifacts.ROOT)
        self.assertEqual(run.call_args.kwargs["timeout"], artifacts.GIT_TIMEOUT_SECONDS)
        for response in (subprocess.CompletedProcess([], 1, "false evidence", ""),
                         subprocess.CompletedProcess([], 0, "x" * (artifacts.MAX_GIT_TEXT + 1), "")):
            with patch.object(artifacts.subprocess, "run", return_value=response):
                self.assertIsNone(artifacts._git_text("status"))
        with patch.object(artifacts.subprocess, "run", side_effect=subprocess.TimeoutExpired("git", 3)):
            self.assertIsNone(artifacts._git_text("status"))


class MarkdownTests(unittest.TestCase):
    def test_interrupted_report_preserves_failure_and_explicit_evidence_limits(self):
        report = {
            "outcome": "FAIL", "model": "zetta", "address": 0x50, "allow_write": True,
            "serial_executed": True, "capture_error": "disk full", "electrical_evidence": "NOT_RUN",
            "results": [{"command": "rw_suite 0 32 confirm", "group": "scratch", "outcome": "FAIL", "reason": "readback mismatch"},
                        {"command": "read 0 256", "group": "full-check", "outcome": "NOT_RUN"}],
            "soak": {"outcome": "NOT_RUN", "requested_s": 60},
        }
        rendered = artifacts.render_markdown(report)
        self.assertIn("Overall outcome: **FAIL**", rendered)
        self.assertIn("zetta / 0x50", rendered)
        self.assertIn("1 FAIL, 1 NOT", rendered)
        self.assertIn("readback mismatch", rendered)
        self.assertIn("disk full", rendered)
        self.assertIn("Electrical evidence", rendered)
        self.assertIn("scratch-preimage.bin", rendered)
        self.assertIn("does not automatically restore, retry, or reconnect", rendered)
        self.assertIn("Host provenance was not recorded", rendered)

    def test_untrusted_strings_cannot_add_rows_links_or_html(self):
        hostile = "bad | cell\n# forged\n<script>alert(1)</script>[click](https://bad/)`code`"
        report = {"outcome": "INCONCLUSIVE", "operator": hostile, "firmware": hostile,
                  "results": [{"command": hostile, "reason": hostile}],
                  "configuration": {hostile: hostile}, "provenance": {"argv": [hostile]}}
        rendered = artifacts.render_markdown(report)
        self.assertNotIn("<script>", rendered)
        self.assertNotIn("[click](", rendered)
        self.assertNotIn("\n# forged", rendered)
        self.assertNotIn("bad | cell", rendered)
        self.assertIn("bad &#124; cell", rendered)
        self.assertIn("&lt;script&gt;", rendered)
        self.assertIn("&#96;code&#96;", rendered)

    def test_dry_run_and_partial_report_never_infer_pass_or_executed_steps(self):
        for report in ({}, {"outcome": "NOT_RUN", "plan": [{"command": "read 0 256"}]}):
            rendered = artifacts.render_markdown(report)
            self.assertNotIn("**PASS**", rendered)
            self.assertIn("a plan alone is not execution evidence", rendered)
            self.assertIn("No soak result was recorded", rendered)

    def test_soak_and_provenance_are_rendered_without_recalculating_verdict(self):
        report = {"outcome": "INCONCLUSIVE", "configuration": {"timeout": 10, "soak_interval": 1},
                  "firmware_identity": {"outcome": "MATCHED", "observed": {"commit": "b" * 40}},
                  "soak": {"outcome": "INCONCLUSIVE", "requested_s": 3600, "elapsed_s": 22,
                           "cycles": 2, "duration_met": False, "worst_command": "crc 0 256",
                           "per_command": {"read": {"count": 2, "max_s": 0.2}},
                           "failure": {"command": "read 0 256", "reason": "disconnect"}},
                  "provenance": {"scope": "Host only", "argv": ["--timeout", "10"],
                                 "repository": {"commit": "a" * 40, "worktree": "dirty"}}}
        rendered = artifacts.render_markdown(report)
        for evidence in ("INCONCLUSIVE", "Requested duration met | False", "disconnect", "crc 0 256",
                         "Host only", "a" * 40, "dirty", "Effective settings", "--timeout",
                         "MATCHED", "b" * 40, "Per-command timing aggregates"):
            self.assertIn(evidence, rendered)


if __name__ == "__main__":
    unittest.main()
