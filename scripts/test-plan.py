#!/usr/bin/env python3
"""Shared dependency-aware test planner. See --help for explicit contracts."""
import argparse
import hashlib
import importlib
import json
import os
import signal
import subprocess
import time
from pathlib import Path
import sys

import test_selection as selection


def parser():
    root = argparse.ArgumentParser(description=__doc__)
    commands = root.add_subparsers(dest="command", required=True)
    for name in ("plan", "explain", "validate", "resolve", "check-inputs", "run", "aggregate", "release-plan"):
        cmd = commands.add_parser(name)
        cmd.add_argument("--repo", default=str(Path(__file__).resolve().parents[1]))
        cmd.add_argument("--inventory", default="tests/selection/inventory.json")
        cmd.add_argument("--output")
        if name in {"plan", "explain"}:
            cmd.add_argument("--head", required=True)
            cmd.add_argument("--base")
            cmd.add_argument("--context", choices=("local", "ci", "release"), required=True)
            cmd.add_argument("--mode", choices=("affected", "broad"), default="affected")
            cmd.add_argument("--event", help="JSON file: kind and event SHA identities; array for multi-ref union")
            cmd.add_argument("--profile", action="append")
        elif name in {"validate", "resolve", "check-inputs", "run"}:
            cmd.add_argument("--build-dir", required=name != "validate")
            cmd.add_argument("--profile")
            cmd.add_argument("--plan", required=name in {"resolve", "run"})
            cmd.add_argument("--resolved")
            cmd.add_argument("--phase", choices=("configure", "build", "test", "all"), default="all")
            cmd.add_argument("--attempt", type=int, default=1)
            cmd.add_argument("--configure", action="store_true")
            cmd.add_argument("--strict", action="store_true")
            cmd.add_argument("--after-build", action="store_true")
            cmd.add_argument("--evidence-dir")
            cmd.add_argument("--parallel", type=int, default=1)
            cmd.add_argument("--timeout", type=int)
            cmd.add_argument("--evidence-path", action="append", default=[])
        elif name == "aggregate":
            cmd.add_argument("--plan", required=True)
            cmd.add_argument("--receipt", action="append", default=[])
            cmd.add_argument("--partial", action="store_true", help="diagnostic aggregation, cannot establish complete CI coverage")
            cmd.add_argument("--selection-conclusion", help="actual selector job conclusion; required for complete CI/release aggregation")
            cmd.add_argument("--job-results", help="independent lane ID to actual worker conclusion map; required for complete CI aggregate")
            cmd.add_argument("--release-validation", help="broad exact-tag validation result JSON")
            cmd.add_argument("--package-results", help="package job to conclusion map JSON; plan must be release-plan")
        elif name == "release-plan":
            cmd.add_argument("--head", required=True)
            cmd.add_argument("--tag")
            cmd.add_argument("--component")
            cmd.add_argument("--dry-run", action="store_true")
            cmd.add_argument("--run-id")
    return root


def _retained_file(path):
    path = Path(path)
    if not path.is_file() or path.is_symlink():
        return {"path": str(path), "status": "missing_or_unsafe"}
    hashed = hashlib.sha256()
    try:
        with path.open("rb") as stream:
            while chunk := stream.read(65536):
                hashed.update(chunk)
        return {"path": str(path), "status": "retained", "sha256": hashed.hexdigest(), "bytes": path.stat().st_size}
    except OSError as exc:
        return {"path": str(path), "status": "unavailable", "error": type(exc).__name__}


def _source_status(repo, excluded=()):
    """Safe dirty-source diagnostics: names/status/hashes, never diffs or contents."""
    raw = selection.git(repo, "status", "--porcelain=v1", "-z", "--untracked-files=all").stdout
    entries, records = iter(raw.split(b"\0")), []
    for entry in entries:
        if not entry:
            continue
        status, name = os.fsdecode(entry[:2]), os.fsdecode(entry[3:])
        record = {"git_status": status, "path": name}
        path = Path(repo) / name
        if status == "??" and not path.is_symlink() and any(path.resolve().is_relative_to(Path(p).resolve()) for p in excluded):
            continue
        if "R" in status or "C" in status:
            record["original_path"] = os.fsdecode(next(entries))
        # Resolve before opening: a changed symlink must not disclose or hash a
        # target outside the checkout. Only filenames and link status survive.
        if path.is_symlink() or not path.resolve().is_relative_to(Path(repo).resolve()):
            record["content_status"] = "symlink_or_outside_checkout"
        else:
            retained = _retained_file(path)
            record.update({k: v for k, v in retained.items() if k != "path"})
        records.append(record)
    return {"head": selection.commit_sha(repo, "HEAD"), "changes": records}


