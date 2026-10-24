#!/usr/bin/env python3
"""Collect actual hosted attempts, execute bound physical phases, and prepare W12 patches.

No operation grants W11 credit: physical process success is not reviewed coverage,
job timing is not a complete paired workload, and planner replay is not execution.
Use an empty output directory; failures and original inputs are retained. Cutover
support only emits unapplied patches; it never changes GitHub or active policy.
"""
import argparse
import contextlib
import datetime as dt
import hashlib
import json
import math
import os
import platform
import zipfile
import io
import fcntl
import difflib
import stat
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


class CommandInterrupted(CommandTimeout):
    """An owned command stopped by cancellation; handled exactly like a timeout."""


def command(argv, cwd, prefix, timeout=120, env=None):
    start = time.monotonic()
    if os.name != "posix":
        raise ValueError("diagnostic command custody requires POSIX process groups")
    proc = subprocess.Popen(argv, cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                            env=environment() if env is None else env, start_new_session=True)
    timed_out, termination, interrupted = False, None, False
    try:
        stdout, stderr = proc.communicate(timeout=timeout)
        code = proc.returncode
    except (subprocess.TimeoutExpired, KeyboardInterrupt) as exc:
        interrupted = isinstance(exc, KeyboardInterrupt)
        timed_out = not interrupted
        try:
            os.killpg(proc.pid, signal.SIGKILL)
            termination = "owned_process_group_sigkill"
        except ProcessLookupError:
            termination = "owned_process_group_already_exited"
        except OSError:
            termination = "cleanup_unverified_group_kill"
        try:
            stdout, stderr = proc.communicate(timeout=5)
        except subprocess.TimeoutExpired as cleanup_exc:
            # An escaped child may retain pipes. Do not wait forever or claim
            # descendant custody: the cohort stops and its checkout is retained.
            stdout, stderr = cleanup_exc.stdout or b"", cleanup_exc.stderr or b""
            proc.stdout.close()
            proc.stderr.close()
            termination = "cleanup_unverified_pipes_retained"
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            termination = "cleanup_unverified_primary_not_reaped"
        code = 130 if interrupted else 124
    prefix.with_suffix(".stdout").write_bytes(stdout)
    prefix.with_suffix(".stderr").write_bytes(stderr)
    record = {"argv": argv, "cwd": str(cwd), "exit_code": code,
              "elapsed_seconds": time.monotonic() - start,
              "stdout_sha256": digest(stdout), "stderr_sha256": digest(stderr),
              "timed_out": timed_out, "interrupted": interrupted, "termination": termination,
              "custody_scope": "owned_posix_process_group_not_kernel_namespace"}
    write(prefix.with_suffix(".process.json"), record)
    if timed_out or interrupted:
        raise (CommandInterrupted if interrupted else CommandTimeout)(
            f"command timed out; cohort stopped; see {prefix}.process.json")
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
                         "seconds": (end - start).total_seconds(),
                         "runner_id": job.get("runner_id"), "runner_name": job.get("runner_name"),
                         "labels": job.get("labels", []), "steps": job.get("steps", [])})
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

    return {"samples": len(values), "median": quantile(.5), "min": values[0], "max": values[-1], "range": [values[0], values[-1]],
            "iqr": quantile(.75) - quantile(.25), "quantile_method": "linear (n-1)"}


CONDITIONS = ("grotto", "groundhog-ui", "mls", "shared-core", "shared-store-media",
              "graph-change", "unknown-path")
PHASES = ("setup", "configure", "build", "test")


def api(endpoint, out, name):
    raw = command(["gh", "api", endpoint], out, out / name, timeout=60)
    (out / (name + ".json")).write_bytes(raw)
    return json.loads(raw)


def api_pages(endpoint, key, out, name):
    rows, total = [], None
    for page in range(1, 101):
        payload = api(endpoint + f"?per_page=100&page={page}", out, f"{name}-{page}")
        if total is None:
            total = payload["total_count"]
        if type(total) is not int or payload["total_count"] != total:
            raise ValueError("API inventory changed during pagination; retain and recollect")
        rows.extend(payload[key])
        if len(rows) >= total:
            break
        if not payload[key]:
            raise ValueError("incomplete API pagination")
    if len(rows) != total or len({r["id"] for r in rows}) != total:
        raise ValueError("incomplete or duplicate API inventory")
    return rows


