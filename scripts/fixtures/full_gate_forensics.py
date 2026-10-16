#!/usr/bin/env python3
"""Exercise test-full-gate.sh's real failure traps; retain all negative evidence.

Run explicitly: python3 scripts/fixtures/full_gate_forensics.py -v
Injected failures verify recurrence capture, not the cause of the lost .24 trace.
"""
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import stat
import subprocess
import tempfile
import unittest


class FullGateForensics(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        root = Path(__file__).resolve().parents[2]
        cls.evidence = Path(tempfile.mkdtemp(prefix="nostrc-full-gate-forensics-"))
        cls.source = cls.evidence / "source"
        inputs = [
            "scripts/test-full-gate.sh", "scripts/full-gate.sh",
            "scripts/install-hooks.sh", "scripts/linux-gate.sh",
            "scripts/linux-gate-smoke.sh", "scripts/sanitizer-gate-ci.py",
            "scripts/test_selection.py", "scripts/test_selection_realization.py",
            "tests/selection/inventory.json", ".github/workflows/groundhog-ci.yml",
        ]
        hashes = {}
        for name in inputs:
            dest = cls.source / name
            dest.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(root / name, dest)
            hashes[name] = hashlib.sha256(dest.read_bytes()).hexdigest()
        (cls.evidence / "input-hashes.json").write_text(json.dumps(hashes, indent=2))
        cls.original = (cls.source / inputs[0]).read_text()
        print(f"Full-gate forensics evidence (including intentional failures): {cls.evidence}", flush=True)

    def invoke(self, name, needle=None, replacement=None, status=1):
        script = self.source / f"scripts/test-full-gate-{name}.sh"
        content = self.original
        if needle == "grep -q '^BEADS$' \"$tmp/trace\"":
            # The same assertion also exists in the drained-stdin phase.
            prefix = 'phase=baseline\nrun_hook > "$tmp/output" 2>&1\n'
            needle, replacement = prefix + needle, prefix + replacement
        if needle is not None:
            self.assertEqual(content.count(needle), 1)
            content = content.replace(needle, replacement, 1)
        script.write_text(content)
        result = subprocess.run(
            ["bash", str(script)], cwd=self.source,
            env=dict(os.environ, PYTHONDONTWRITEBYTECODE="1"),
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=180,
        )
        (self.evidence / f"{name}.log").write_bytes(result.stdout)
        self.assertEqual(result.returncode, status, result.stdout.decode(errors="replace"))
        output = result.stdout.decode()
        match = re.search(r"test-full-gate fixture: (.+) \(retained on failure;", output)
        self.assertIsNotNone(match, output)
        fixture = Path(match[1])
        (self.evidence / f"{name}.json").write_text(json.dumps({
            "exit": result.returncode, "fixture": str(fixture),
            "injected": replacement, "script": str(script),
        }, indent=2))
        if status == 0:
            self.assertIn("full-gate tests passed", output)
            self.assertNotIn("private fixture retained:", output)
            self.assertFalse(fixture.exists())
            return
        self.assertEqual(stat.S_IMODE(fixture.stat().st_mode), 0o700)
        self.assertEqual((fixture / "failing-fixture.sh").read_text(), content)
        failure = (fixture / "failure.log").read_text()
        self.assertIn(f"status={status} source={script}", failure)
        self.assertIn("phase=baseline", failure)
        self.assertIn("functions=record_failure", failure)
        self.assertIn("call_lines=", failure)
        self.assertTrue((fixture / "output").stat().st_size)
        self.assertTrue((fixture / "trace").stat().st_size)
        trees = subprocess.check_output(
            ["git", "-C", str(fixture / "main"), "worktree", "list", "--porcelain"], text=True)
        paths = [Path(line[9:]) for line in trees.splitlines() if line.startswith("worktree ")]
        self.assertEqual(len(paths), 2)
        self.assertTrue(all(p.resolve().is_relative_to(fixture.resolve()) for p in paths))
        return failure, content

    def test_assertion_command_and_line(self):
        command = "grep -q '^FORENSICS_MISSING_MARKER$' \"$tmp/trace\""
        failure, content = self.invoke("assertion", "grep -q '^BEADS$' \"$tmp/trace\"", command)
        line = content.splitlines().index(command) + 1
        self.assertIn("event=ERR status=1", failure)
        self.assertIn(f"bash_line={line} phase=baseline\ncommand={command}\n", failure)

    def test_explicit_nonzero_exit(self):
        failure, _ = self.invoke("exit", "grep -q '^BEADS$' \"$tmp/trace\"", "exit 37", status=37)
        self.assertIn("event=EXIT status=37", failure)
        self.assertIn("command=exit 37\n", failure)

    def test_helper_stack(self):
        failure, _ = self.invoke("helper", "grep -q '^BEADS$' \"$tmp/trace\"", "absent '^BEADS$' \"$tmp/trace\"")
        self.assertIn("command=exit 1\n", failure)
        self.assertIn("absent", failure)

    def test_redirected_hook_failure(self):
        failure, content = self.invoke("redirected", 'run_hook > "$tmp/output" 2>&1',
                                       'run_hook build > "$tmp/output" 2>&1')
        caller = content.splitlines().index('run_hook build > "$tmp/output" 2>&1') + 1
        self.assertIn('"$repo/.beads/hooks/pre-push"', failure)
        self.assertIn("functions=record_failure run_hook main", failure)
        self.assertRegex(failure, rf"call_lines=\d+ {caller} 0")

    def test_success_cleans_fixture(self):
        self.invoke("success", status=0)


if __name__ == "__main__":
    unittest.main()