def _failure_receipt(args, policy, plan, lane, state, exc):
    resolved = state.get("resolved")
    raw = None
    raw_path = state.get("backend_receipt_path")
    if raw_path and Path(raw_path).is_file():
        # Retain bytes even if the child receipt cannot be interpreted.
        try:
            raw = selection.read_json(raw_path)
        except (OSError, ValueError):
            pass
    observed = raw.get("observed_executions", {}) if isinstance(raw, dict) else {}
    if not isinstance(observed, dict):
        observed = {}
    observed = {n: outcome for n, outcome in observed.items() if isinstance(outcome, str) and outcome in selection.OUTCOMES}
    checks = {}
    for ident in lane["checks"]:
        check = policy["checks"][ident]
        names = check["ctest_tests"][args.profile] if check["kind"] == "ctest" else [ident]
        outcomes = [observed.get(n, "missing") for n in names]
        checks[ident] = {"outcome": next((o for o in outcomes if o != "passed"), "passed"),
                         "expected_executions": names, "observed_executions": [n for n in names if observed.get(n, "missing") != "missing"]}
    error_chain, cancelled = [], False
    cause = exc
    while cause is not None:
        item = {"type": type(cause).__name__, "message": str(cause)}
        if hasattr(cause, "execution_status"):
            item["process_status"] = cause.execution_status
        cancelled |= isinstance(cause, KeyboardInterrupt)
        error_chain.append(item)
        cause = cause.__cause__
    # Hash only explicit owned evidence, never arbitrary source contents.
    paths = list(state["evidence_paths"])
    if raw_path and Path(raw_path).parent.is_dir():
        paths += [p for p in Path(raw_path).parent.rglob("*") if p.is_file()]
    paths += [Path(str(p) + ".process.json") for p in state["evidence_paths"] if str(p).endswith(".log")]
    failed_audit = any(not a["passed"] for a in state["audits"])
    registration_status = "failed" if failed_audit else "passed" if state["audits"] else "not_performed"
    input_status = "failed" if failed_audit else "passed" if any(a.get("after_build") for a in state["audits"]) else "not_completed"
    result = {"schema_version": 1, "receipt_kind": "lane_failure_v1",
              "plan_id": plan["plan_id"], "inventory_hash": plan["inventory_hash"],
              "source_identity": plan["source_identity"], "lane_id": lane["id"],
              "profile": args.profile, "profile_hash": lane["profile_hash"],
              "profile_fingerprint": state.get("realized", {}).get("profile_fingerprint"),
              "registration_audit": registration_status, "input_audit": input_status,
              "job_conclusion": "cancelled" if cancelled else "failure", "checks": checks,
              "complete": False, "diagnostic_eligible": False, "complete_gate_eligible": False,
              "coverage_gaps": plan.get("coverage_gaps", []),
              "failure": {"stage": state["stage"], "errors": error_chain,
                          "requested_phase": args.phase, "attempt": args.attempt,
                          "input_audits": state["audits"], "resolved": resolved,
                          "work_status": state["work_status"],
                          "execution_status": {n: observed.get(n, "not_executed" if state["work_status"]["test"] == "not_executed" else "missing")
                              for n in (resolved.get("expected_executions", resolved["tests"]) if resolved else
                                        [n for c in checks.values() for n in c["expected_executions"]])},
                          "evidence": [_retained_file(p) for p in sorted(set(map(str, paths)))],
                          "backend_receipt": raw}}
    for key in ("phase_context", "phase_context_hash"):
        if key in state.get("realized", {}):
            result[key] = state["realized"][key]
    try:
        result["failure"]["source_status"] = _source_status(args.repo, (args.build_dir, args.evidence_dir))
    except (OSError, ValueError) as error:
        result["failure"]["source_status_error"] = type(error).__name__ + ": " + str(error)
    return result