def collect(args, out):
    """One bounded attempt, not a wait/retry loop or automatic acceptance vote."""
    if not re.fullmatch(r"[\w.-]+/[\w.-]+", args.repository):
        raise ValueError("invalid repository")
    root = f"repos/{args.repository}/actions/runs/{args.run}"
    before = api(root, out, "run-before")
    attempt = api(root + f"/attempts/{args.attempt}", out, "attempt")
    if attempt["id"] != args.run or attempt["run_attempt"] != args.attempt:
        raise ValueError("run/attempt identity mismatch")
    jobs = api_pages(root + f"/attempts/{args.attempt}/jobs", "jobs", out, "jobs")
    if any(j["run_id"] != args.run or j["run_attempt"] != args.attempt for j in jobs):
        raise ValueError("job run/attempt identity mismatch")
    requested_logs = set(args.job_log or [])
    by_id = {j["id"]: j for j in jobs}
    if not requested_logs <= set(by_id) or any(by_id[i]["status"] != "completed" for i in requested_logs):
        raise ValueError("only completed job logs from this exact attempt may be fetched")
    log_proofs = []
    for job_id in sorted(requested_logs):
        endpoint = f"repos/{args.repository}/actions/jobs/{job_id}/logs"
        raw = command(["gh", "api", endpoint], out, out / f"job-log-{job_id}", timeout=60)
        if len(raw) > args.max_artifact_bytes:
            raise ValueError("job log over retained-byte budget; raw stdout retained")
        target = out / f"job-{job_id}.log"
        target.write_bytes(raw)
        log_proofs.append({"job_id": job_id, "endpoint": endpoint, "bytes": len(raw),
                           "sha256": digest(raw), "api_digest": None,
                           "limits": "Job logs API supplies no authoritative content digest; SHA256 is capture integrity, not an API checksum."})
    artifacts = api_pages(root + "/artifacts", "artifacts", out, "artifacts")
    requested = set(args.artifact or [])
    if not requested <= {a["id"] for a in artifacts}:
        raise ValueError("requested original artifact not in this root")
    for artifact in artifacts:
        if artifact["id"] not in requested:
            continue
        expected = artifact.get("digest", "")
        if (artifact["expired"] or not re.fullmatch(r"sha256:[0-9a-f]{64}", expected)
                or artifact["size_in_bytes"] > args.max_artifact_bytes):
            raise ValueError("artifact expired/unverifiable/over budget; no download")
        raw = command(["gh", "api", f"repos/{args.repository}/actions/artifacts/{artifact['id']}/zip"],
                      out, out / f"archive-{artifact['id']}", timeout=60)
        # Keep original ZIP, including failed verification; never extract paths.
        (out / f"artifact-{artifact['id']}.zip").write_bytes(raw)
        if len(raw) != artifact["size_in_bytes"] or "sha256:" + digest(raw) != expected:
            raise ValueError("original ZIP digest/size mismatch")
        with zipfile.ZipFile(io.BytesIO(raw)) as archive:
            members = archive.infolist()
            if (sum(m.file_size for m in members) > args.max_artifact_bytes
                    or len({m.filename for m in members}) != len(members)):
                raise ValueError("expanded artifact over budget or duplicate members")
            proof = [{"name": m.filename, "bytes": m.file_size,
                      "sha256": digest(archive.read(m))} for m in members]
        write(out / f"artifact-{artifact['id']}-proof.json", proof)
    after = api(root + f"/attempts/{args.attempt}", out, "attempt-after")
    stable = all(attempt.get(k) == after.get(k) for k in
                 ("status", "conclusion", "updated_at", "head_sha", "run_attempt"))
    timing = summarize_jobs({"total_count": len(jobs), "jobs": jobs})
    return {"kind": "w11_hosted_attempt_collection_v1", "repository": args.repository,
            "run_id": args.run, "run_attempt": args.attempt, "head_sha": attempt["head_sha"],
            "status": after["status"], "conclusion": after["conclusion"],
            "stable_snapshot": stable, "latest_attempt_at_capture": before["run_attempt"],
            "timing": timing, "job_log_proofs": log_proofs, "downloaded_artifact_ids": sorted(requested),
            "artifact_identities": [{k: a.get(k) for k in ("id", "name", "digest", "size_in_bytes", "workflow_run")} for a in artifacts if a["id"] in requested],
            "complete": False, "complete_gate_eligible": False, "qualifying_comparisons": 0,
            "limits": "API head is not tested merge SHA. Verify original plan/checkout evidence. "
                      "Artifacts are root-scoped, not inferred to belong to this attempt. "
                      "Job sums are observed allocated time, not billed time or DAG critical path."}


def census(args, out):
    if not re.fullmatch(r"[\w.-]+/[\w.-]+", args.repository):
        raise ValueError("invalid repository")
    rows = api_pages(f"repos/{args.repository}/actions/workflows/dependency-tests.yml/runs",
                     "workflow_runs", out, "roots")
    counts = {}
    for run in rows:
        state = run["conclusion"] or run["status"]
        counts[state] = counts.get(state, 0) + 1
    return {"kind": "w11_root_census_v1", "repository": args.repository, "root_count": len(rows),
            "root_states": counts, "roots": [{k: r.get(k) for k in
                ("id", "run_attempt", "event", "head_sha", "status", "conclusion", "updated_at", "html_url")} for r in rows],
            "snapshot_completed_at": dt.datetime.now(dt.timezone.utc).isoformat(),
            "complete": False, "complete_gate_eligible": False, "qualifying_comparisons": 0,
            "qualifying_broad_integrations": 0,
            "limits": "Dated sequential API census of distinct roots/latest attempts, not reviewed acceptance. "
                      "Prior attempts/failures in older retained snapshots are not erased."}


def physical_tree(repo, sha):
    """Compare real checkout bytes/modes to committed blobs, independent of index stat flags."""
    table = []
    for entry in git(repo, "ls-tree", "-rz", sha).split(b"\0"):
        if not entry:
            continue
        header, relative = entry.split(b"\t", 1)
        mode, kind, oid = header.decode().split()
        path = repo / os.fsdecode(relative)
        if any((repo / parent).is_symlink() for parent in path.relative_to(repo).parents):
            raise ValueError("physical source directory redirected through a symlink")
        if mode == "160000":
            if path.is_symlink():
                raise ValueError("physical submodule directory is a symlink")
            if git(path, "rev-parse", "HEAD").decode().strip() != oid:
                raise ValueError("physical submodule commit mismatch")
            table.append([os.fsdecode(relative), mode, physical_tree(path, oid)])
            continue
        actual_mode = path.lstat().st_mode
        if mode == "120000":
            if not stat.S_ISLNK(actual_mode):
                raise ValueError("physical source symlink mode mismatch")
            raw = os.fsencode(os.readlink(path))
        else:
            if not stat.S_ISREG(actual_mode) or bool(actual_mode & stat.S_IXUSR) != (mode == "100755"):
                raise ValueError("physical source file mode mismatch")
            raw = path.read_bytes()
        blob = hashlib.sha1(b"blob " + str(len(raw)).encode() + b"\0" + raw).hexdigest()
        if blob != oid:
            raise ValueError(f"physical source bytes differ from committed blob: {os.fsdecode(relative)}")
        table.append([os.fsdecode(relative), mode, digest(raw)])
    return digest(json.dumps(table, separators=(",", ":")).encode())


