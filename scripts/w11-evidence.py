#!/usr/bin/env python3
"""Reproduce immutable real-PR selection and measure retained Actions jobs.

Neither operation grants W11 credit: plans are not physical coverage, and nested
job timing is not an independently validated legacy/selected workload pair.
Use an empty output directory; failed commands and original inputs are retained.
"""
import argparse
import datetime as dt
import hashlib
import json
import math
import os
from pathlib import Path
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import time


SHA = re.compile(r"[0-9a-f]{40}")


def digest(data):
    return hashlib.sha256(data).hexdigest()


def write(path, value):
    path.write_text(json.dumps(value, sort_keys=True, indent=2) + "\n")


def environment():
    # This tool only uses local Git. Never inherit caller index/repository,
    # alternate-object/config routing into the disposable checkout or its plans.
    env = {k: v for k, v in os.environ.items() if not k.startswith("GIT_")}
    env.update(GIT_CONFIG_NOSYSTEM="1", GIT_CONFIG_GLOBAL=os.devnull,
               GIT_TERMINAL_PROMPT="0", PYTHONDONTWRITEBYTECODE="1")
    return env


class CommandTimeout(ValueError):
    pass


def command(argv, cwd, prefix, timeout=120):
    start = time.monotonic()
    if os.name != "posix":
        raise ValueError("diagnostic command custody requires POSIX process groups")
    proc = subprocess.Popen(argv, cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                            env=environment(), start_new_session=True)
    timed_out, termination = False, None
    try:
        stdout, stderr = proc.communicate(timeout=timeout)
        code = proc.returncode
    except subprocess.TimeoutExpired as exc:
        timed_out = True
        try:
            os.killpg(proc.pid, signal.SIGKILL)
            termination = "owned_process_group_sigkill"
        except ProcessLookupError:
            termination = "owned_process_group_already_exited"
        except OSError:
            termination = "cleanup_unverified_group_kill"
        try:
            stdout, stderr = proc.communicate(timeout=5)
        except subprocess.TimeoutExpired:
            # An escaped child may retain pipes. Do not wait forever or claim
            # descendant custody: the cohort stops and its checkout is retained.
            stdout, stderr = exc.stdout or b"", exc.stderr or b""
            proc.stdout.close()
            proc.stderr.close()
            termination = "cleanup_unverified_pipes_retained"
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            termination = "cleanup_unverified_primary_not_reaped"
        code = 124
    prefix.with_suffix(".stdout").write_bytes(stdout)
    prefix.with_suffix(".stderr").write_bytes(stderr)
    record = {"argv": argv, "cwd": str(cwd), "exit_code": code,
              "elapsed_seconds": time.monotonic() - start,
              "stdout_sha256": digest(stdout), "stderr_sha256": digest(stderr),
              "timed_out": timed_out, "termination": termination,
              "custody_scope": "owned_posix_process_group_not_kernel_namespace"}
    write(prefix.with_suffix(".process.json"), record)
    if timed_out:
        raise CommandTimeout(f"command timed out; cohort stopped; see {prefix}.process.json")
    if code:
        raise ValueError(f"command failed ({code}); see {prefix}.stderr")
    return stdout


def git(repo, *args):
    return subprocess.check_output(["git", "-C", str(repo), *args], stderr=subprocess.PIPE,
                                   env=environment())


def immutable(value):
    if not isinstance(value, str) or not SHA.fullmatch(value):
        raise ValueError("full immutable commit SHA required")
    return value


def candidates(prs, repo):
    """Validate merged PR metadata against local Git, not PR title guesses."""
    result, seen = [], set()
    for pr in sorted(prs, key=lambda p: p["number"]):
        number = pr["number"]
        if type(number) is not int or number <= 0 or number in seen:
            raise ValueError("invalid or duplicate PR number")
        seen.add(number)
        if not pr.get("merged_at"):
            raise ValueError(f"PR{number} is not merged")
        base, head, merged = (immutable(v) for v in
                              (pr["base"]["sha"], pr["head"]["sha"], pr["merge_commit_sha"]))
        for sha in (base, head, merged):
            if git(repo, "rev-parse", "--verify", sha + "^{commit}").decode().strip() != sha:
                raise ValueError("commit identity mismatch")
        parents = git(repo, "show", "-s", "--format=%P", merged).decode().split()
        # Squash/rebase histories need an independently reviewed range binding.
        # Do not guess their old base/head from the current PR API response.
        if parents != [base, head]:
            raise ValueError(f"PR{number} merged parents differ from API base/head")
        changes = git(repo, "diff", "--no-ext-diff", "--no-textconv", "--name-status", "-z",
                      "--find-renames", base, merged, "--")
        result.append({"number": number, "url": pr["html_url"], "title": pr["title"],
                       "base_sha": base, "pr_head_sha": head, "merged_sha": merged,
                       "tree": git(repo, "rev-parse", merged + "^{tree}").decode().strip(),
                       "diff_sha256": digest(changes), "diff_hex": changes.hex(), "replay_status": "not_run"})
    return result


