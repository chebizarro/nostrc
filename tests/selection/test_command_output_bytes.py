"""Real byte-producing commands, owned-child cleanup and finalized evidence.

These miniature source-only commands are not native/profile/live acceptance.
"""
import hashlib
import os
import subprocess
import sys
from unittest import mock
import test_command_execution as commands
from realization_support import Project, r

PAYLOAD = b"valid:\xc3\xa9\r\n\xff\x00\xfe\x80\r\n"


class CommandOutputBytesTests(Project):
    def ready_command(self, body):
        commands.CommandExecutionTests.command(self, body, required_evidence=[])
        self.inventory["checks"] = {"command.actual": self.inventory["checks"]["command.actual"]}
        self.profile["configure_args"] = []
        return commands.CommandExecutionTests.ready(self, source_only=True)

    def read_evidence(self, resolved):
        directory = r._attempt_directory(resolved, self.evidence, 1)
        log = directory / "command-command.actual.log"
        receipt = r._read(directory / "receipt.json")
        status = r._read(str(log) + ".process.json")
        self.assertEqual(log.read_bytes(), PAYLOAD)
        self.assertEqual(status["output_sha256"], hashlib.sha256(PAYLOAD).hexdigest())
        self.assertEqual(status["output_encoding"], "raw bytes")
        self.assertNotEqual(receipt["outcome"], "running")
        return receipt, status

    def assert_reaped(self, status):
        if os.name != "nt":
            with self.assertRaises(ChildProcessError):
                os.waitpid(status["child_pid"], os.WNOHANG)

    def test_immediate_exit_raw_output_is_lossless_and_uses_actual_exit_status(self):
        for code in (0, 7, 77):
            with self.subTest(exit_code=code):
                body = f"import os,sys; os.write(1,{PAYLOAD!r}); sys.exit({code})\n"
                resolved, audit = self.ready_command(body)
                # No failure cleanup/group signalling for an already exited
                # command, including the former Darwin zombie-group race.
                with mock.patch.object(r.os, "killpg", wraps=r.os.killpg) as group_signal:
                    raw = r.run_resolved(resolved, self.build, self.evidence, input_audit=audit, build=False)
                    group_signal.assert_not_called()
                receipt, status = self.read_evidence(resolved)
                expected = "passed" if code == 0 else "failed"
                self.assertEqual(raw["outcome"], expected)
                self.assertEqual(receipt["outcome"], expected)
                self.assertEqual(receipt["job_conclusion"], "success" if code == 0 else "failure")
                self.assertEqual(receipt["observed_executions"], {"command.actual": expected})
                self.assertEqual(receipt["checks"]["command.actual"]["exit_code"], code)
                self.assertEqual(status["exit_code"], code)
                self.assertEqual(status["outcome"], expected)
                self.assert_reaped(status)

    def test_closed_invalid_output_live_child_times_out_with_final_failed_receipt(self):
        body = f"import os,signal; os.write(1,{PAYLOAD!r}); os.close(1); os.close(2); signal.pause()\n"
        resolved, audit = self.ready_command(body)
        with self.assertRaises(r.RealizationError) as caught:
            r.run_resolved(resolved, self.build, self.evidence, input_audit=audit, build=False, timeout=0.5)
        self.assertIsInstance(caught.exception.__cause__, subprocess.TimeoutExpired)
        receipt, status = self.read_evidence(resolved)
        self.assertEqual(receipt["outcome"], "failed")
        self.assertEqual(receipt["job_conclusion"], "failure")
        self.assertFalse(receipt["diagnostic_eligible"])
        self.assertFalse(receipt["complete_gate_eligible"])
        self.assertTrue(receipt["checks"])
        self.assertNotEqual(receipt["observed_executions"]["command.actual"], "passed")
        self.assertTrue(status["error"].startswith("TimeoutExpired:"), status)
        self.assertTrue(status["direct_child_reaped"], status)
        self.assertFalse(status["cleanup_complete"], status)
        self.assertEqual(status["arbitrary_process_tree"], "unverified")
        self.assertEqual(receipt["process_error"], status)
        self.assert_reaped(status)

    def test_group_permission_error_cannot_mask_timeout_or_discard_raw_bytes(self):
        body = f"import os,signal; os.write(1,{PAYLOAD!r}); os.close(1); os.close(2); signal.pause()\n"
        resolved, audit = self.ready_command(body)
        # Fault only the group signal, never process success: the real owned
        # child is terminated/reaped by Popen's direct-child fallback.
        with mock.patch.object(r.os, "killpg", side_effect=PermissionError("injected group cleanup refusal")):
            with self.assertRaises(r.RealizationError) as caught:
                r.run_resolved(resolved, self.build, self.evidence, input_audit=audit, build=False, timeout=0.5)
        self.assertIsInstance(caught.exception.__cause__, subprocess.TimeoutExpired)
        receipt, status = self.read_evidence(resolved)
        self.assertEqual(receipt["outcome"], "failed")
        self.assertEqual(receipt["job_conclusion"], "failure")
        self.assertTrue(status["error"].startswith("TimeoutExpired:"), status)
        self.assertFalse(status["cleanup_complete"])
        self.assertTrue(any("PermissionError" in entry["error"] for entry in status["cleanup_errors"]))
        self.assertEqual(receipt["process_error"], status)
        self.assert_reaped(status)

    def test_text_interface_round_trips_invalid_utf8_without_losing_crlf_or_nul(self):
        log = self.root.parent / "raw-output.log"
        result = r._execute([sys.executable, "-c", f"import os; os.write(1,{PAYLOAD!r})"], log_path=log)
        self.assertEqual(result.returncode, 0)
        self.assertEqual(result.stdout.encode("utf-8", errors="surrogateescape"), PAYLOAD)
        self.assertEqual(log.read_bytes(), PAYLOAD)

    def test_unexpected_text_consumer_error_still_finalizes_attempt(self):
        resolved, audit = self.ready_command(f"import os; os.write(1,{PAYLOAD!r})\n")
        actual_execute = r._execute
        def strict_text_consumer(*args, **kwargs):
            result = actual_execute(*args, **kwargs)
            directory = r._attempt_directory(resolved, self.evidence, 1)
            checkpoint = r._read(directory / "receipt.json")
            self.assertEqual(checkpoint["outcome"], "running")
            self.assertEqual(checkpoint["job_conclusion"], "running")
            result.stdout.encode("utf-8")  # deliberately faulty caller, real subprocess already ran
            return result
        with mock.patch.object(r, "_execute", side_effect=strict_text_consumer):
            with self.assertRaises(r.RealizationError) as caught:
                r.run_resolved(resolved, self.build, self.evidence, input_audit=audit, build=False)
        self.assertIsInstance(caught.exception.__cause__, UnicodeError)
        receipt, status = self.read_evidence(resolved)
        self.assertEqual(receipt["outcome"], "failed")
        self.assertEqual(receipt["job_conclusion"], "failure")
        self.assertTrue(receipt["checks"])
        self.assertEqual(status["exit_code"], 0)  # actual process success is not caller success
        error_log = r._attempt_directory(resolved, self.evidence, 1) / "execution-error.txt"
        self.assertIn("UnicodeEncodeError", error_log.read_text())
        self.assert_reaped(status)