def source_state(repo, sha):
    if git(repo, "rev-parse", "HEAD").decode().strip() != sha:
        raise ValueError("checkout is not exact candidate")
    if git(repo, "status", "--porcelain", "--untracked-files=normal").strip():
        raise ValueError("measurement checkout must be clean (ignored build/cache outputs may remain)")
    modules = git(repo, "submodule", "status", "--recursive").decode()
    if any(line and line[0] != " " for line in modules.splitlines()):
        raise ValueError("submodules must be fully initialized at committed identities")
    return {"sha": sha, "tree": git(repo, "rev-parse", "HEAD^{tree}").decode().strip(),
            "submodules": modules, "physical_tree_sha256": physical_tree(repo, sha),
            "tracked_diff_sha256": digest(git(repo, "diff", "--no-ext-diff", "--no-textconv", "HEAD", "--"))}


def bound_files(repo, bindings):
    result = {}
    if not isinstance(bindings, dict) or not bindings:
        raise ValueError("nonempty committed input/tool/profile/capability bindings required")
    for relative, expected in bindings.items():
        path = Path(relative)
        if path.is_absolute() or ".." in path.parts:
            raise ValueError("binding must be repository relative")
        committed = git(repo, "show", "HEAD:" + relative)
        actual = (repo / path).read_bytes()
        if actual != committed or digest(actual) != expected:
            raise ValueError(f"binding mismatch: {relative}")
        result[relative] = expected
    return result


def provenance(args, repo, sha, out):
    """Bind current genuine PR metadata; historical transplanted changes are not admitted."""
    if not re.fullmatch(r"[\w.-]+/[\w.-]+", args.repository):
        raise ValueError("invalid repository")
    pr = api(f"repos/{args.repository}/pulls/{args.pull}", out, "pull")
    if pr["number"] != args.pull or pr["base"]["repo"]["full_name"] != args.repository:
        raise ValueError("PR repository/number mismatch")
    # No hidden second source: either its actual head or canonical merged result.
    if sha not in (pr["head"]["sha"], pr.get("merge_commit_sha")):
        raise ValueError("candidate must be original PR head or merge, not a transplanted/replayed change")
    commit = api(f"repos/{args.repository}/git/commits/{sha}", out, "candidate-commit")
    if commit["sha"] != sha or commit["tree"]["sha"] != git(repo, "rev-parse", sha + "^{tree}").decode().strip():
        raise ValueError("API candidate source tree mismatch")
    parents = git(repo, "show", "-s", "--format=%P", sha).decode().strip().split()
    if parents != [p["sha"] for p in commit["parents"]]:
        raise ValueError("API candidate parents mismatch")
    base = parents[0] if sha == pr.get("merge_commit_sha") else immutable(pr["base"]["sha"])
    merge_base = git(repo, "merge-base", base, pr["head"]["sha"]).decode().strip()
    diff_base, diff_head = (base, sha) if sha == pr.get("merge_commit_sha") else (merge_base, sha)
    diff = git(repo, "diff", "--no-ext-diff", "--no-textconv", "--binary", diff_base, diff_head, "--")
    (out / "original-change.patch").write_bytes(diff)
    return {"pull": args.pull, "url": pr["html_url"], "repository": args.repository,
            "head_sha": pr["head"]["sha"], "canonical_merge_sha": pr.get("merge_commit_sha"),
            "tested_sha": sha, "comparison_base": diff_base, "pr_merge_base": merge_base,
            "diff_kind": "merge_first_parent_integration" if sha == pr.get("merge_commit_sha") else "pr_merge_base_to_head",
            "change_sha256": digest(diff),
            "method": "original_pr_source_not_transplanted", "reviewed": False}