def replay(args, out):
    raw = Path(args.prs).read_bytes()
    (out / "prs.json").write_bytes(raw)
    census = candidates(json.loads(raw), args.repo)
    result = {"schema_version": 1, "kind": "real_pr_logical_replay_v1",
              "prs_sha256": digest(raw), "qualifying_comparisons": 0,
              "complete": False, "complete_gate_eligible": False, "candidates": census}
    write(out / "result.json", result)  # Keep the census if a later setup step fails.
    temp = tempfile.mkdtemp(prefix="nostrc-w11-replay-")
    try:
        checkout = Path(temp) / "source"
        command(["git", "clone", "--shared", "--no-checkout", str(Path(args.repo).resolve()),
                 str(checkout)], None, out / "clone")
        git(checkout, "config", "core.hooksPath", "/dev/null")
        git(checkout, "config", "core.autocrlf", "false")
        for entry in census:
            dest = out / f"pr-{entry['number']}"
            dest.mkdir()
            sha = entry["merged_sha"]
            try:
                # This clone is owned and disposable. Never let an older PR's
                # ignored/untracked state influence the next original planner.
                command(["git", "clean", "-ffdx"], checkout, dest / "clean")
                command(["git", "checkout", "--detach", "--force", sha], checkout, dest / "checkout")
                # These are merged results, NOT the historically tested refs/pull/N/merge.
                # Bind the original committed planner, inventory and workflow bytes.
                names = git(checkout, "ls-files", "-z", "scripts/test*.py", "tests/selection/inventory.json",
                            ".github/workflows").decode().split("\0")
                inputs = {name: digest((checkout / name).read_bytes()) for name in names if name}
                write(dest / "inputs.json", inputs)
                event = {"kind": "pull_request", "base_sha": entry["base_sha"],
                         "pr_head_sha": entry["pr_head_sha"], "tested_sha": sha}
                write(dest / "event.json", event)
                plans = []
                for index in (1, 2):
                    data = command([sys.executable, "scripts/test-plan.py", "plan", "--repo", str(checkout),
                                    "--head", sha, "--context", "ci", "--event", str(dest / "event.json")],
                                   checkout, dest / f"plan-{index}")
                    plans.append(data)
                if plans[0] != plans[1]:
                    raise ValueError("logical selection is not byte-stable")
                plan = json.loads(plans[0])
                if plan["tested_sha"] != sha or plan["source_identity"] != {"kind": "committed", "sha": sha}:
                    raise ValueError("replayed plan source mismatch")
                if git(checkout, "status", "--porcelain", "--untracked-files=all", "--ignored").strip():
                    raise ValueError("historical planner modified source or left unbound state")
                if inputs != {name: digest((checkout / name).read_bytes()) for name in inputs}:
                    raise ValueError("historical planner inputs changed")
                entry.update({"replay_status": "logical_only", "source_role": "merged_result_not_hosted_tested_merge",
                              "plan_id": plan["plan_id"], "inventory_hash": plan["inventory_hash"],
                              "effective_mode": plan["effective_mode"], "selected_checks": plan["selected_checks"],
                              "expected_lane_ids": plan["expected_lane_ids"],
                              "broadening_reasons": plan["broadening_reasons"],
                              "inputs_sha256": digest((dest / "inputs.json").read_bytes()),
                              "plan_sha256": digest((dest / "plan-1.stdout").read_bytes())})
            except CommandTimeout as exc:
                entry.update({"replay_status": "failed", "error": str(exc)})
                raise
            except (ValueError, OSError, subprocess.SubprocessError) as exc:
                entry.update({"replay_status": "failed", "error": str(exc)})
            write(out / "result.json", result)
    except CommandTimeout:
        result["retained_checkout"] = temp
        write(out / "result.json", result)
        raise
    finally:
        if not result.get("retained_checkout"):
            shutil.rmtree(temp)
    return result


def instant(value):
    parsed = dt.datetime.fromisoformat(value.replace("Z", "+00:00"))
    if parsed.tzinfo is None:
        raise ValueError("job timestamp has no timezone")
    return parsed


def summarize_jobs(payload):
    jobs = payload["jobs"]
    if type(payload.get("total_count")) is not int or payload["total_count"] != len(jobs):
        raise ValueError("incomplete/paginated job inventory")
    ids, measured, pending = set(), [], []
    for job in jobs:
        if type(job["id"]) is not int or job["id"] in ids:
            raise ValueError("invalid or duplicate job ID")
        ids.add(job["id"])
        if job["status"] != "completed":
            pending.append(job["id"])
            continue
        if job.get("conclusion") == "skipped":
            continue
        if not job.get("started_at") or not job.get("completed_at"):
            raise ValueError("completed job is missing physical timestamps")
        start, end = instant(job["started_at"]), instant(job["completed_at"])
        if end < start:
            raise ValueError("negative job duration")
        measured.append({"id": job["id"], "name": job["name"], "conclusion": job["conclusion"],
                         "seconds": (end - start).total_seconds()})
    return {"kind": "partial_nested_job_timing_v1", "complete": False,
            "complete_gate_eligible": False, "qualifying_comparisons": 0,
            "job_count": len(jobs), "pending_job_ids": pending,
            "skipped_job_ids": [j["id"] for j in jobs if j.get("conclusion") == "skipped"],
            "observed_job_minutes": sum(j["seconds"] for j in measured) / 60,
            "jobs": measured,
            "limits": "Not billed runner-minutes, a DAG critical path, phase timings, a complete workload or paired savings."}


