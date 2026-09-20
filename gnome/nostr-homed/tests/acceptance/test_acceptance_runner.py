#!/usr/bin/env python3
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


HERE = Path(__file__).resolve().parent
RUNNER = HERE / "run_acceptance.py"
OPT_IN = "I_UNDERSTAND_THIS_RUNS_AGAINST_A_DISPOSABLE_LAB"


class AcceptanceRunnerTest(unittest.TestCase):
    def run_runner(self, *arguments, env=None):
        return subprocess.run(
            [sys.executable, str(RUNNER), *map(str, arguments)],
            text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            env=env, check=False,
        )

    def write_matrix(self, root):
        matrix = {
            "schema_version": 1,
            "matrix_id": "runner-test",
            "support_claim": "test-only",
            "roles": {
                "lab": {
                    "required": True,
                    "state": "ready",
                    "reason": "",
                    "required_pins": ["image_sha256", "version"],
                    "pins": {"image_sha256": "a" * 64, "version": "1.2.3"},
                }
            },
            "suites": {
                "gdm": {
                    "adapter_env": "NOSTR_ACCEPTANCE_GDM_ADAPTER",
                    "timeout_seconds": 30,
                    "required_roles": ["lab"],
                }
            },
            "cases": [{"id": "gdm.required", "suite": "gdm", "required": True}],
        }
        path = root / "matrix.json"
        path.write_text(json.dumps(matrix), encoding="utf-8")
        return path

    def write_adapter(self, root, body):
        path = root / "adapter.py"
        path.write_text("#!%s\n%s" % (sys.executable, body), encoding="utf-8")
        path.chmod(0o700)
        return path

    def ready_env(self, adapter):
        env = os.environ.copy()
        env["NOSTR_ACCEPTANCE_LAB_OPT_IN"] = OPT_IN
        env["NOSTR_ACCEPTANCE_GDM_ADAPTER"] = str(adapter)
        return env

    def test_repository_matrix_is_explicitly_unavailable(self):
        result = self.run_runner("--check-readiness")
        self.assertEqual(result.returncode, 2)
        self.assertEqual(json.loads(result.stdout)["status"], "unavailable")

    def test_run_requires_literal_opt_in(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            matrix = self.write_matrix(root)
            adapter = self.write_adapter(root, "raise SystemExit(99)\n")
            env = os.environ.copy()
            env["NOSTR_ACCEPTANCE_GDM_ADAPTER"] = str(adapter)
            result = self.run_runner("--run", "--suite", "gdm", "--matrix", matrix, "--evidence-dir", root / "evidence", env=env)
            self.assertEqual(result.returncode, 2)
            self.assertEqual(json.loads(result.stdout)["status"], "unavailable")

    def test_complete_adapter_result_passes(self):
        body = """import argparse, json
p=argparse.ArgumentParser(); p.add_argument('--matrix'); p.add_argument('--evidence-dir'); p.add_argument('--results'); a=p.parse_args()
e=__import__('pathlib').Path(a.evidence_dir); (e/'trace.txt').write_text('redacted trace', encoding='utf-8')
__import__('pathlib').Path(a.results).write_text(json.dumps({'schema_version':1,'suite':'gdm','results':[{'id':'gdm.required','status':'pass','evidence':['trace.txt']}]}), encoding='utf-8')
"""
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            matrix = self.write_matrix(root)
            adapter = self.write_adapter(root, body)
            result = self.run_runner("--run", "--suite", "gdm", "--matrix", matrix, "--evidence-dir", root / "evidence", env=self.ready_env(adapter))
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            report = json.loads((root / "evidence" / "acceptance-report.json").read_text())
            self.assertEqual(report["status"], "pass")
            self.assertEqual(report["results"][0]["evidence"][0]["path"], "trace.txt")

    def test_required_skip_is_unavailable_not_success(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            matrix = self.write_matrix(root)
            adapter = self.write_adapter(root, "raise SystemExit(77)\n")
            result = self.run_runner("--run", "--suite", "gdm", "--matrix", matrix, "--evidence-dir", root / "evidence", env=self.ready_env(adapter))
            self.assertEqual(result.returncode, 2)
            report = json.loads((root / "evidence" / "acceptance-report.json").read_text())
            self.assertEqual(report["results"][0]["status"], "skip")
            self.assertEqual(report["status"], "unavailable")

    def test_pass_without_evidence_fails(self):
        body = """import argparse, json
p=argparse.ArgumentParser(); p.add_argument('--matrix'); p.add_argument('--evidence-dir'); p.add_argument('--results'); a=p.parse_args()
__import__('pathlib').Path(a.results).write_text(json.dumps({'schema_version':1,'suite':'gdm','results':[{'id':'gdm.required','status':'pass','evidence':[]}]}), encoding='utf-8')
"""
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            matrix = self.write_matrix(root)
            adapter = self.write_adapter(root, body)
            result = self.run_runner("--run", "--suite", "gdm", "--matrix", matrix, "--evidence-dir", root / "evidence", env=self.ready_env(adapter))
            self.assertEqual(result.returncode, 1)
            report = json.loads((root / "evidence" / "acceptance-report.json").read_text())
            self.assertEqual(report["status"], "fail")

    def test_secret_canary_in_evidence_fails(self):
        body = """import argparse, json, os
p=argparse.ArgumentParser(); p.add_argument('--matrix'); p.add_argument('--evidence-dir'); p.add_argument('--results'); a=p.parse_args()
e=__import__('pathlib').Path(a.evidence_dir); (e/'leak.txt').write_text(os.environ['NOSTR_ACCEPTANCE_TEST_SECRET_CANARY'], encoding='utf-8')
__import__('pathlib').Path(a.results).write_text(json.dumps({'schema_version':1,'suite':'gdm','results':[{'id':'gdm.required','status':'pass','evidence':['leak.txt']}]}), encoding='utf-8')
"""
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            matrix = self.write_matrix(root)
            adapter = self.write_adapter(root, body)
            result = self.run_runner("--run", "--suite", "gdm", "--matrix", matrix, "--evidence-dir", root / "evidence", env=self.ready_env(adapter))
            self.assertEqual(result.returncode, 1)
            report = json.loads((root / "evidence" / "acceptance-report.json").read_text())
            self.assertEqual(report["status"], "fail")
            self.assertNotIn("NOSTR_ACCEPTANCE_TEST_SECRET_CANARY", result.stdout)


if __name__ == "__main__":
    unittest.main()
