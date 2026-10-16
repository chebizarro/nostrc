#!/usr/bin/env python3
"""Exercise test-full-gate.sh's real failure traps; retain all negative evidence.

Run explicitly: python3 scripts/fixtures/full_gate_forensics.py -v
Injected failures verify recurrence capture, not the cause of the lost .24 trace.
There is no generic native kill deadline: arbitrary shell descendants cannot be
safely cancelled here. The timeout case uses an explicit EOF-cooperative fixture
that reaps its one known child before Bash can finalize its ERR/EXIT evidence.
"""
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import stat
import subprocess
import sys
import tempfile
import unittest


def cooperative_timeout_helper():
    """Closed test fixture, not an arbitrary-process custody implementation."""
    receipt = Path(os.environ["FORENSICS_CHILD_RECEIPT"])
    cancel = int(os.environ["FORENSICS_CANCEL_FD"])
    ready = int(os.environ["FORENSICS_READY_FD"])
    child = subprocess.Popen(
        [sys.executable, "-c", "import sys; sys.stdin.buffer.read(); print('owned child observed EOF', flush=True)"],
        stdin=subprocess.PIPE,
    )
    state = {"owned_child_pid": child.pid, "owned_child_reaped": False}
    try:
        receipt.write_text(json.dumps(state))
        os.write(ready, b"R")
        os.close(ready)
        os.read(cancel, 1)  # The observer closes its writer on the deadline.
        os.close(cancel)
    finally:
        child.stdin.close()  # EOF releases only this explicitly owned child.
        state["owned_child_exit"] = child.wait()
        state["owned_child_reaped"] = True
        receipt.write_text(json.dumps(state))
    return 124


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
            "scripts/fixtures/full_gate_forensics.py",
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

    def invoke(self, name, needle=None, replacement=None, status=1, *, cooperative_timeout=False):
        if cooperative_timeout:
            self.assertEqual(replacement, 'python3 "$FORENSICS_TIMEOUT_HELPER" --cooperative-timeout-helper')
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
        log = self.evidence / f"{name}.log"
        metadata = self.evidence / f"{name}.json"
        state = {"state": "starting", "exit": None, "fixture": None,
                 "injected": replacement, "script": str(script),
                 "cooperative_timeout": cooperative_timeout, "timeout_observed": False,
                 "deadline_seconds": 0.05 if cooperative_timeout else None,
                 "timeout_policy": "fixture_cooperative_only" if cooperative_timeout else "no_generic_native_deadline"}

        def save_state():
            temporary = metadata.with_suffix(".json.tmp")
            temporary.write_text(json.dumps(state, indent=2))
            temporary.replace(metadata)

        channels = {}
        env = dict(os.environ, PYTHONDONTWRITEBYTECODE="1")
        if cooperative_timeout:
            channels["ready_read"], channels["ready_write"] = os.pipe()
            channels["cancel_read"], channels["cancel_write"] = os.pipe()
            env.update(FORENSICS_READY_FD=str(channels["ready_write"]),
                       FORENSICS_CANCEL_FD=str(channels["cancel_read"]),
                       FORENSICS_CHILD_RECEIPT=str(self.evidence / f"{name}-child.json"),
                       FORENSICS_TIMEOUT_HELPER=str(self.source / "scripts/fixtures/full_gate_forensics.py"))
        save_state()  # Must exist even if launch or an assertion fails.
        process = None
        try:
            with log.open("wb") as stream:
                process = subprocess.Popen(
                    ["bash", str(script)], cwd=self.source, env=env,
                    stdout=stream, stderr=subprocess.STDOUT,
                    pass_fds=tuple(channels[k] for k in ("ready_write", "cancel_read") if k in channels),
                )
                state.update(state="running", fixture_pid=process.pid)
                save_state()
                if cooperative_timeout:
                    for key in ("ready_write", "cancel_read"):
                        os.close(channels.pop(key))
                    self.assertEqual(os.read(channels["ready_read"], 1), b"R")
                    try:
                        process.wait(timeout=0.05)
                    except subprocess.TimeoutExpired:
                        state.update(state="timeout_cancelling", timeout_observed=True)
                        save_state()
                    finally:
                        # No signal or numeric PID/group kill. The known helper
                        # releases/reaps its child, then exits through Bash traps.
                        os.close(channels.pop("cancel_write"))
                state["exit"] = process.wait()
                state["state"] = "exited"
        except BaseException as error:
            state.update(state="observer_error", error=repr(error))
            raise
        finally:
            for fd in channels.values():
                os.close(fd)
            if cooperative_timeout and process is not None:
                # Closing the control pipe is the fixture's cancellation API.
                state["exit"] = process.wait()
            output = log.read_bytes().decode(errors="replace") if log.exists() else ""
            match = re.search(r"test-full-gate fixture: (.+) \(retained on failure;", output)
            if match:
                state["fixture"] = match[1]
            save_state()
        self.assertEqual(state["exit"], status, output)
        self.assertIsNotNone(match, output)
        fixture = Path(match[1])
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

    def test_cooperative_timeout_finalizes_and_reaps_owned_child(self):
        failure, _ = self.invoke("timeout", "grep -q '^BEADS$' \"$tmp/trace\"",
            'python3 "$FORENSICS_TIMEOUT_HELPER" --cooperative-timeout-helper',
            status=124, cooperative_timeout=True)
        state = json.loads((self.evidence / "timeout.json").read_text())
        child = json.loads((self.evidence / "timeout-child.json").read_text())
        self.assertTrue(state["timeout_observed"])
        self.assertEqual(state["state"], "exited")
        self.assertTrue(child["owned_child_reaped"])
        self.assertEqual(child["owned_child_exit"], 0)
        self.assertIn("owned child observed EOF", (self.evidence / "timeout.log").read_text())
        self.assertIn("event=EXIT status=124", failure)

    def test_uncooperative_timeout_refused_before_launch(self):
        with self.assertRaises(AssertionError):
            self.invoke("unowned-timeout", "grep -q '^BEADS$' \"$tmp/trace\"",
                        "sleep 9999 & wait", status=124, cooperative_timeout=True)
        self.assertFalse((self.evidence / "unowned-timeout.json").exists())
        self.assertFalse((self.source / "scripts/test-full-gate-unowned-timeout.sh").exists())

    def test_unexpected_exit_still_has_case_log_and_metadata(self):
        with self.assertRaises(AssertionError):
            self.invoke("unexpected", "grep -q '^BEADS$' \"$tmp/trace\"", "exit 19")
        state = json.loads((self.evidence / "unexpected.json").read_text())
        self.assertEqual(state["exit"], 19)
        self.assertTrue((self.evidence / "unexpected.log").stat().st_size)
        self.assertTrue((Path(state["fixture"]) / "failure.log").is_file())

    def test_success_cleans_fixture(self):
        self.invoke("success", status=0)


if __name__ == "__main__":
    if sys.argv[1:] == ["--cooperative-timeout-helper"]:
        sys.exit(cooperative_timeout_helper())
    unittest.main()
