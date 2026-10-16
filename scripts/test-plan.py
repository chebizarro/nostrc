#!/usr/bin/env python3
"""Shared dependency-aware test planner. See --help for explicit contracts."""
import argparse
import importlib
import json
import os
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


def realization_command(args, policy, backend, *, custody=None):
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
    if args.configure or args.phase == "configure":
        selection.require(identity is not None, "configure requires --plan with execution source identity")
        profile_checks = [c for c in policy["checks"].values() if args.profile in c["profiles"]]
        source_only = profile_checks and all(c["kind"] == "command" and not c.get("build_targets") for c in profile_checks)
        configure = backend.configure_command_profile if source_only else backend.configure_profile
        realized = configure(args.repo, args.build_dir, args.profile, profile, identity, selection.digest(policy))
    else:
        realized = backend.load_realization(args.build_dir, identity)
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
        audit = backend.check_inputs(policy, realized, identity, after_build=args.after_build)
        return audit, 0 if audit["passed"] else 1
    selection.require(plan is not None, "resolve/run requires --plan")
    resolved = backend.resolve_checks(policy, plan, realized, args.profile)
    if args.resolved:
        supplied = selection.read_json(args.resolved)
        selection.require(supplied == resolved, "supplied resolved plan differs from fresh resolution")
    if args.command == "resolve":
        return resolved, 0
    selection.require(args.evidence_dir is not None, "run requires --evidence-dir")
    selection.require(args.parallel > 0 and (args.timeout is None or args.timeout > 0), "parallel/timeout must be positive")
    # Existing adapters must provision live peers/locks; direct CLI cannot waive them.
    if custody is not None:
        custody.validate(resolved)
    elif set(resolved.get("required_capabilities", [])) & {"docker", "external_peer"}:
        raise selection.CheckFailure("live peer/image lifecycle requires the existing guarded adapter")
    audit = backend.check_inputs(policy, realized, identity, after_build=False)
    if not audit["passed"]:
        return audit, 1
    if args.phase == "configure":
        return {"schema_version": 1, "phase": "configure", "resolved": resolved, "input_audit": audit, "execution_complete": False}, 0
    build_seconds, build_commands, build_evidence = 0.0, [], []
    if args.phase in {"all", "build"} and resolved["build_targets"]:
        command = ["cmake", "--build", args.build_dir, "--parallel", str(args.parallel)]
        if resolved.get("configuration"):
            command += ["--config", resolved["configuration"]]
        command += ["--target", *resolved["build_targets"]]
        directory = Path(args.evidence_dir) / identity["sha"] / args.profile / plan["plan_id"]
        directory.mkdir(parents=True, exist_ok=True)
        logfile = directory / f"build-attempt-{args.attempt}.log"
        started = time.monotonic()
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
    realized = backend.load_realization(args.build_dir, identity)
    audit = backend.check_inputs(policy, realized, identity, after_build=True)
    if not audit["passed"]:
        return audit, 1
    if args.phase in {"configure", "build"}:
        return {"schema_version": 1, "phase": args.phase, "resolved": resolved, "input_audit": audit, "execution_complete": False}, 0
    raw = backend.run_resolved(resolved, args.build_dir, args.evidence_dir, args.attempt, input_audit=audit,
                               build=False, parallel=args.parallel, timeout=args.timeout, evidence_paths=args.evidence_path,
                               **({"custody": custody} if custody is not None else {}))
    raw["build_seconds"] = build_seconds
    raw["configure_seconds"] = realized.get("configure_seconds")
    raw["commands"] = build_commands + raw.get("commands", [])
    raw["build_evidence"] = build_evidence
    result = normalize_receipt(policy, plan, resolved, raw)
    return result, 0 if raw.get("outcome") == "passed" else 1


def normalize_receipt(policy, plan, resolved, raw):
    """Retain backend evidence and validate it before adding logical lane identity."""
    profile_id = resolved["profile_id"]
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
            "baseline_exclusions": raw.get("baseline_exclusions", []), "coverage_gaps": raw.get("coverage_gaps", [])}


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
                result, code = realization_command(args, policy, realization)
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
