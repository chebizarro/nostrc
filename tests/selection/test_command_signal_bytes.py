"""Emitted bytes survive real child-to-parent SIGTERM, not readiness delays."""
import hashlib
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock

import test_command_output_bytes as output_tests
from realization_support import Project, r

PAYLOAD = b"cancelling:\xff\x00\xfe\r\n"


def cancelling_body(payload_expression):
    # The flush completes BEFORE SIGTERM. No sleeps, parent-readiness assumption,
    # timeout fallback, or repeated attempts can substitute for byte retention.
    return ("import os,signal,sys; sys.stdout.buffer.write(" + payload_expression
            + "); sys.stdout.buffer.flush(); os.kill(os.getppid(),signal.SIGTERM); signal.pause()\n")


class DirectSignalCaptureTests(unittest.TestCase):
    def test_detached_child_cannot_be_reported_as_complete_tree_cleanup(self):
        import socket
        import subprocess
        with tempfile.TemporaryDirectory() as temporary:
            address = str(Path(temporary) / 'control')
            log = Path(temporary) / 'detached.log'
            child = ("import os,signal,socket,sys; signal.signal(signal.SIGTERM,signal.SIG_IGN); "
                     "s=socket.socket(socket.AF_UNIX); s.connect(sys.argv[1]); os.write(1,b'R'); "
                     "assert s.recv(4)==b'PING'; s.sendall(b'ALIVE'); assert s.recv(4)==b'STOP'; s.close()")
            body = ("import os,signal,subprocess,sys; "
                    f"p=subprocess.Popen([sys.executable,'-c',{child!r},{address!r}],start_new_session=True,stdout=subprocess.PIPE); "
                    "assert p.stdout.read(1)==b'R'; "
                    f"os.write(1,{PAYLOAD!r}); os.kill(os.getppid(),signal.SIGTERM); signal.pause()")
            sibling = subprocess.Popen([sys.executable, '-c', "import sys; print('ready',flush=True); sys.stdin.buffer.read()"],
                                       stdin=subprocess.PIPE, stdout=subprocess.PIPE)
            server = socket.socket(socket.AF_UNIX)
            connection = None
            try:
                self.assertEqual(sibling.stdout.readline(), b'ready\n')
                server.bind(address)
                server.listen(1)
                server.settimeout(20)
                with self.assertRaises(KeyboardInterrupt):
                    r._execute([sys.executable, '-c', body], log_path=log, timeout=20)
                connection, _ = server.accept()
                connection.settimeout(20)
                connection.sendall(b'PING')
                self.assertEqual(connection.recv(5), b'ALIVE')
                status = r._read(str(log) + '.process.json')
                self.assertTrue(status['direct_child_reaped'])
                self.assertFalse(status['cleanup_complete'])
                self.assertEqual(status['arbitrary_process_tree'], 'unverified')
                self.assertEqual(log.read_bytes(), PAYLOAD)
                self.assertIsNone(sibling.poll())
            finally:
                if connection is not None:
                    connection.sendall(b'STOP')
                    self.assertEqual(connection.recv(1), b'')
                    connection.close()
                server.close()
                sibling.stdin.close()
                sibling.wait(timeout=20)
                sibling.stdout.close()


    def test_sigterm_at_consumed_pipe_boundary_does_not_discard_bytes(self):
        actual_read = os.read
        def interrupt_after_consumption(descriptor, size):
            data = actual_read(descriptor, size)
            if data == PAYLOAD:
                # Reproduce the interruption window without relying on kernel
                # scheduling: bytes consumed, but not yet returned to the caller.
                os.kill(os.getpid(), r.signal.SIGTERM)
            return data
        with tempfile.TemporaryDirectory() as temporary:
            log = Path(temporary) / "consumed-boundary.log"
            body = f"import os,signal; os.write(1,{PAYLOAD!r}); signal.pause()"
            with mock.patch.object(r.os, "read", side_effect=interrupt_after_consumption):
                with self.assertRaises(KeyboardInterrupt):
                    r._execute([sys.executable, "-c", body], log_path=log, timeout=0.25)
            self.assertEqual(log.read_bytes(), PAYLOAD)
            status = r._read(str(log) + ".process.json")
            self.assertEqual(status["outcome"], "cancelled")
            self.assertTrue(status["direct_child_reaped"], status)
            self.assertFalse(status["cleanup_complete"], status)
            self.assertEqual(status["arbitrary_process_tree"], "unverified")
            self.assertEqual(status["output_sha256"], hashlib.sha256(PAYLOAD).hexdigest())

    def test_real_sigterm_retains_short_and_multipipe_output_with_or_without_timeout(self):
        large = b"prefix:" + bytes(range(256)) * 1024 + b":end\r\n"
        cases = ((None, PAYLOAD, repr(PAYLOAD)), (0.25, PAYLOAD, repr(PAYLOAD)),
                 (5, large, "b'prefix:'+bytes(range(256))*1024+b':end\\r\\n'"))
        with tempfile.TemporaryDirectory() as temporary:
            for index, (timeout, payload, expression) in enumerate(cases):
                with self.subTest(timeout=timeout, bytes=len(payload)):
                    log = Path(temporary) / f"capture-{index}.log"
                    with self.assertRaises(KeyboardInterrupt) as caught:
                        r._execute([sys.executable, "-c", cancelling_body(expression)], log_path=log, timeout=timeout)
                    self.assertIn("cancelled by signal", str(caught.exception))
                    status = r._read(str(log) + ".process.json")
                    self.assertEqual(log.read_bytes(), payload)
                    self.assertEqual(status["output_sha256"], hashlib.sha256(payload).hexdigest())
                    self.assertEqual(status["outcome"], "cancelled")
                    self.assertTrue(status["direct_child_reaped"], status)
                    self.assertFalse(status["cleanup_complete"], status)
                    self.assertEqual(status["arbitrary_process_tree"], "unverified")
                    self.assertEqual(caught.exception.execution_status, status)
                    with self.assertRaises(ChildProcessError):
                        os.waitpid(status["child_pid"], os.WNOHANG)


