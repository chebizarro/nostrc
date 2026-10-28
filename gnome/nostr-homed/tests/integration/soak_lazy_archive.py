#!/usr/bin/env python3
"""Bounded read-only FUSE Archive soak. Standard-library-only, Linux only.

The caller owns setup and teardown. This process never mounts, unmounts, starts
services, deletes data, or changes network state. Each timed probe is a fresh
subprocess so a hung FUSE read cannot hang the metrics collector.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import signal
import subprocess
import sys
import time


def probe(path: Path, gio: bool = False) -> str:
    # Exercise the directory lookup as well as the file read on each probe.
    with os.scandir(path.parent) as entries:
        if path.name not in {entry.name for entry in entries}:
            raise FileNotFoundError(path)
    if gio:
        # GIO is the closest bounded file-manager path without scripting
        # Nautilus UI. Run inside the active GNOME session bus.
        subprocess.run(["gio", "info", str(path)], check=True,
                       stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        while chunk := stream.read(1024 * 1024):
            digest.update(chunk)
    return digest.hexdigest()


def mount_id(mountpoint: Path) -> int | None:
    # mountinfo is procfs metadata: unlike stat/ismount on a dead FUSE
    # mount, reading it cannot enter the FUSE request path.
    target = str(mountpoint)
    for line in Path("/proc/self/mountinfo").read_text().splitlines():
        before, separator, after = line.partition(" - ")
        if not separator:
            continue
        fields = before.split()
        fs_fields = after.split()
        if len(fields) < 5 or not fs_fields or fs_fields[0] != "fuse.porthome":
            continue
        encoded = fields[4]
        path = re.sub(r"\\([0-7]{3})", lambda m: chr(int(m.group(1), 8)), encoded)
        if path == target:
            return int(fields[0])
    return None


def tree_bytes(root: Path) -> int:
    total = 0
    if root.exists():
        for base, _, names in os.walk(root):
            for name in names:
                try:
                    total += (Path(base) / name).stat().st_size
                except OSError:
                    pass
    return total


def process_rss_kib(pid: int | None) -> int | None:
    if pid is None:
        return None
    try:
        for line in Path(f"/proc/{pid}/status").read_text().splitlines():
            if line.startswith("VmRSS:"):
                return int(line.split()[1])
    except (OSError, ValueError):
        pass
    return None


def status_data(path: Path | None) -> dict:
    if path is None:
        return {}
    try:
        raw = json.loads(path.read_text())
        return raw.get("fuse", raw)
    except (OSError, ValueError, TypeError):
        return {}


def percentile(values: list[float], pct: float) -> float | None:
    if not values:
        return None
    ordered = sorted(values)
    return round(ordered[min(len(ordered) - 1, int((len(ordered) - 1) * pct))], 3)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mountpoint", type=Path, required=True)
    parser.add_argument("--sample", required=True, help="path relative to mountpoint")
    parser.add_argument("--sha256", required=True, help="expected plaintext hash")
    parser.add_argument("--duration-seconds", type=int, required=True)
    parser.add_argument("--interval-seconds", type=float, default=30.0)
    parser.add_argument("--read-timeout-seconds", type=float, default=20.0)
    parser.add_argument("--cache-dir", type=Path, required=True)
    parser.add_argument("--status", type=Path)
    parser.add_argument("--pid", type=int, help="nostr-home-fuse MainPID")
    parser.add_argument("--session-pid", type=int, help="gnome-session MainPID")
    parser.add_argument("--gio", action="store_true", help="also exercise GIO in a GNOME session")
    parser.add_argument("--report", type=Path, required=True)
    parser.add_argument("--label", choices=("gnome", "headless-vm", "headless-container"), required=True)
    args = parser.parse_args()
    if args.duration_seconds < 1 or args.interval_seconds <= 0 or args.read_timeout_seconds <= 0:
        parser.error("duration, interval, and timeout must be positive")
    if args.sample.startswith("/") or ".." in Path(args.sample).parts or args.sample in ("", "."):
        parser.error("sample must be a safe mount-relative file path")
    if len(args.sha256) != 64 or any(c not in "0123456789abcdef" for c in args.sha256):
        parser.error("sha256 must be 64 lowercase hex characters")
    if not args.mountpoint.is_absolute():
        parser.error("mountpoint must be absolute")
    initial_mount_id = mount_id(args.mountpoint)
    if initial_mount_id is None:
        parser.error("mountpoint is not fuse.porthome; no soak was run")
    if args.report.exists() or args.report.with_suffix(".jsonl").exists():
        parser.error("report path already exists; refusing to overwrite evidence")
    args.report.parent.mkdir(parents=True, exist_ok=True)
    samples_path = args.report.with_suffix(".jsonl")
    started = time.time()
    deadline = time.monotonic() + args.duration_seconds
    cache_start = tree_bytes(args.cache_dir)
    status_start = status_data(args.status)
    latencies: list[float] = []
    counts = {"probes": 0, "ok": 0, "read_errors": 0, "hash_errors": 0,
              "hangs": 0, "mount_losses": 0, "mount_replacements": 0,
              "process_losses": 0, "session_losses": 0}
    max_cache = cache_start
    max_rss = process_rss_kib(args.pid)
    interrupted = False
    try:
        with samples_path.open("x") as output:
            while True:
                now = time.time()
                row: dict = {"epoch": now, "elapsed_seconds": round(now - started, 3)}
                counts["probes"] += 1
                current_mount_id = mount_id(args.mountpoint)
                if current_mount_id is None:
                    row["result"] = "mount_lost"
                    counts["mount_losses"] += 1
                elif current_mount_id != initial_mount_id:
                    row["result"] = "mount_replaced"
                    counts["mount_replacements"] += 1
                elif args.pid and not Path(f"/proc/{args.pid}").exists():
                    row["result"] = "process_lost"
                    counts["process_losses"] += 1
                elif args.session_pid and not Path(f"/proc/{args.session_pid}").exists():
                    row["result"] = "session_lost"
                    counts["session_losses"] += 1
                else:
                    before = time.monotonic()
                    try:
                        worker = subprocess.Popen(
                            [sys.executable, __file__, "--probe-gio" if args.gio else "--probe",
                             str(args.mountpoint / args.sample)],
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                            text=True, start_new_session=True)
                        try:
                            stdout, stderr = worker.communicate(timeout=args.read_timeout_seconds)
                        except subprocess.TimeoutExpired:
                            # Kill the entire worker group (including gio). Do not
                            # wait indefinitely on an uninterruptible FUSE task.
                            os.killpg(worker.pid, signal.SIGKILL)
                            try:
                                worker.communicate(timeout=1)
                            except subprocess.TimeoutExpired:
                                pass
                            row["latency_ms"] = round((time.monotonic() - before) * 1000, 3)
                            row["result"] = "hang"
                            counts["hangs"] += 1
                        else:
                            row["latency_ms"] = round((time.monotonic() - before) * 1000, 3)
                            if worker.returncode:
                                row["result"] = "read_error"
                                row["detail"] = stderr[-300:]
                                counts["read_errors"] += 1
                            elif stdout.strip() != args.sha256:
                                row["result"] = "hash_error"
                                counts["hash_errors"] += 1
                            else:
                                row["result"] = "ok"
                                counts["ok"] += 1
                                latencies.append(row["latency_ms"])
                    except OSError as error:
                        row["result"] = "read_error"
                        row["detail"] = str(error)[-300:]
                        counts["read_errors"] += 1
                row["cache_bytes"] = tree_bytes(args.cache_dir)
                max_cache = max(max_cache, row["cache_bytes"])
                row["rss_kib"] = process_rss_kib(args.pid)
                if row["rss_kib"] is not None:
                    max_rss = max(max_rss or 0, row["rss_kib"])
                row["fuse_status"] = status_data(args.status)
                output.write(json.dumps(row, sort_keys=True) + "\n")
                output.flush()
                if time.monotonic() >= deadline:
                    break
                time.sleep(min(args.interval_seconds, max(0, deadline - time.monotonic())))
    except KeyboardInterrupt:
        interrupted = True
    status_end = status_data(args.status)
    report = {
        "schema": 1, "label": args.label, "started_epoch": started,
        "ended_epoch": time.time(), "requested_seconds": args.duration_seconds,
        "completed": not interrupted and time.monotonic() >= deadline,
        "mountpoint": str(args.mountpoint), "mount_id": initial_mount_id,
        "sample": args.sample,
        "gio": args.gio, "session_pid": args.session_pid,
        "counts": counts, "latency_ms": {
            "p50": percentile(latencies, .50), "p95": percentile(latencies, .95),
            "p99": percentile(latencies, .99), "max": max(latencies, default=None)},
        "cache_bytes": {"start": cache_start, "end": tree_bytes(args.cache_dir), "max": max_cache},
        "rss_kib": {"end": process_rss_kib(args.pid), "max": max_rss},
        "fuse_status_start": status_start, "fuse_status_end": status_end,
        "samples_jsonl": str(samples_path),
    }
    with args.report.open("x") as output:
        json.dump(report, output, indent=2, sort_keys=True)
        output.write("\n")
    print(json.dumps(report, indent=2, sort_keys=True))
    return 0 if report["completed"] and counts["ok"] == counts["probes"] else 1


if __name__ == "__main__":
    if len(sys.argv) == 3 and sys.argv[1] in ("--probe", "--probe-gio"):
        try:
            print(probe(Path(sys.argv[2]), gio=sys.argv[1] == "--probe-gio"))
        except (OSError, ValueError, subprocess.CalledProcessError) as error:
            print(f"probe: {error}", file=sys.stderr)
            sys.exit(1)
    else:
        sys.exit(main())
