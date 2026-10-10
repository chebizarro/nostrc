#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Pure runner guard tests; never start a daemon or use real secrets."""
import importlib.util
import json
from pathlib import Path
import subprocess
import unittest
from unittest import mock

source = Path(__file__).with_name("run_live_epoch_bahia.py")
spec = importlib.util.spec_from_file_location("live_epoch_runner", source)
runner = importlib.util.module_from_spec(spec)
spec.loader.exec_module(runner)


def events(*actions, test=runner.LIVE_TEST):
    return "\n".join(json.dumps({"Action": action, "Test": test}) for action in actions)


class RunnerGuards(unittest.TestCase):
    def test_pubkey_secret_is_stdin_not_argv(self):
        fake = subprocess.CompletedProcess(["nak", "key", "public"], 0, "a" * 64 + "\n", "")
        with mock.patch.object(runner.subprocess, "run", return_value=fake) as call:
            self.assertEqual(runner.pubkey("synthetic-secret"), "a" * 64)
        self.assertEqual(call.call_args.args[0], ["nak", "key", "public"])
        self.assertEqual(call.call_args.kwargs["input"], "synthetic-secret\n")

    def test_exact_run_pass_required(self):
        runner.require_live_test_pass(events("run", "output", "pass"))
        for stream in ("", events("run"), events("skip"),
                       events("run", "skip"), events("run", "fail"),
                       events("pass"), events("run", "pass", "pass"),
                       events("run", "run", "pass"),
                       events("run", "pass", test="OtherTest"),
                       "not-json"):
            with self.subTest(stream=stream), self.assertRaises(RuntimeError):
                runner.require_live_test_pass(stream)


if __name__ == "__main__":
    unittest.main()