class FinalizedSignalReceiptTests(Project):
    def test_emitted_invalid_bytes_and_negative_receipt_survive_actual_sigterm(self):
        evidence_root = self.evidence
        for timeout in (None, 0.25):
            with self.subTest(timeout=timeout):
                self.evidence = evidence_root / str(timeout)
                resolved, audit = output_tests.CommandOutputBytesTests.ready_command(self, cancelling_body(repr(PAYLOAD)))
                with self.assertRaises(r.RealizationError) as caught:
                    r.run_resolved(resolved, self.build, self.evidence, input_audit=audit, build=False, timeout=timeout)
                directory = r._attempt_directory(resolved, self.evidence, 1)
                log = directory / "command-command.actual.log"
                receipt = r._read(directory / "receipt.json")
                status = r._read(str(log) + ".process.json")
                self.assertEqual(log.read_bytes(), PAYLOAD)
                self.assertEqual(status["output_sha256"], hashlib.sha256(PAYLOAD).hexdigest())
                self.assertEqual(status["outcome"], "cancelled")
                self.assertTrue(status["direct_child_reaped"], status)
                self.assertFalse(status["cleanup_complete"], status)
                self.assertEqual(status["arbitrary_process_tree"], "unverified")
                self.assertEqual(receipt["process_error"], status)
                self.assertEqual(receipt["outcome"], "cancelled")
                self.assertEqual(receipt["execution_outcome"], "cancelled")
                self.assertEqual(receipt["job_conclusion"], "failure")
                self.assertFalse(receipt["diagnostic_eligible"])
                self.assertFalse(receipt["complete_gate_eligible"])
                self.assertNotEqual(receipt["observed_executions"]["command.actual"], "passed")
                self.assertNotEqual(receipt["checks"]["command.actual"]["outcome"], "passed")
                self.assertIsInstance(caught.exception.__cause__, KeyboardInterrupt)