def realization_command(args, policy, backend, *, custody=None, retain_lane_failures=False):
    # Library adapters keep their existing exception/composition contract. The
    # CLI opts into retained negatives; this cannot authorize a peer adapter.
    if args.command != "run" or not retain_lane_failures:
        return _realization_command(args, policy, backend, custody=custody)
    # Only an admitted plan/profile can name a lane. Invalid plans never acquire
    # authorization by passing through an exception handler.
    plan = selection.validate_plan(selection.read_json(args.plan), policy)
    lane = next((x for x in plan["lanes"] if x["profile"] == args.profile), None)
    selection.require(lane is not None, "run profile is not selected by plan")
    selection.require(args.evidence_dir is not None and args.attempt > 0, "run requires evidence directory and positive attempt")
    # A valid logical plan does not authorize a different checkout or a build
    # realized elsewhere. Preserve rejection without a lane execution receipt.
    if selection.commit_sha(args.repo, "HEAD") != plan["source_identity"]["sha"]:
        if plan["source_identity"]["kind"] == "committed":
            raise selection.ContractError("execution checkout is not the selected committed SHA")
        raise selection.CheckFailure("source checkout HEAD does not match execution identity")
    if not args.configure and args.phase != "configure":
        record = Path(args.build_dir) / "nostrc-test-realization.json"
        if record.is_file():
            realized = selection.read_json(record)
            root = Path(args.repo).resolve()
            paths = {"repo_root": root, "source_dir": (root / backend._expand_source(policy["profiles"][args.profile]["source_dir"], root)).resolve(),
                     "build_dir": Path(args.build_dir).resolve()}
            for field, expected in paths.items():
                if Path(realized[field]).resolve() != expected:
                    raise selection.CheckFailure(f"realization {field} does not match admitted execution checkout: {expected}")
    directory = Path(args.evidence_dir) / plan["source_identity"]["sha"] / args.profile / plan["plan_id"] / f"lane-{args.phase}-attempt-{args.attempt}"
    directory.mkdir(parents=True, exist_ok=False)
    state = {"directory": directory, "stage": "source_validation", "audits": [], "evidence_paths": [],
             "work_status": {p: "not_executed" for p in ("configure", "build", "test")}}
    previous_handler = None
    def cancel(signum, frame):
        raise KeyboardInterrupt(f"lane execution cancelled by signal {signum}")
    try:
        try:
            previous_handler = signal.signal(signal.SIGTERM, cancel)
        except ValueError:
            pass  # Library callers outside the main thread own cancellation.
        return _realization_command(args, policy, backend, custody=custody, state=state)
    except (selection.CheckFailure, selection.ContractError, backend.RealizationError,
            OSError, ValueError, KeyError, TypeError, AttributeError, subprocess.TimeoutExpired, KeyboardInterrupt) as exc:
        if state["stage"] in state["work_status"]:
            state["work_status"][state["stage"]] = "interrupted_or_failed"
        result = _failure_receipt(args, policy, plan, lane, state, exc)
        print(f"test-plan: {exc}", file=sys.stderr)
        code = 1 if isinstance(exc, (selection.CheckFailure, backend.RealizationError, subprocess.TimeoutExpired, KeyboardInterrupt)) else 2
        result["failure"]["cli_exit_code"] = code
        try:
            selection.write_json(directory / "failure.json", result)
        except OSError as error:
            result["failure"]["retention_error"] = type(error).__name__ + ": " + str(error)
        return result, code
    finally:
        if previous_handler is not None:
            signal.signal(signal.SIGTERM, previous_handler)


