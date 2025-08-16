#!/usr/bin/env python3
import argparse
import os
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[1]
BUILD_DIR = REPO_ROOT / "build"
TEST_BIN = BUILD_DIR / "tests" / "test_subscription_backpressure_long"
SUMMARY = REPO_ROOT / "scripts" / "metrics_summary.py"


def run_one(cap: int, duration_ms: int, burst: int, sleep_us: int, interval_ms: int, outdir: Path) -> int:
    outdir.mkdir(parents=True, exist_ok=True)
    jsonl = outdir / "metrics.jsonl"
    env = os.environ.copy()
    # Metrics dump configuration
    env["NOSTR_METRICS_DUMP"] = "1"
    env["NOSTR_METRICS_INTERVAL_MS"] = str(interval_ms)
    env["NOSTR_METRICS_DUMP_ON_EXIT"] = "1"
    env["NOSTR_TEST_MODE"] = "1"
    # Subscription channel capacities (events only; others default to 1 unless overridden)
    env["NOSTR_SUB_EVENTS_CAP"] = str(cap)
    # Stress knobs
    env["BP_DURATION_MS"] = str(duration_ms)
    env["BP_BURST"] = str(burst)
    env["BP_SLEEP_US"] = str(sleep_us)

    if not TEST_BIN.exists():
        print(f"Missing test binary: {TEST_BIN}. Build the project first.")
        return 1

    # Run the test and capture metrics jsonl
    cmd = [str(TEST_BIN)]
    with open(jsonl, "w") as f:
        print(f"[run] cap={cap} duration_ms={duration_ms} burst={burst} sleep_us={sleep_us} interval_ms={interval_ms}")
        rc = subprocess.run(cmd, cwd=BUILD_DIR, env=env, stdout=f, stderr=subprocess.STDOUT).returncode
        if rc != 0:
            print(f"Test failed for cap={cap} (rc={rc})")
            return rc

    # Summarize
    interval_s = max(interval_ms, 1) / 1000.0
    csv_prefix = outdir / "cap"
    svg_prefix = outdir / "cap"
    sum_cmd = [sys.executable, str(SUMMARY), str(jsonl), "--interval", str(interval_s), "--csv-prefix", str(csv_prefix), "--svg-prefix", str(svg_prefix)]
    rc2 = subprocess.run(sum_cmd, cwd=REPO_ROOT).returncode
    if rc2 != 0:
        print(f"Summary failed for cap={cap}")
    return rc2


def main():
    ap = argparse.ArgumentParser(description="Run backpressure capacity matrix and summarize metrics")
    ap.add_argument("--caps", type=str, default="1,2,4,8,16,32,64", help="comma-separated event channel capacities")
    ap.add_argument("--duration-ms", type=int, default=10000)
    ap.add_argument("--burst", type=int, default=64)
    ap.add_argument("--sleep-us", type=int, default=1000)
    ap.add_argument("--interval-ms", type=int, default=200)
    ap.add_argument("--outdir", type=str, default=str(BUILD_DIR / "bp_matrix"))
    args = ap.parse_args()

    caps = [int(x) for x in args.caps.split(",") if x.strip()]
    outdir = Path(args.outdir)

    overall = 0
    for cap in caps:
        cap_out = outdir / f"cap_{cap}"
        rc = run_one(cap, args.duration_ms, args.burst, args.sleep_us, args.interval_ms, cap_out)
        overall |= rc
    print("Matrix run complete. Artifacts in:", outdir)
    return overall


if __name__ == "__main__":
    sys.exit(main())