def execute(args, out):
    repo = Path(args.repo).resolve()
    lock = Path(git(repo, "rev-parse", "--git-common-dir").decode().strip())
    if not lock.is_absolute():
        lock = repo / lock
    with (lock / "w11-measurement.lock").open("a") as handle:
        try:
            fcntl.flock(handle, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError as exc:
            raise ValueError("another measurement owns this repository; no overlapping samples") from exc
        return execute_locked(args, out)


def execute_locked(args, out):
    """Run reviewed phase commands in ONE pre-provisioned checkout; no copying/setup inference."""
    repo = Path(args.repo).resolve()
    raw = Path(args.contract).read_bytes()
    (out / "contract.json").write_bytes(raw)
    contract = json.loads(raw)
    sha = immutable(contract["candidate_sha"])
    initial = source_state(repo, sha)
    if contract["condition"] not in CONDITIONS or contract["cache_state"] not in ("clean", "incremental"):
        raise ValueError("unknown measurement condition/cache state")
    if contract["route"] not in ("legacy", "selected") or type(contract["sample"]) is not int or contract["sample"] < 1:
        raise ValueError("invalid route/sample")
    if contract.get("kind") != "w11_physical_contract_v1":
        raise ValueError("unsupported execution contract")
    bindings = bound_files(repo, contract["input_bindings"])
    origin = provenance(args, repo, sha, out)
    # Contract authors must retain exact recipe, not use an unreviewed shell string.
    phases = contract["phases"]
    phase_names = [p["phase"] for p in phases]
    if phase_names not in (list(PHASES), ["workload"]):
        raise ValueError("use exact ordered setup/configure/build/test or one intact workload command; never invent phase splits")
    env = contract["environment"]
    if not isinstance(env, dict) or any(not isinstance(k, str) or not isinstance(v, str) for k, v in env.items()):
        raise ValueError("explicit string environment required")
    if not all(env.get(k) for k in ("PATH", "HOME", "TMPDIR")):
        raise ValueError("explicit PATH/HOME/TMPDIR required; no inherited skip/cache controls")
    if any(k.startswith(("GIT_", "NOSTRC_SKIP_")) or re.search(r"TOKEN|PASSWORD|SECRET|CREDENTIAL|PRIVATE_KEY", k, re.I) for k in env):
        raise ValueError("caller Git routing, skip controls and secret-bearing environment are forbidden")
    if env.get("NOSTRC_GATE_LINUX_TESTS") == "0":
        raise ValueError("disabled tests cannot be a measured workload")
    tools = {}
    for phase in phases:
        argv = phase["argv"]
        if not isinstance(argv, list) or not argv or any(not isinstance(a, str) or not a for a in argv):
            raise ValueError("nonempty physical argv required")
        executable = shutil.which(argv[0], path=env["PATH"])
        if not executable or digest(Path(executable).resolve().read_bytes()) != phase["executable_sha256"]:
            raise ValueError("physical executable fingerprint differs from reviewed contract")
        if type(phase["timeout_seconds"]) is not int or phase["timeout_seconds"] < 1:
            raise ValueError("positive phase timeout required")
        tools[phase["phase"]] = {"path": str(Path(executable).resolve()), "sha256": phase["executable_sha256"]}
    probes = []
    for index, probe in enumerate(contract["tool_probes"]):
        executable = shutil.which(probe["argv"][0], path=env["PATH"])
        if not executable or digest(Path(executable).resolve().read_bytes()) != probe["executable_sha256"]:
            raise ValueError("tool probe executable fingerprint mismatch")
        command([str(Path(executable).resolve()), *probe["argv"][1:]], repo,
                out / f"tool-{index}", timeout=30, env=env)
        probes.append(json.loads((out / f"tool-{index}.process.json").read_text()))
    if not probes:
        raise ValueError("actual tool version probes required")
    result = {"kind": "w11_physical_sample_v1", "contract_sha256": digest(raw), "source": initial, "provenance": origin,
              "condition": contract["condition"], "cache_state": contract["cache_state"],
              "route": contract["route"], "sample": contract["sample"],
              "pair_id": contract["pair_id"], "profile": contract["profile"],
              "runner": {"system": platform.system(), "machine": platform.machine(), "node": platform.node()},
              "environment": env, "input_bindings": bindings, "tools": tools, "tool_probes": probes,
              "cache_evidence": contract["cache_evidence"], "phases": [],
              "phase_breakdown_available": phase_names == list(PHASES),
              "source_immutability_verified": False,
              "source_verification": "physical_pre_post_endpoints_only_within_phase_mutation_unverified",
              "complete": False, "complete_gate_eligible": False, "qualifying_comparisons": 0,
              "limits": "Measured serial local invocation, not runner union/billed/DAG cost or acceptance. "
                        "Cache labels, coverage, profile/capability and recipe equivalence require review; "
                        "process-group supervision is not kernel descendant custody. Source byte/mode checks "
                        "verify endpoints only, not immutable execution or modify/restore during a phase."}
    write(out / "result.json", result)
    started = time.monotonic()
    failure = None
    for phase in phases:
        name = phase["phase"]
        if failure:
            result["phases"].append({"phase": name, "status": "not_run"})
            continue
        try:
            source_state(repo, sha)
            executable = tools[name]["path"]
            if digest(Path(executable).read_bytes()) != tools[name]["sha256"]:
                raise ValueError("executable changed after admission")
            command([executable, *phase["argv"][1:]], repo, out / name, phase["timeout_seconds"], env=env)
            record = json.loads((out / (name + ".process.json")).read_text())
            if digest(Path(executable).read_bytes()) != tools[name]["sha256"]:
                raise ValueError("executable changed during phase")
            source_state(repo, sha)
            bound_files(repo, bindings)
            result["phases"].append({"phase": name, "status": "passed", "process": record})
        except (ValueError, OSError) as exc:
            failure = str(exc)
            path = out / (name + ".process.json")
            result["phases"].append({"phase": name, "status": "failed", "error": failure,
                                      "process": json.loads(path.read_text()) if path.exists() else None})
        write(out / "result.json", result)
    result.update(execution_passed=failure is None, error=failure,
                  serial_wall_seconds=time.monotonic() - started)
    outputs = []
    total_bytes = 0
    for index, relative in enumerate(contract.get("retained_outputs", [])):
        path = Path(relative)
        if path.is_absolute() or ".." in path.parts or not (repo / path).resolve().is_relative_to(repo):
            raise ValueError("retained output must stay inside measurement checkout")
        source = repo / path
        if not source.is_file():
            outputs.append({"path": relative, "status": "missing"})
            continue
        size = source.stat().st_size
        total_bytes += size
        if total_bytes > 8 * 1024 * 1024:
            raise ValueError("retained measurement outputs exceed 8 MiB budget")
        raw_output = source.read_bytes()
        target = out / f"retained-{index}"
        target.write_bytes(raw_output)
        outputs.append({"path": relative, "retained": target.name,
                        "sha256": digest(raw_output), "bytes": len(raw_output)})
    result["retained_outputs"] = outputs
    write(out / "result.json", result)
    return result


def matrix(args, out):
    """Serial capture of an explicit same-source schedule, never a recipe generator."""
    raw = Path(args.manifest).read_bytes()
    (out / "manifest.json").write_bytes(raw)
    manifest = json.loads(raw)
    if not isinstance(manifest, dict) or manifest.get("kind") != "w11_physical_matrix_v1" or not isinstance(manifest.get("entries"), list):
        raise ValueError("explicit w11_physical_matrix_v1 entries required")
    repository = manifest["repository"]
    if not isinstance(repository, str) or not re.fullmatch(r"[\w.-]+/[\w.-]+", repository):
        raise ValueError("invalid repository")
    scheduled, identities, pairs = [], set(), {}
    contracts = out / "contracts"
    contracts.mkdir()
    for index, entry in enumerate(manifest["entries"]):
        path = Path(entry["contract"])
        repo = Path(entry["repo"])
        if not path.is_absolute() or not repo.is_absolute():
            raise ValueError("matrix checkout/contract paths must be absolute")
        contract_raw = path.read_bytes()
        retained = contracts / f"{index:04d}.json"
        retained.write_bytes(contract_raw)
        if digest(contract_raw) != entry["contract_sha256"]:
            raise ValueError("matrix contract digest mismatch")
        c = json.loads(contract_raw)
        if (not isinstance(c, dict) or c.get("kind") != "w11_physical_contract_v1" or c["condition"] not in CONDITIONS
                or c["cache_state"] not in ("clean", "incremental") or c["route"] not in ("legacy", "selected")
                or type(c["sample"]) is not int or c["sample"] < 1
                or not isinstance(c["pair_id"], str) or not c["pair_id"]
                or type(entry["pull"]) is not int or entry["pull"] < 1):
            raise ValueError("invalid matrix contract identity")
        immutable(c["candidate_sha"])
        # A renamed pair is still the same sample slot, not an independent retry.
        key = (c["condition"], c["cache_state"], c["sample"])
        identity = key + (c["route"],)
        if identity in identities:
            raise ValueError("duplicate matrix sample/retry slot")
        identities.add(identity)
        pair = pairs.setdefault(key, {})
        if pair:
            other, other_entry = next(iter(pair.values()))
            if (any(c[k] != other[k] for k in ("candidate_sha", "pair_id", "profile", "environment", "tool_probes"))
                    or entry["pull"] != other_entry["pull"]):
                raise ValueError("matrix pair source/PR/profile/environment/tool identity differs")
        pair[c["route"]] = (c, entry)
        scheduled.append({"index": index, "repo": str(repo.resolve()), "pull": entry["pull"],
                          "contract": str(retained), "contract_sha256": digest(contract_raw),
                          "condition": c["condition"], "cache_state": c["cache_state"],
                          "route": c["route"], "sample": c["sample"], "status": "not_run"})
    cells = [{"condition": condition, "cache_state": cache,
              "planned_pairs": sum(k[:2] == (condition, cache) and set(v) == {"legacy", "selected"}
                                   for k, v in pairs.items())}
             for condition in CONDITIONS for cache in ("clean", "incremental")]
    ready = all(c["planned_pairs"] >= 5 for c in cells) and all(len(v) == 2 for v in pairs.values())
    result = {"kind": "w11_physical_matrix_capture_v1", "manifest_sha256": digest(raw),
              "schedule_ready": ready, "execute_requested": args.execute, "cells": cells, "entries": scheduled,
              "complete": False, "complete_gate_eligible": False, "qualifying_comparisons": 0,
              "qualifying_broad_integrations": 0,
              "limits": "Schedule readiness is not recipe/cache/capability approval or a CPU/Docker slot grant. "
                        "No checkout provisioning, cache reset, source transplant or historical engine override. "
                        "Uses existing endpoint-only local executor; full coverage/runner-union/critical-path review remains required."}
    write(out / "result.json", result)
    if not ready or not args.execute:
        return result
    sample_paths, active, cancelled = [], None, False
    try:
        # Keep all common-repository locks across the cohort, not just within a phase.
        with contextlib.ExitStack() as stack:
            lock_paths = set()
            for entry in scheduled:
                common = Path(git(entry["repo"], "rev-parse", "--git-common-dir").decode().strip())
                if not common.is_absolute():
                    common = Path(entry["repo"]) / common
                lock_paths.add(common.resolve() / "w11-measurement.lock")
            for path in sorted(lock_paths):
                handle = stack.enter_context(path.open("a"))
                try:
                    fcntl.flock(handle, fcntl.LOCK_EX | fcntl.LOCK_NB)
                except BlockingIOError as exc:
                    raise ValueError("another measurement owns a matrix repository; no overlapping samples") from exc
            for entry in scheduled:
                active = entry
                child = out / f"sample-{entry['index']:04d}"
                child.mkdir()
                # Retain only the exact admitted contract, including on later preflight failure.
                admitted = Path(entry["contract"]).read_bytes()
                if digest(admitted) != entry["contract_sha256"]:
                    raise ValueError("retained matrix contract changed after schedule admission")
                (child / "contract.json").write_bytes(admitted)
                child_args = argparse.Namespace(repo=entry["repo"], pull=entry["pull"],
                                                contract=str(child / "contract.json"), repository=repository)
                try:
                    sample = execute_locked(child_args, child)
                    sample_paths.append(str(child / "result.json"))
                    entry["status"] = "passed" if sample["execution_passed"] else "failed"
                    # command() captures a running-phase interrupt as interrupted:true
                    # in the child's process record; that is cancellation, not an
                    # ordinary workload failure.
                    interrupted = [phase["phase"] for phase in sample.get("phases", [])
                                   if (phase.get("process") or {}).get("interrupted") is True]
                    if interrupted:
                        cancelled = True
                        entry["status"] = "failed"
                        write(child / "cancellation.json", {"cancelled": True, "interrupted_phases": interrupted,
                                                            "process_records": [str(child / (name + ".process.json"))
                                                                                for name in interrupted],
                                                            "complete": False})
                except (ValueError, OSError, KeyError, TypeError, KeyboardInterrupt, subprocess.SubprocessError) as exc:
                    interrupt = isinstance(exc, (KeyboardInterrupt, CommandInterrupted))
                    cancelled = cancelled or interrupt
                    write(child / "failure.json", {"error": str(exc), "cancelled": interrupt, "complete": False})
                    sample_paths.append(str(child / "failure.json"))
                    entry.update(status="failed", error=str(exc))
                if cancelled:
                    entry["cancelled"] = True
                    result["cancelled"] = True
                write(out / "result.json", result)
                active = None
                if entry["status"] == "failed":
                    break  # No retry, replacement sample, or later workload after failure/cancellation.
        result["execution_passed"] = not cancelled and all(e["status"] == "passed" for e in scheduled)
        result["cancelled"] = cancelled
        result["measurement_report"] = measurement_report(sample_paths)
        result["measurement_matrix_satisfied"] = result["measurement_report"]["measurement_matrix_satisfied"]
        write(out / "result.json", result)
    except (ValueError, OSError, KeyError, TypeError, KeyboardInterrupt, subprocess.SubprocessError) as exc:
        if active is not None and active["status"] == "not_run":
            active.update(status="failed", error=str(exc))
        result.update(execution_passed=False, measurement_matrix_satisfied=False,
                      cohort_error=str(exc), cancelled=cancelled or isinstance(exc, KeyboardInterrupt),
                      retained_sample_paths=sample_paths)
        write(out / "failure.json", {"error": str(exc), "cancelled": result["cancelled"], "complete": False})
        write(out / "result.json", result)
    return result


def measurement_report(paths):
    """Descriptive paired data only. Never allows user-supplied `complete` to promote samples."""
    samples, identities, pairs, preflight_failures = [], set(), {}, []
    for path in paths:
        raw = Path(path).read_bytes()
        sample = json.loads(raw)
        if Path(path).name == "failure.json" and "error" in sample:
            contract_path = Path(path).parent / "contract.json"
            preflight_failures.append({"path": str(path), "sha256": digest(raw), "error": sample["error"],
                                       "contract_sha256": digest(contract_path.read_bytes()) if contract_path.exists() else None})
            continue
        if sample["kind"] != "w11_physical_sample_v1":
            raise ValueError("only physical sample records admitted")
        if (sample["condition"] not in CONDITIONS or sample["cache_state"] not in ("clean", "incremental")
                or sample["route"] not in ("legacy", "selected") or type(sample["sample"]) is not int
                or sample["sample"] < 1 or type(sample["execution_passed"]) is not bool
                or type(sample["serial_wall_seconds"]) not in (int, float)
                or not math.isfinite(sample["serial_wall_seconds"]) or sample["serial_wall_seconds"] < 0):
            raise ValueError("invalid physical sample condition/route/identity/outcome/timing")
        key = (sample["condition"], sample["cache_state"], sample["pair_id"], sample["sample"])
        identity = (sample["condition"], sample["cache_state"], sample["sample"], sample["route"])
        if identity in identities:
            raise ValueError("duplicate sample/retry; retain failed attempt separately, never overwrite")
        identities.add(identity)
        pairs.setdefault(key, {})[sample["route"]] = sample
        samples.append({"path": str(path), "sha256": digest(raw), "execution_passed": sample["execution_passed"]})
    cells = {}
    incomplete = []
    for key, routes in pairs.items():
        if set(routes) != {"legacy", "selected"}:
            incomplete.append(list(key))
            continue
        legacy, selected = routes["legacy"], routes["selected"]
        if any(legacy[k] != selected[k] for k in ("source", "profile", "runner", "provenance", "environment")):
            raise ValueError("paired routes differ in candidate/submodule/profile/runner/environment identity")
        def versions(sample):
            return sorted((tuple(p["argv"]), p["stdout_sha256"], p["stderr_sha256"]) for p in sample["tool_probes"])
        if versions(legacy) != versions(selected):
            raise ValueError("paired routes differ in actual tool version probes")
        cell = cells.setdefault(key[:2], {"legacy": [], "selected": [], "failures": [], "phases": {r: {} for r in ("legacy", "selected")}})
        if not legacy["execution_passed"] or not selected["execution_passed"]:
            cell["failures"].append(list(key))
            continue
        for route in routes:
            cell[route].append(routes[route]["serial_wall_seconds"])
            physical = routes[route]["phases"]
            if [p["phase"] for p in physical] not in (list(PHASES), ["workload"]) or any(
                    p["status"] != "passed" or p["process"]["exit_code"] != 0 for p in physical):
                raise ValueError("successful measurement lacks actual zero-exit physical accounting")
            for phase in physical:
                cell["phases"][route].setdefault(phase["phase"], []).append(phase["process"]["elapsed_seconds"])
    rows = []
    for condition in CONDITIONS:
        for cache in ("clean", "incremental"):
            cell = cells.get((condition, cache), {"legacy": [], "selected": [], "failures": []})
            row = {"condition": condition, "cache_state": cache, "successful_pairs": len(cell["legacy"]),
                   "failed_pairs": cell["failures"]}
            if len(cell["legacy"]) >= 5:
                row.update(legacy=statistics(cell["legacy"]), selected=statistics(cell["selected"]),
                           phase_statistics={r: {p: statistics(v) for p, v in phases.items() if len(v) >= 5}
                                             for r, phases in cell["phases"].items()},
                           full_phase_breakdown=all(set(phases) == set(PHASES) and all(len(v) == len(cell[r]) for v in phases.values())
                                                    for r, phases in cell["phases"].items()))
                baseline = row["legacy"]["median"]
                row["serial_wall_reduction_percent"] = (100 * (1 - row["selected"]["median"] / baseline)
                                                         if baseline else None)
            rows.append(row)
    return {"kind": "w11_unreviewed_paired_measurements_v1", "cells": rows, "inputs": samples,
            "unpaired_samples": incomplete, "preflight_failures": preflight_failures,
            "measurement_matrix_satisfied": not incomplete and not preflight_failures and all(
                r["successful_pairs"] >= 5 and not r["failed_pairs"] for r in rows),
            "complete": False, "complete_gate_eligible": False,
            "qualifying_comparisons": 0, "qualifying_broad_integrations": 0,
            "limits": "Descriptive serial phase/wall measurements only. No PR provenance, recipe, "
                      "cache independence, coverage differences, runner union, critical path or W11 thresholds accepted."}


LEGACY_TRIGGER_FILES = (
    "bp-matrix.yml", "gnostr-appimage.yml", "grotto-ci.yml", "grotto-fuzz.yml",
    "groundhog-ci.yml", "libgo-ci.yml", "libhanami-ci.yml", "libnostr-gi.yml",
    "marmot-mdk-interop.yml", "marmot-mdk011-interop.yml", "nip55l-libsecret-ci.yml",
    "nostr-homed-ci.yml", "nostrc-ci.yml", "signet-ci.yml", "static-checks.yml", "windows-build.yml")


def remove_duplicate_triggers(text, windows=False):
    """Narrow authored YAML shape only; no general workflow parser or active mutation."""
    lines = text.splitlines(keepends=True)
    start = lines.index("on:\n") + 1
    end = next((i for i in range(start, len(lines))
                if lines[i].strip() and not lines[i][0].isspace() and not lines[i].startswith("#")), len(lines))
    body = lines[start:end]
    result, i = [], 0
    while i < len(body):
        match = re.fullmatch(r"  (push|pull_request|schedule):\n", body[i])
        if not match:
            result.append(body[i])
            i += 1
            continue
        trigger = match[1]
        i += 1
        while i < len(body) and not re.match(r"  [a-z_]+:", body[i]) and (
                not body[i].strip() or body[i].startswith(("    ", "#"))):
            i += 1
        if windows and trigger == "push":
            # This workflow is also a publisher. Never remove its tag path.
            result.extend(["  push:\n", "    tags: ['v*']\n"])
    return "".join(lines[:start] + result + lines[end:])


def prepare_cutover(args, out):
    """Emit forward/reverse patches only. No application, HTTP writes or branch moves."""
    repo, sha = Path(args.repo).resolve(), immutable(args.sha)
    before, after = {}, {}
    def load(path):
        text = git(repo, "show", sha + ":" + path).decode()
        before[path] = text
        return text
    for name in LEGACY_TRIGGER_FILES:
        path = ".github/workflows/" + name
        after[path] = remove_duplicate_triggers(load(path), windows=name == "windows-build.yml")
    path = ".github/workflows/dependency-tests.yml"
    text = load(path)
    old = "# NOT required yet. Keep legacy required checks, duplicate triggers and schedules through W11."
    if old not in text or "  schedule:\n" in text:
        raise ValueError("unexpected dependency trigger shape; re-review source, do not guess")
    text = text.replace(old, "# W12 cutover: require Dependency gate only after dual-status W11 acceptance.")
    # W9 moves scheduled workloads into existing master/release/manual broad
    # calls. Owner policy has no scheduled tier; do not create replacement cron.
    after[path] = text
    path = "scripts/pre-push"
    text = load(path)
    needle = ('if [ "${NOSTRC_SELECTIVE_GATE:-0}" = 1 ]; then\n'
              '    echo "Pre-push: explicit diagnostic shared gate (does not replace mandatory rollout gates)"\n'
              '    bash "$repo_root/scripts/test-gate.sh" --repo "$repo_root" --pre-push --refs-file "$tmp/refs"\nfi')
    if text.count(needle) != 1:
        raise ValueError("unexpected local hook source")
    after[path] = text.replace(needle,
        'echo "Pre-push: shared selective gate (NOSTRC_FULL_GATE=1 selects full override)"\n'
        'bash "$repo_root/scripts/test-gate.sh" --repo "$repo_root" --pre-push --refs-file "$tmp/refs"')
    header, marker, body = after[path].partition("set -euo pipefail\n")
    if not marker:
        raise ValueError("unexpected hook header shape")
    after[path] = ('#!/usr/bin/env bash\n'
                   '# W12 operator-accepted selective pre-push: static checks then shared execution.\n'
                   '# NOSTRC_FULL_GATE=1 retains the original full broad override.\n'
                   '# Explicit NOSTRC_SKIP_PRE_PUSH remains an announced bypass, not validation.\n'
                   + marker + body)
    for path, heading in (("AGENTS.md", "# Agent Instructions\n"), ("docs/TESTING.md", None)):
        text = load(path)
        policy = ("\n## W12 default policy (operator-accepted cutover)\n\n"
                  "The pre-push hook runs static checks and the shared selective gate by default.\n"
                  "`NOSTRC_FULL_GATE=1 git push` retains the original full broad override.\n"
                  "`NOSTRC_SELECTIVE_GATE=0` does not disable shared execution. Explicit skip\n"
                  "controls are bypasses, never successful validation. GitHub requires\n"
                  "`Dependency gate`; legacy required checks are removed only after dual-status\n"
                  "PR and enabled merge-group proof. Proven master/release/manual broad calls\n"
                  "replace all three legacy scheduled workloads; no scheduled tier remains.\n"
                  "Rollback restores broad required checks and legacy\n"
                  "triggers before removing the new requirement. Release publication mode\n"
                  "is unchanged by this patch. Dated shadow/default statements below are historical;\n"
                  "their source-bound test results and limitations are not rewritten as acceptance.\n")
        # Replace contradictory default statements, not just add a new headline.
        text = text.replace("`NOSTRC_SELECTIVE_GATE=1` adds diagnostics to existing push gates, never replaces\n  them.",
                            "Shared selective execution is now the default after W11/operator acceptance;\n  static checks and the explicit full override remain.")
        text = text.replace("**Staged dependency-aware testing — operational rules:**", "**Dependency-aware testing — operational rules:**")
        text = text.replace("- Shadow workflows are additive, not accepted required checks. Publisher",
                            "- After recorded W11/operator acceptance, Dependency gate is required and the\n  shared local gate is default. Publisher")
        text = text.replace("Full builds and tests run in CI, scoped per component by\nworkflow path filters.",
                            "Full builds and tests run through shared CI selection;\nmaster/releases/manual retain complete broad validation.")
        if heading:
            after[path] = text.replace(heading, heading + policy, 1)
        else:
            first, sep, rest = text.partition("\n")
            after[path] = first + sep + policy + "\n" + rest
    forward, reverse = [], []
    for path in sorted(before):
        forward.extend(difflib.unified_diff(before[path].splitlines(True), after[path].splitlines(True),
                                           fromfile="a/" + path, tofile="b/" + path))
        reverse.extend(difflib.unified_diff(after[path].splitlines(True), before[path].splitlines(True),
                                           fromfile="a/" + path, tofile="b/" + path))
    # Git accepts unprefixed empty context lines. Keep versioned patch artifacts
    # whitespace-clean without weakening repository diff checks or target context.
    def patch_text(lines):
        return "".join("\n" if line == " \n" else line for line in lines)
    (out / "w12-forward.patch").write_text(patch_text(forward))
    (out / "w12-rollback.patch").write_text(patch_text(reverse))
    protection = {"status": "blocked", "reason": "refresh protected-branch snapshot and operator-approved legacy required contexts"}
    if args.protection:
        raw = Path(args.protection).read_bytes()
        (out / "protection-input.json").write_bytes(raw)
        current = json.loads(raw)
        status = current.get("required_status_checks")
        if status and type(status.get("strict")) is bool and status.get("checks"):
            old = [{"context": c["context"], "app_id": c["app_id"]} for c in status["checks"]]
            new = {"context": "Dependency gate", "app_id": args.app_id}
            if type(args.app_id) is not int or args.app_id < 1:
                raise ValueError("observed GitHub Actions app ID required, never inferred")
            if new in old:
                raise ValueError("snapshot already requires new gate; rebase transition protocol")
            obsolete = set(args.obsolete_context or [])
            if not obsolete or any(sum(c["context"] == name and c["app_id"] == args.app_id for c in old) != 1 for name in obsolete):
                raise ValueError("explicit reviewed obsolete contexts from the observed Actions app required")
            retained = [c for c in old if not (c["context"] in obsolete and c["app_id"] == args.app_id)]
            write(out / "required-dual.json", {"strict": status["strict"], "checks": old + [new]})
            write(out / "required-selected.json", {"strict": status["strict"], "checks": retained + [new]})
            write(out / "required-rollback-dual.json", {"strict": status["strict"], "checks": old + [new]})
            write(out / "required-rollback-legacy.json", {"strict": status["strict"], "checks": old})
            protection = {"status": "prepared_not_applied", "input_sha256": digest(raw),
                          "method": "PATCH", "endpoint": "repos/chebizarro/nostrc/branches/master/protection/required_status_checks",
                          "order": ["required-dual.json", "verify real PR and enabled merge_group", "required-selected.json"],
                          "rollback_order": ["required-rollback-dual.json", "restore repository patch and verify broad checks", "required-rollback-legacy.json"]}
    return {"kind": "w12_unapplied_patch_v1", "source_sha": sha,
            "source_bindings": {p: digest(t.encode()) for p, t in before.items()},
            "forward_sha256": digest((out / "w12-forward.patch").read_bytes()),
            "rollback_sha256": digest((out / "w12-rollback.patch").read_bytes()),
            "protection": protection, "applied": False, "complete": False,
            "prerequisites": "Original W11 complete; parent accepts exact source, coverage/trigger union, "
                             "manual broad override, release dryrun and rollback tests. Recheck live protection "
                             "before each conditional write; 404 is not permission to invent a branch policy. "
                             "Do not apply either patch before that acceptance."}


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
    cp = sub.add_parser("collect", help="Retain one actual hosted run/attempt and bounded original ZIPs; never poll")
    cp.add_argument("--repository", default="chebizarro/nostrc")
    cp.add_argument("--run", type=int, required=True)
    cp.add_argument("--attempt", type=int, required=True)
    cp.add_argument("--artifact", type=int, action="append", help="explicit original ZIP ID; default metadata only")
    cp.add_argument("--job-log", type=int, action="append", help="explicit completed job ID from this attempt; logs API has no content digest")
    cp.add_argument("--max-artifact-bytes", type=int, default=8 * 1024 * 1024)
    cn = sub.add_parser("census", help="Retain all shadow root metadata once, no old artifact downloads")
    cn.add_argument("--repository", default="chebizarro/nostrc")
    ep = sub.add_parser("execute", help="Run reviewed physical phases or one intact adapter in an existing exact-source checkout")
    ep.add_argument("--repo", required=True)
    ep.add_argument("--contract", required=True)
    ep.add_argument("--repository", default="chebizarro/nostrc")
    ep.add_argument("--pull", type=int, required=True)
    mp = sub.add_parser("measurements", help="Account paired physical samples, failures and all 14 clean/incremental cells")
    mp.add_argument("--sample", action="append", required=True)
    mx = sub.add_parser("matrix", help="Validate all 14 five-pair cells; optionally capture serial reviewed contracts without retries")
    mx.add_argument("--manifest", required=True)
    mx.add_argument("--execute", action="store_true", help="requires parent exclusive slot and reviewed recipes; default only checks schedule")
    wp = sub.add_parser("prepare-cutover", help="Generate source-bound forward/rollback patches; NEVER apply or mutate GitHub")
    wp.add_argument("--repo", required=True)
    wp.add_argument("--sha", required=True)
    wp.add_argument("--protection", help="retained GET branch protection JSON; 404 leaves server patch blocked")
    wp.add_argument("--app-id", type=int, help="actual observed check-run app.id")
    wp.add_argument("--obsolete-context", action="append", help="only these explicitly reviewed old Actions contexts may be removed")
    for cmd in (rp, jp, sp, ip, cp, cn, ep, mp, mx, wp):
        cmd.add_argument("--output", required=True, help="new retained evidence directory")
    args = parser.parse_args()
    out = Path(args.output).resolve()
    out.mkdir(parents=True, exist_ok=False)
    write(out / "runner.json", {"argv": sys.argv, "runner_sha256": digest(Path(__file__).read_bytes()),
                                "python": sys.version, "python_executable": str(Path(sys.executable).resolve()),
                                "python_sha256": digest(Path(sys.executable).resolve().read_bytes()),
                                "started_at": dt.datetime.now(dt.timezone.utc).isoformat()})
    if args.operation in ("execute", "matrix"):
        def interrupted(signum, frame):
            raise KeyboardInterrupt("measurement cancelled")
        signal.signal(signal.SIGTERM, interrupted)
    try:
        if args.operation == "replay":
            result = replay(args, out)
        elif args.operation == "collect":
            result = collect(args, out)
        elif args.operation == "census":
            result = census(args, out)
        elif args.operation == "execute":
            result = execute(args, out)
        elif args.operation == "matrix":
            result = matrix(args, out)
        elif args.operation == "measurements":
            result = measurement_report(args.sample)
        elif args.operation == "prepare-cutover":
            result = prepare_cutover(args, out)
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
        return int(result.get("execution_passed") is False or result.get("schedule_ready") is False or
                   result.get("measurement_matrix_satisfied") is False or
                   any(c["replay_status"] == "failed" for c in result.get("candidates", [])))
    except (ValueError, OSError, KeyError, TypeError, KeyboardInterrupt, zipfile.BadZipFile, subprocess.SubprocessError) as exc:
        write(out / "failure.json", {"error": str(exc), "complete": False})
        print(str(exc), file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