def _realization_command(args, policy, backend, *, custody=None, state=None):
    """Compose the real realization API; never emulate a configure/audit/run."""
    selection.require(args.phase in ("configure", "build", "test", "all"), "unknown execution phase")
    plan = selection.read_json(args.plan) if args.plan else None
    if plan is not None:
        selection.validate_plan(plan, policy)
        identity = plan["source_identity"]
    else:
        identity = None
    profile = policy["profiles"][args.profile]
    if profile.get("execution_domain") == "prepared_peer_linux_v1":
        from test_selection_custody import PreparedContext
        selection.require(isinstance(custody, PreparedContext), "prepared profile requires the contained Linux runner")
    if identity and identity.get("kind") == "committed":
        selection.require(selection.commit_sha(args.repo, "HEAD") == identity["sha"], "execution checkout is not the selected committed SHA")
        if selection.git(args.repo, "diff", "--quiet", "HEAD", "--", check=False).returncode:
            raise selection.CheckFailure("committed-tree execution refuses dirty tracked inputs")
        excluded = [Path(x).resolve() for x in (args.build_dir, args.evidence_dir) if x]
        for value in selection.git(args.repo, "ls-files", "--others", "--exclude-standard", "-z").stdout.split(b"\0"):
            if value:
                path = (Path(args.repo) / os.fsdecode(value)).resolve()
                if not any(path == root or root in path.parents for root in excluded):
                    raise selection.CheckFailure(f"committed-tree execution refuses untracked source input: {os.fsdecode(value)}")
    if state is not None:
        state["stage"] = "configure" if args.configure or args.phase == "configure" else "load_realization"
    if args.configure or args.phase == "configure":
        selection.require(identity is not None, "configure requires --plan with execution source identity")
        profile_checks = [c for c in policy["checks"].values() if args.profile in c["profiles"]]
        source_only = profile_checks and all(c["kind"] == "command" and not c.get("build_targets") for c in profile_checks)
        configure = backend.configure_command_profile if source_only else backend.configure_profile
        options = {}
        if state is not None and not source_only:
            logfile = state["directory"] / "configure.log"
            state["evidence_paths"].append(logfile)
            options = {"log_path": logfile}
        realized = configure(args.repo, args.build_dir, args.profile, profile, identity, selection.digest(policy), **options)
        if state is not None:
            state["work_status"]["configure"] = "passed"
    else:
        realized = backend.load_realization(args.build_dir, identity)
    if state is not None:
        state["realized"] = realized
        state["stage"] = "realization_validation"
    # Stored identity equality is insufficient: another checkout may share the
    # same commit, or the original build's source may since have advanced.
    repo_root = Path(args.repo).resolve()
    source_dir = (repo_root / backend._expand_source(profile["source_dir"], repo_root)).resolve()
    expected_paths = {"repo_root": repo_root, "source_dir": source_dir,
                      "build_dir": Path(args.build_dir).resolve()}
    for field, expected in expected_paths.items():
        if Path(realized[field]).resolve() != expected:
            raise selection.CheckFailure(f"realization {field} does not match admitted execution checkout: {expected}")
    selection.require(realized["profile_id"] == args.profile and realized["profile"] == profile and realized["inventory_hash"] == selection.digest(policy), "realization policy/profile mismatch")
    if args.command in {"validate", "check-inputs"}:
        audit = backend.check_inputs(policy, realized, identity, after_build=args.after_build,
                                     **({'custody': custody} if custody is not None else {}))
        return audit, 0 if audit["passed"] else 1
    selection.require(plan is not None, "resolve/run requires --plan")
    if state is not None:
        state["stage"] = "resolve"
    resolved = backend.resolve_checks(policy, plan, realized, args.profile)
    if state is not None:
        state["resolved"] = resolved
    if args.resolved:
        supplied = selection.read_json(args.resolved)
        selection.require(supplied == resolved, "supplied resolved plan differs from fresh resolution")
    if args.command == "resolve":
        return resolved, 0
    selection.require(args.evidence_dir is not None, "run requires --evidence-dir")
    selection.require(args.parallel > 0 and (args.timeout is None or args.timeout > 0), "parallel/timeout must be positive")
    if any(e.get("authority") == "reviewed_diagnostic_nonmembers_v1" for e in resolved.get("baseline_exclusions", [])):
        diagnostic_options = backend._diagnostic_execution_options(resolved, realized, args.parallel, args.timeout)
        args.timeout = diagnostic_options["timeout_seconds"]
    if state is not None:
        state["stage"] = "capability"
    # Existing adapters must provision live peers/locks; direct CLI cannot waive them.
    if custody is not None:
        custody.validate(resolved)
    elif set(resolved.get("required_capabilities", [])) & {"docker", "external_peer", "prepared_peer"}:
        raise selection.CheckFailure("live peer/image lifecycle requires the existing guarded adapter")
    if state is not None:
        state["stage"] = "preaudit"
    audit = backend.check_inputs(policy, realized, identity, after_build=False,
                                 **({'custody': custody} if custody is not None else {}))
    if state is not None:
        state["audits"].append(audit)
        auditpath = state["directory"] / "preaudit.json"
        selection.write_json(auditpath, audit)
        state["evidence_paths"].append(auditpath)
    if not audit["passed"]:
        if state is None:
            return audit, 1
        raise selection.CheckFailure("pre-build input audit failed; original findings retained")
    if args.phase == "configure":
        return {"schema_version": 1, "phase": "configure", "resolved": resolved, "input_audit": audit, "execution_complete": False}, 0
    build_seconds, build_commands, build_evidence = 0.0, [], []
    if args.phase in {"all", "build"} and resolved["build_targets"]:
        if state is not None:
            state["stage"] = "build"
            state["work_status"]["build"] = "started"
        command = ["cmake", "--build", args.build_dir, "--parallel", str(args.parallel)]
        if resolved.get("configuration"):
            command += ["--config", resolved["configuration"]]
        command += ["--target", *resolved["build_targets"]]
        directory = Path(args.evidence_dir) / identity["sha"] / args.profile / plan["plan_id"]
        directory.mkdir(parents=True, exist_ok=True)
        logfile = directory / f"build-attempt-{args.attempt}.log"
        started = time.monotonic()
        if state is not None:
            state["evidence_paths"].append(logfile)
        with logfile.open("x"):
            pass  # reserve this exact attempt; never overwrite previous evidence
        try:
            process = backend._execute(command, cwd=args.repo, env=backend.phase_environment(profile, repo_root, "build"),
                                       log_path=logfile, timeout=args.timeout)
            returncode = process.returncode
        except (subprocess.TimeoutExpired, KeyboardInterrupt, backend.RealizationError) as exc:
            raise selection.CheckFailure(f"selected build interrupted/failed; retained {logfile} and process status: {exc}") from exc
        build_seconds = time.monotonic() - started
        build_commands.append(command)
        build_evidence.append(str(logfile))
        if returncode:
            raise selection.CheckFailure(f"selected prerequisite build failed: {returncode}; retained {logfile}")
        if state is not None:
            state["work_status"]["build"] = "passed"
    if state is not None:
        state["stage"] = "postaudit"
    realized = backend.load_realization(args.build_dir, identity)
    audit = backend.check_inputs(policy, realized, identity, after_build=True,
                                 **({'custody': custody} if custody is not None else {}))
    if state is not None:
        state["audits"].append(audit)
        auditpath = state["directory"] / "postaudit.json"
        selection.write_json(auditpath, audit)
        state["evidence_paths"].append(auditpath)
    if not audit["passed"]:
        if state is None:
            return audit, 1
        raise selection.CheckFailure("post-build input audit failed; original findings retained")
    if args.phase in {"configure", "build"}:
        return {"schema_version": 1, "phase": args.phase, "resolved": resolved, "input_audit": audit, "execution_complete": False}, 0
    if state is not None:
        state["stage"] = "test"
        state["work_status"]["test"] = "started"
        path = backend._attempt_directory(resolved, args.evidence_dir, args.attempt) / "receipt.json"
        if not path.parent.exists():
            state["backend_receipt_path"] = str(path)
    raw = backend.run_resolved(resolved, args.build_dir, args.evidence_dir, args.attempt, input_audit=audit,
                               build=False, parallel=args.parallel, timeout=args.timeout, evidence_paths=args.evidence_path,
                               **({"custody": custody} if custody is not None else {}))
    raw["build_seconds"] = build_seconds
    raw["configure_seconds"] = realized.get("configure_seconds")
    raw["commands"] = build_commands + raw.get("commands", [])
    raw["build_evidence"] = build_evidence
    if state is not None:
        state["stage"] = "normalize"
        state["work_status"]["test"] = raw.get("outcome", "unknown")
    result = normalize_receipt(policy, plan, resolved, raw)
    return result, 0 if raw.get("outcome") == "passed" else 1


