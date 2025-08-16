#!/usr/bin/env python3
import argparse
import os
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[1]
BUILD_DIR = REPO_ROOT / "build"
TRY_BIN = BUILD_DIR / "tests" / "test_subscription_backpressure_long"
BLOCK_BIN = BUILD_DIR / "tests" / "test_subscription_blocking_depth"
SUMMARY = REPO_ROOT / "scripts" / "metrics_summary.py"
AGGREGATE = REPO_ROOT / "scripts" / "aggregate_bp_matrix.py"
REPORT = REPO_ROOT / "scripts" / "generate_bp_report.py"


def run_one(test_bin: Path, cap: int, duration_ms: int, burst: int, sleep_us: int, interval_ms: int, extra_env: dict, outdir: Path) -> int:
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
    # Extra env (e.g., consumer pacing for blocking mode)
    if extra_env:
        env.update({k: str(v) for k, v in extra_env.items()})

    if not test_bin.exists():
        print(f"Missing test binary: {test_bin}. Build the project first.")
        return 1

    # Run the test and capture metrics jsonl
    cmd = [str(test_bin)]
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


def parse_list(arg: str, cast=int):
    vals = []
    for part in arg.split(','):
        p = part.strip()
        if not p:
            continue
        vals.append(cast(p))
    return vals


def main():
    ap = argparse.ArgumentParser(description="Run backpressure capacity matrix and summarize metrics")
    ap.add_argument("--caps", type=str, default="1,2,4,8,16,32,64", help="comma-separated event channel capacities")
    ap.add_argument("--duration-ms", type=str, default="10000", help="single or comma-separated list")
    ap.add_argument("--burst", type=str, default="64", help="single or comma-separated list")
    ap.add_argument("--sleep-us", type=str, default="1000", help="single or comma-separated list")
    ap.add_argument("--interval-ms", type=int, default=200)
    ap.add_argument("--outdir", type=str, default=str(BUILD_DIR / "bp_matrix"))
    ap.add_argument("--modes", type=str, default="try,block", help="comma-separated modes: try, block")
    ap.add_argument("--consume-us", type=str, default="5000", help="single or comma-separated list (blocking mode)")
    ap.add_argument("--aggregate", action="store_true", help="run aggregator per combo")
    ap.add_argument("--report", action="store_true", help="generate REPORT.md per combo (implies --aggregate)")
    args = ap.parse_args()

    caps = [int(x) for x in args.caps.split(",") if x.strip()]
    outdir = Path(args.outdir)
    modes = [m.strip() for m in args.modes.split(",") if m.strip()]
    bursts = parse_list(args.burst, int)
    sleeps = parse_list(args.sleep_us, int)
    durations = parse_list(args.duration_ms, int)
    consumes = parse_list(args.consume_us, int)

    overall = 0
    for dur in durations:
        for burst in bursts:
            for sleep in sleeps:
                combo_name = f"dur_{dur}_burst_{burst}_sleep_{sleep}"
                combo_dir = outdir / combo_name
                for mode in modes:
                    if mode not in ("try", "block"):
                        print(f"Unknown mode: {mode}")
                        overall |= 1
                        continue
                    test_bin = TRY_BIN if mode == "try" else BLOCK_BIN
                    if mode == "block":
                        for consume in consumes:
                            mode_dir = combo_dir / f"mode_{mode}_consume_{consume}"
                            extra_env = {"BP_CONSUME_US": consume}
                            for cap in caps:
                                cap_out = mode_dir / f"cap_{cap}"
                                rc = run_one(test_bin, cap, dur, burst, sleep, args.interval_ms, extra_env, cap_out)
                                overall |= rc
                            if args.aggregate or args.report:
                                # aggregate and optional report for this combo+mode
                                agg_prefix = mode_dir / "summary"
                                acmd = [sys.executable, str(AGGREGATE), "--base", str(mode_dir), "--out-prefix", str(agg_prefix)]
                                subprocess.run(acmd, cwd=REPO_ROOT)
                                if args.report:
                                    rcmd = [sys.executable, str(REPORT), "--base", str(mode_dir), "--out", str(mode_dir / "REPORT.md")]
                                    subprocess.run(rcmd, cwd=REPO_ROOT)
                    else:
                        mode_dir = combo_dir / f"mode_{mode}"
                        extra_env = {}
                        for cap in caps:
                            cap_out = mode_dir / f"cap_{cap}"
                            rc = run_one(test_bin, cap, dur, burst, sleep, args.interval_ms, extra_env, cap_out)
                            overall |= rc
                        if args.aggregate or args.report:
                            agg_prefix = mode_dir / "summary"
                            acmd = [sys.executable, str(AGGREGATE), "--base", str(mode_dir), "--out-prefix", str(agg_prefix)]
                            subprocess.run(acmd, cwd=REPO_ROOT)
                            if args.report:
                                rcmd = [sys.executable, str(REPORT), "--base", str(mode_dir), "--out", str(mode_dir / "REPORT.md")]
                                subprocess.run(rcmd, cwd=REPO_ROOT)
    print("Matrix run complete. Artifacts in:", outdir)
    return overall


if __name__ == "__main__":
    sys.exit(main())
