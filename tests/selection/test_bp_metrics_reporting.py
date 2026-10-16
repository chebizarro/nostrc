#!/usr/bin/env python3
"""BP reporting regressions; run with python3 tests/selection/test_bp_metrics_reporting.py.

tests/fixtures/bp_metrics_reporting.jsonl contains two consecutive, unmodified JSON
lines emitted by test_subscription_backpressure_long on Ubuntu 24.04/aarch64,
GCC 13, Release + NOSTR_ENABLE_METRICS=ON, source a478070509569c26917dd5fb13a548b1f68eade9.
Captured 2026-10-16 from the full regular matrix (cap=1, duration-ms=15000,
burst=128, sleep-us=500, interval-ms=200). This fixture is parser evidence,
not a replacement for running workloads or full-job acceptance.
"""
import contextlib
import csv
import io
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

SCRIPTS = Path(__file__).resolve().parents[2] / "scripts"
sys.path.insert(0, str(SCRIPTS))
import run_bp_matrix as runner

FIXTURE = Path(__file__).resolve().parents[1] / "fixtures" / "bp_metrics_reporting.jsonl"
COMBO = "dur_15000_burst_128_sleep_500"


class BPReportingTests(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.root = Path(tmp.name)
        self.mode = self.root / COMBO / "mode_try"
        self.cap = self.mode / "cap_1"
        self.cap.mkdir(parents=True)
        shutil.copyfile(FIXTURE, self.cap / "metrics.jsonl")

    def cli(self, script, *args):
        return subprocess.run([sys.executable, str(SCRIPTS / script), *map(str, args)],
                              capture_output=True, text=True)

    def summarize(self):
        return self.cli("metrics_summary.py", self.cap / "metrics.jsonl",
                        "--interval", "0.2", "--csv-prefix", self.cap / "cap")

    def aggregate(self, base):
        return self.cli("aggregate_bp_matrix.py", "--base", base,
                        "--out-prefix", self.root / "summary")

    def test_real_emission_summary_root_aggregation_and_report(self):
        result = self.summarize()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        snapshots = [json.loads(line) for line in FIXTURE.read_text().splitlines()]
        expected_rate = (snapshots[1]["counters"]["bp_events_generated"] -
                         snapshots[0]["counters"]["bp_events_generated"]) / 0.2
        summaries = []
        # Root, direct combo, and direct mode calls must keep the same CSV contract.
        for base in (self.root, self.mode.parent, self.mode):
            with self.subTest(base=base):
                result = self.aggregate(base)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                summary = self.root / "summary_matrix_summary.csv"
                summaries.append(summary.read_bytes())
                with summary.open() as f:
                    rows = list(csv.DictReader(f))
                self.assertEqual(len(rows), 1)
                row = rows[0]
                self.assertEqual((row["mode"], row["cap"]), ("try", "1"))
                self.assertGreater(expected_rate, 0)
                self.assertEqual(float(row["bp_events_generated_avg"]), expected_rate)
                counters = snapshots[-1]["counters"]
                self.assertEqual(float(row["send_avg_depth"]),
                                 counters["go_chan_send_depth_sum"] /
                                 counters["go_chan_send_depth_samples"])
                self.assertEqual(int(row["go_chan_recv_wait_p99_ns"]),
                                 snapshots[-1]["histograms"]["go_chan_recv_wait_ns"]["p99_ns"])
        self.assertEqual(summaries, [summaries[0]] * 3)
        result = self.cli("generate_bp_report.py", "--base", self.root)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("Modes: try", (self.root / "REPORT.md").read_text())
        self.assertTrue((self.root / "summary_send_fail_ratio.svg").exists())
        self.assertTrue((self.root / "summary_correlations.csv").exists())

    def test_missing_summaries_do_not_aggregate(self):
        result = self.aggregate(self.root)
        self.assertEqual(result.returncode, 1)
        self.assertIn("No rows aggregated", result.stdout)

    def test_no_metrics_and_malformed_metrics_fail_summary(self):
        for text in ("test: OK\n", "{invalid JSON\n"):
            with self.subTest(text=text):
                (self.cap / "metrics.jsonl").write_text(text)
                self.assertEqual(self.summarize().returncode, 1)
                self.assertEqual(self.aggregate(self.root).returncode, 1)

    def test_single_snapshot_cannot_fabricate_interval_rates(self):
        (self.cap / "metrics.jsonl").write_text(FIXTURE.read_text().splitlines()[0] + "\n")
        self.assertEqual(self.summarize().returncode, 0)
        self.assertEqual(self.aggregate(self.root).returncode, 1)

    def test_multiple_workload_combinations_are_not_pooled(self):
        self.assertEqual(self.summarize().returncode, 0)
        (self.root / "dur_30000_burst_128_sleep_500").mkdir()
        result = self.aggregate(self.root)
        self.assertEqual(result.returncode, 1)
        self.assertIn("aggregate each combo directory separately", result.stdout)
        self.assertFalse((self.root / "summary_matrix_summary.csv").exists())

    def test_runner_propagates_workload_summary_aggregate_and_report_failures(self):
        # These mocks test exit-code plumbing only; they never produce metrics.
        cases = [(0, 0, 0, 0, 2), (7, 0, 0, 7, 2),
                 (0, 9, 0, 9, 1), (0, 0, 11, 11, 2)]
        for mode in ("try", "block"):
            for run_rc, agg_rc, report_rc, expected_rc, calls in cases:
                with self.subTest(mode=mode, run_rc=run_rc, agg_rc=agg_rc, report_rc=report_rc):
                    args = ["run_bp_matrix.py", "--caps", "1", "--modes", mode,
                            "--outdir", str(self.root), "--report"]
                    with patch.object(sys, "argv", args), \
                         patch.object(runner, "run_one", return_value=run_rc), \
                         patch.object(runner.subprocess, "run", side_effect=[
                             subprocess.CompletedProcess([], agg_rc),
                             subprocess.CompletedProcess([], report_rc)]) as run, \
                         contextlib.redirect_stdout(io.StringIO()):
                        self.assertEqual(runner.main(), expected_rc)
                        self.assertEqual(run.call_count, calls)

    def test_run_one_preserves_binary_and_summary_failures(self):
        binary = self.root / "bp-test"
        binary.touch()
        for binary_rc, summary_rc, expected_rc, calls in ((7, 0, 7, 1), (0, 9, 9, 2)):
            with self.subTest(binary_rc=binary_rc, summary_rc=summary_rc):
                with patch.object(runner, "BUILD_DIR", self.root), \
                     patch.object(runner.subprocess, "run", side_effect=[
                         subprocess.CompletedProcess([], binary_rc),
                         subprocess.CompletedProcess([], summary_rc)]) as run, \
                     contextlib.redirect_stdout(io.StringIO()):
                    rc = runner.run_one(binary, 1, 15000, 128, 500, 200, {}, self.root / "run")
                    self.assertEqual(rc, expected_rc)
                    self.assertEqual(run.call_count, calls)


if __name__ == "__main__":
    unittest.main()