def normalize_receipt(policy, plan, resolved, raw):
    """Retain backend evidence and validate it before adding logical lane identity."""
    profile_id = resolved["profile_id"]
    if any(e.get("authority") == "reviewed_diagnostic_nonmembers_v1" for e in resolved.get("baseline_exclusions", [])):
        configuration = policy["profiles"][profile_id]["build_type"]
        selection.require(raw.get("configuration") == resolved.get("configuration") == configuration
                          == resolved.get("phase_context", {}).get("configuration"),
                          "backend diagnostic configuration differs from resolved/phase/profile configuration")
    lane = next((x for x in plan["lanes"] if x["profile"] == profile_id), None)
    selection.require(lane is not None, "backend returned unselected profile")
    for key in ("source_identity", "inventory_hash", "profile_fingerprint", "phase_context", "phase_context_hash"):
        selection.require(raw.get(key) == resolved.get(key), f"backend receipt identity mismatch: {key}")
    selection.require(raw.get("plan_hash") == selection.digest(plan) and raw.get("profile_id") == profile_id, "backend plan/profile identity mismatch")
    selection.require(raw.get("resolved_hash") == resolved["resolved_hash"], "backend resolved identity mismatch")
    selection.require(set(raw.get("checks", {})) == set(lane["checks"]), "backend receipt check accounting mismatch")
    observed = raw.get("observed_executions", {})
    selection.require(isinstance(observed, dict) and set(observed) == set(resolved.get("expected_executions", resolved["tests"])), "backend receipt execution accounting mismatch")
    post = raw.get("input_audit", {})
    selection.require(post.get("passed") and post.get("after_build"), "backend did not retain passing post-build audit")
    for key in ("phase_context", "phase_context_hash"):
        selection.require(post.get(key) == resolved.get(key), f"backend input audit identity mismatch: {key}")
    checks = {}
    for ident, record in raw["checks"].items():
        check = policy["checks"][ident]
        names = check["ctest_tests"][profile_id] if check["kind"] == "ctest" else [ident]
        selection.require(set(record.get("executions", [])) == set(names), "backend primary execution membership mismatch")
        actual_outcome = next((observed[n] for n in sorted(names) if observed[n] != "passed"), "passed")
        selection.require(record.get("outcome") == actual_outcome, "backend check/observed outcome contradiction")
        checks[ident] = dict(record, expected_executions=names, observed_executions=[n for n in names if observed.get(n) != "missing"])
    return {"schema_version": 1, "plan_id": plan["plan_id"], "inventory_hash": plan["inventory_hash"],
            "source_identity": plan["source_identity"], "lane_id": lane["id"], "profile": profile_id,
            "profile_hash": lane["profile_hash"], "profile_fingerprint": resolved["profile_fingerprint"],
            "phase_context": resolved.get("phase_context"), "phase_context_hash": resolved.get("phase_context_hash"),
            "registration_audit": "passed", "input_audit": "passed", "job_conclusion": "success" if raw.get("outcome") == "passed" else "failure",
            "checks": checks, "backend_receipt": raw,
            "diagnostic_eligible": raw.get("diagnostic_eligible", False),
            "complete_gate_eligible": raw.get("complete_gate_eligible", False),
            "baseline_exclusions": raw.get("baseline_exclusions", []), "coverage_gaps": raw.get("coverage_gaps", []),
            **({"complete": False} if raw.get("complete") is False else {})}