def inventory_register(policy):
    """Describe current gaps/claims; counts do not authorize execution."""
    subsets = policy["diagnostic_nonmember_subsets"]
    profiles = ("groundhog.linux", "grotto.linux")
    names = subsets[profiles[0]]["registrations"]
    if (len(names) != 33 or len(set(names)) != 33
            or any(subsets[p]["registrations"] != names for p in profiles)):
        raise ValueError("current ordinary33 cohort is ambiguous; review required")
    ownership = {}
    for profile in policy["profiles"]:
        claims = {name: [] for name in names}
        for ident, check in policy["checks"].items():
            if check.get("kind") == "ctest" and profile in check.get("profiles", []):
                for name in check.get("ctest_tests", {}).get(profile, []):
                    if name in claims:
                        claims[name].append(ident)
        ownership[profile] = {"claimed": sum(bool(v) for v in claims.values()),
                              "unclaimed": [n for n, v in claims.items() if not v],
                              "multiply_claimed": [n for n, v in claims.items() if len(v) > 1], "claims": claims}
    return {"kind": "current_inventory_gap_register_v1", "complete": False, "complete_gate_eligible": False,
            "enforcement_ready": policy["enforcement_ready"], "coverage_gaps": policy["coverage_gaps"],
            "workflow_job_rows": policy.get("workflow_jobs", {}),
            "ordinary33_registrations": names, "ownership_by_profile": ownership,
            "ordinary_subset_contracts": {p: {k: subsets[p][k] for k in
                 ("profile_hash", "universe_hash", "candidate_contract_hash", "toolchain_contract_hash")}
                 for p in profiles},
            "limits": "Claims only, not fresh registration/input/prerequisite/phase/runtime audits or full-profile admission."}


def statistics(values):
    if len(values) < 5 or any(type(v) not in (int, float) or not math.isfinite(v) or v < 0 for v in values):
        raise ValueError("at least five finite nonnegative samples required")
    values = sorted(values)

    def quantile(q):
        index = (len(values) - 1) * q
        left = int(index)
        return values[left] + (values[min(left + 1, len(values) - 1)] - values[left]) * (index - left)

    return {"samples": len(values), "median": quantile(.5), "min": values[0], "max": values[-1],
            "iqr": quantile(.75) - quantile(.25), "quantile_method": "linear (n-1)"}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="operation", required=True)
    rp = sub.add_parser("replay", help="Execute each merged PR's own committed planner twice; never coverage")
    rp.add_argument("--prs", required=True, help="complete GitHub pulls API array (merged PRs only)")
    rp.add_argument("--repo", required=True)
    jp = sub.add_parser("timings", help="Measure retained complete-page Actions job API JSON, without promotion")
    jp.add_argument("--jobs", required=True)
    sp = sub.add_parser("statistics", help="Unattested five-sample descriptive statistics; not acceptance")
    sp.add_argument("--samples", required=True, help="JSON array of duration observations")
    ip = sub.add_parser("inventory", help="Retain the current gap rows and profile-specific ordinary33 claims")
    ip.add_argument("--inventory", required=True)
    for cmd in (rp, jp, sp, ip):
        cmd.add_argument("--output", required=True, help="new retained evidence directory")
    args = parser.parse_args()
    out = Path(args.output).resolve()
    out.mkdir(parents=True, exist_ok=False)
    write(out / "runner.json", {"argv": sys.argv, "runner_sha256": digest(Path(__file__).read_bytes()),
                                "python": sys.version, "python_executable": str(Path(sys.executable).resolve()),
                                "python_sha256": digest(Path(sys.executable).resolve().read_bytes()),
                                "started_at": dt.datetime.now(dt.timezone.utc).isoformat()})
    try:
        if args.operation == "replay":
            result = replay(args, out)
        else:
            source = {"timings": "jobs", "statistics": "samples", "inventory": "inventory"}[args.operation]
            raw = Path(getattr(args, source)).read_bytes()
            (out / "input.json").write_bytes(raw)
            result = (summarize_jobs(json.loads(raw)) if args.operation == "timings" else
                      inventory_register(json.loads(raw)) if args.operation == "inventory" else
                      {"kind": "unattested_descriptive_statistics_v1", "complete": False,
                       "complete_gate_eligible": False, "statistics": statistics(json.loads(raw))})
            result["input_sha256"] = digest(raw)
        write(out / "result.json", result)
        print(out / "result.json")
        return int(any(c["replay_status"] == "failed" for c in result.get("candidates", [])))
    except (ValueError, OSError, KeyError, TypeError, subprocess.SubprocessError) as exc:
        write(out / "failure.json", {"error": str(exc), "complete": False})
        print(str(exc), file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