def main(argv=None):
    args = parser().parse_args(argv)
    try:
        args.repo = str(Path(args.repo).resolve())
        inv = Path(args.inventory)
        if not inv.is_absolute():
            inv = Path(args.repo) / inv
        args.inventory = str(inv)
        if args.command in {"plan", "explain", "release-plan"} and not selection.zero_oid(args.head):
            rel = str(inv.relative_to(args.repo))
            sha = selection.commit_sha(args.repo, args.head)
            policy = selection.validate_inventory(json.loads(selection.git(args.repo, "show", f"{sha}:{rel}").stdout, object_pairs_hook=selection._unique_object))
        else:
            policy = selection.load_inventory(inv)
        if args.command in {"resolve", "check-inputs", "run"} or (args.command == "validate" and args.build_dir):
            selection.require(args.profile is not None and args.profile in policy["profiles"], "realization requires a supported --profile")
            selection.require(args.attempt > 0, "attempt must be positive")
            if args.plan:
                selection.validate_plan(selection.read_json(args.plan), policy)
            try:
                realization = importlib.import_module("test_selection_realization")
            except ImportError as exc:
                raise selection.ContractError("real realization module unavailable; no execution/validation was performed") from exc
            try:
                result, code = realization_command(args, policy, realization, retain_lane_failures=True)
            except realization.RealizationError as exc:
                raise selection.CheckFailure(str(exc)) from exc
            if args.output:
                selection.write_json(args.output, result)
            else:
                print(json.dumps(result, sort_keys=True, indent=2))
            return code
        if args.command == "validate":
            # Surface tracked coverage without treating metadata as live acceptance.
            tracked = selection.git(args.repo, "ls-files", "-z").stdout.split(b"\0")
            import os
            unclassified = sorted(os.fsdecode(p) for p in tracked if p and selection.classify(policy, os.fsdecode(p))["unclassified"])
            result = {"schema_version": 1, "valid": not unclassified, "inventory_hash": selection.digest(policy),
                      "unclassified_paths": unclassified, "coverage_gaps": policy.get("coverage_gaps", []),
                      "enforcement_ready": policy.get("enforcement_ready", False)}
            code = 1 if unclassified or (args.strict and (policy.get("coverage_gaps") or not policy.get("enforcement_ready", False))) else 0
            if code:
                result["valid"] = False
        elif args.command in {"plan", "explain"}:
            rel = str(inv.relative_to(args.repo))
            result = selection.git_plan(args.repo, policy, head=args.head, base=args.base, context=args.context, mode=args.mode,
                                        event=selection.read_json(args.event) if args.event else None, profiles=args.profile, inventory_path=rel)
            code = 0
            if args.command == "explain" and not args.output:
                print(f"{result['requested_mode']} -> {result['effective_mode']}: {len(result['selected_checks'])} checks in {len(result['lanes'])} lanes")
                for reason in result["broadening_reasons"]:
                    print(f"  broad: {reason}")
                for lane in result["lanes"]:
                    print(f"  {lane['id']}: {', '.join(lane['checks'])}")
                return 0
        elif args.command == "aggregate":
            plan = selection.read_json(args.plan)
            if args.package_results:
                selection.require(args.release_validation is not None, "release aggregation requires --release-validation")
                result = selection.aggregate_release(plan, selection.read_json(args.release_validation), selection.read_json(args.package_results))
            else:
                result = selection.aggregate(policy, plan, [selection.read_json(p) for p in args.receipt], complete=not args.partial,
                                             selection_conclusion=args.selection_conclusion,
                                             job_conclusions=selection.read_json(args.job_results) if args.job_results else None, repo=args.repo)
            code = 0 if result["passed"] else 1
        elif args.command == "release-plan":
            result = selection.release_plan(args.repo, policy, head=args.head, tag=args.tag, component=args.component, dry_run=args.dry_run, run_id=args.run_id)
            code = 0
        else:
            raise selection.ContractError("unsupported command")
        if args.output:
            selection.write_json(args.output, result)
        else:
            print(json.dumps(result, sort_keys=True, indent=2, ensure_ascii=True))
        return code
    except selection.CheckFailure as exc:
        print(f"test-plan: {exc}", file=sys.stderr)
        return 1
    except (selection.ContractError, OSError, ValueError, KeyError, TypeError, AttributeError) as exc:
        print(f"test-plan: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
