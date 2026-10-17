#!/usr/bin/env python3
"""Same-run Actions orchestration; selection and receipt policy stay in test_selection."""
import argparse
import json
import os
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))
import test_selection as selection

# Each lane is a separate reusable call so needs supplies an independent conclusion.
LANES = {"core.linux": "core", "core.linux.asan": "core_asan", "core.macos.asan": "core_native",
         "grotto.linux": "grotto", "groundhog.linux": "groundhog",
         "groundhog.linux.sanitizers": "groundhog_sanitizers", "groundhog.mdk08": "mdk08",
         "groundhog.mdk011": "mdk011",
         "libgo.linux.asan": "libgo", "libgo.macos.asan": "libgo_native", "static.policy": "policy"}
FAMILIES = ("nostrc-ci", "libgo-ci", "libhanami-ci", "libnostr-gi", "groundhog-ci", "grotto-ci",
            "nip55l-libsecret-ci", "nostr-homed-ci", "signet-ci", "static-checks", "gnostr-appimage",
            "windows-validation", "bp-matrix", "bp-matrix-nightly", "grotto-fuzz", "marmot-mdk-interop",
            "marmot-mdk011-interop")
PUBLISHER_JOBS = {
    "main": ("build-source", "build-linux", "build-linux-daemon-tcp", "build-appimage-gnostr",
             "build-snap-gnostr", "build-snap-signer", "build-snap-daemon", "build-flatpak",
             "build-flatpak-gnostr", "build-macos-signer-dmg", "build-macos-tarball"),
    "groundhog": ("source", "deb", "rpm", "arch", "flatpak", "nix"),
    "windows": ("build",),
}


def fuzz_selection(target="all", duration=None, extended=False):
    targets = ["nip49", "bip39", "json-event"]
    selection.require(target == "all" or target in targets, "unknown fuzz target")
    duration = str(duration) if duration is not None else ("300" if extended else "60")
    selection.require(duration.isascii() and duration.isdigit() and 1 <= int(duration) <= 3600,
                      "fuzz duration must be an integer from 1 to 3600")
    return targets if target == "all" else [target], int(duration)


def metadata(name, event, sha, mode="affected", tag=""):
    if tag:
        return {"kind": "release", "tested_sha": sha, "tag": tag}, "broad", "release"
    if name == "pull_request":
        pr = event["pull_request"]
        return {"kind": name, "base_sha": pr["base"]["sha"], "pr_head_sha": pr["head"]["sha"],
                "tested_sha": sha}, mode, "ci"
    if name == "merge_group":
        group = event["merge_group"]
        selection.require(group["head_sha"] == sha, "merge-group tested SHA mismatch")
        return {"kind": name, "base_sha": group["base_sha"], "tested_sha": sha}, mode, "ci"
    if name == "push":
        selection.require(not event.get("deleted"), "deleted ref has no execution")
        return {"kind": "push", "base_sha": event["before"], "tested_sha": sha}, "broad", "ci"
    selection.require(name in {"workflow_dispatch", "workflow_call"}, "unsupported Actions event")
    return {"kind": "manual", "base_sha": event.get("inputs", {}).get("base") or None,
            "tested_sha": sha}, mode, "ci"


def route_release(repo, policy, sha, tag, component=None, dry_run=False, run_id=None):
    release = selection.release_plan(repo, policy, head=sha, tag=tag or None, component=component,
                                     dry_run=dry_run, run_id=run_id)
    route = "validation_only" if release["validation_only"] else ",".join(release["publishers"])
    return release, route


def verify_checkout(repo, sha):
    selection.require(selection.commit_sha(repo, sha) == sha, "requested identity is not a full commit SHA")
    selection.require(selection.commit_sha(repo, "HEAD") == sha, "checkout is not requested SHA")


def check_jobs(plan, needs):
    expected = set(plan["expected_lane_ids"])
    selection.require(expected <= LANES.keys(), "profile has no Actions worker; coverage incomplete")
    errors = []
    for lane, job in LANES.items():
        wanted = "success" if lane in expected else "skipped"
        if needs.get(job, {}).get("result") != wanted:
            errors.append(f"{lane}: expected {wanted}, observed {needs.get(job, {}).get('result', 'missing')}")
    broad = plan["effective_mode"] == "broad"
    if needs.get("broad", {}).get("result") != ("success" if broad else "skipped"):
        errors.append("broad family coverage did not complete as selected")
    return errors, {lane: needs.get(LANES[lane], {}).get("result", "missing") for lane in expected}


def aggregate(repo, policy, plan, needs, receipts):
    selection.validate_plan(plan, policy)
    errors, jobs = check_jobs(plan, needs)
    result = selection.aggregate(policy, plan, receipts, selection_conclusion=needs.get("select", {}).get("result"),
                                 job_conclusions=jobs, repo=repo)
    result["errors"].extend(errors)
    # A partial policy can give useful shadow evidence, never a complete coverage attestation.
    if not plan.get("enforcement_ready") or plan.get("coverage_gaps"):
        result["errors"].append("inventory/profile coverage is incomplete; W4/W11 acceptance not established")
    result["passed"] = not result["errors"]
    return result


def package_conclusions(release, publisher, needs):
    selection.require(publisher in release["publishers"], "publisher does not own release identity")
    required = release["required_package_jobs"]
    owned = {}
    for job in PUBLISHER_JOBS[publisher]:
        key = "source" if job == "build-source" else "windows.build" if publisher == "windows" else job
        keys = [x for x in required if x == key or x.startswith(key + ".")]
        result = needs.get(job, {}).get("result", "missing")
        if keys:
            selection.require(result == "success", f"applicable package {job}: {result}")
            # The Actions matrix conclusion requires all rows; fail-fast stays disabled.
            owned.update({x: result for x in keys})
        else:
            selection.require(result == "skipped", f"inapplicable package {job}: {result}")
    expected = {x for x in required if (x == "windows.build") == (publisher == "windows")}
    selection.require(set(owned) == expected, "unmapped applicable package rows")
    return owned


def package_artifacts(release, publisher, artifacts):
    names = set()
    for key in release['required_package_jobs']:
        if (key == 'windows.build') != (publisher == 'windows'):
            continue
        if key.startswith("nix."):
            continue
        job, _, suffix = key.partition(".")
        mapping = {"source": "source", "build-linux": "linux-packages", "build-linux-daemon-tcp": "linux-daemon-tcp",
                   "build-appimage-gnostr": "appimage-gnostr", "build-snap-gnostr": "snap-gnostr",
                   "build-snap-signer": "snap-grotto", "build-snap-daemon": "snap-grotto-daemon",
                   "build-flatpak": "flatpak", "build-flatpak-gnostr": "flatpak-gnostr",
                   "build-macos-signer-dmg": "macos-dmg-signer", "build-macos-tarball": "macos-tarball",
                   "deb": "deb", "rpm": "rpm", "arch": "arch", "flatpak": "flatpak"}
        if key == "windows.build":
            names.add(f"GNostrSigner-{release['version']}-x64-setup")
            continue  # Portable is retained as a run artifact; historically only exe assets publish.
        selection.require(job in mapping, f"unmapped package artifact: {key}")
        if job == "build-appimage-gnostr":
            names.update({"appimage-gnostr-x86_64", "appimage-gnostr-aarch64"})
        else:
            names.add(mapping[job] + ("-" + "-".join(suffix.rsplit(".", 1)) if suffix else ""))
    found = {}
    for artifact in artifacts:
        if artifact.get("name") in names:
            selection.require(artifact["name"] not in found and not artifact.get("expired") and artifact.get("size_in_bytes", 0) > 0,
                              "duplicate/expired/empty package artifact")
            found[artifact["name"]] = str(artifact["id"])
    selection.require(set(found) == names, f"missing applicable artifacts: {sorted(names - found.keys())}")
    return [found[name] for name in sorted(names)]


def publish_gate(release, publisher, validation, needs, release_mode="enforce"):
    selection.require(release_mode in {"disabled", "shadow", "enforce"}, "invalid release rollout mode")
    packages = package_conclusions(release, publisher, needs)
    if release_mode != "enforce":
        # Explicit rollout, not a forged validation receipt. Legacy publication remains available
        # until the operator enables enforcement after real checked-in-lane/broad evidence.
        return {"schema_version": 1, "passed": True, "publish": release["publish"], "errors": [],
                "validation_required": False, "validation_status": "staged_" + release_mode,
                "validation_conclusion": needs.get("validation", {}).get("result", "missing"),
                "validation_passed": validation.get("passed") if validation else None}
    selection.require(needs.get("validation", {}).get("result") == "success", "release validation job did not succeed")
    selection.require(isinstance(validation, dict), "required release validation receipt is missing")
    selection.require(validation.get("passed") and validation.get("complete") and validation.get("enforcement_ready")
                      and not validation.get("coverage_gaps"), "required release coverage is incomplete")
    # Legacy owners attest independently in the same run, never borrow each other's artifacts.
    scoped = dict(release, required_package_jobs=sorted(packages))
    result = selection.aggregate_release(scoped, validation, packages)
    selection.require(result["passed"], "; ".join(result["errors"]))
    result.update(validation_required=True, validation_status="enforced")
    return result


def outputs(values):
    with open(os.environ["GITHUB_OUTPUT"], "a") as stream:
        for key, value in values.items():
            value = json.dumps(value, separators=(",", ":")) if not isinstance(value, str) else value
            selection.require("\n" not in value and "\r" not in value, "invalid multiline Actions output")
            stream.write(f"{key}={value}\n")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=("select", "aggregate", "release-identity", "publish-gate", "verify", "fuzz"))
    parser.add_argument("--sha", default=os.environ.get("GITHUB_SHA"))
    parser.add_argument("--mode", choices=("affected", "broad"), default="affected")
    parser.add_argument("--tag", default="")
    parser.add_argument("--component")
    parser.add_argument("--fuzz-target", default="all")
    parser.add_argument("--duration")
    parser.add_argument("--publisher", choices=tuple(PUBLISHER_JOBS))
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument("--directory", default=os.environ.get("RUNNER_TEMP", "/tmp") + "/dependency")
    parser.add_argument("--needs")
    parser.add_argument("--artifacts")
    parser.add_argument("--release-mode", choices=("disabled", "shadow", "enforce"), default="enforce")
    args = parser.parse_args()
    directory = Path(args.directory)
    directory.mkdir(parents=True, exist_ok=True)
    try:
        if args.command in {"select", "release-identity"}:
            # GITHUB_SHA can name an annotated tag object. Workers receive only its peeled commit.
            args.sha = selection.commit_sha(ROOT, args.sha)
        verify_checkout(ROOT, args.sha)
        if args.command == "verify":
            return 0
        if args.command == "fuzz":
            targets, duration = fuzz_selection(args.fuzz_target, args.duration or None, args.mode == "broad")
            outputs({"targets": targets, "duration": str(duration)})
            return 0
        policy = selection.load_inventory(ROOT / "tests/selection/inventory.json")
        if args.command in {"select", "release-identity"}:
            ref = os.environ.get("GITHUB_REF", "")
            tag = args.tag or (ref.removeprefix("refs/tags/") if ref.startswith("refs/tags/") else "")
            route = "checks"
            if tag or args.command == "release-identity":
                component = args.component or ("global" if args.command == "release-identity" and not tag else None)
                release, route = route_release(ROOT, policy, args.sha, tag, component, args.dry_run,
                                               os.environ.get("GITHUB_RUN_ID"))
                selection.write_json(directory / "release.json", release)
                if args.command == "release-identity":
                    if args.publisher:
                        selection.require(args.publisher in release["publishers"], "workflow is not this tag's publisher")
                    component = "global" if release["identity"] == "legacy_global" else release["identity"]
                    outputs({"sha": args.sha, "component": component,
                             "version": release["version"] or "0.0.0.git" + args.sha[:8], "tag": tag})
                    return 0
            event = selection.read_json(os.environ["GITHUB_EVENT_PATH"])
            info, mode, context = metadata(os.environ["GITHUB_EVENT_NAME"], event, args.sha, args.mode, args.tag)
            if tag and route == "validation_only":
                info, mode, context = metadata("workflow_call", {}, args.sha, "broad", tag)
            plan = selection.git_plan(ROOT, policy, head=args.sha, context=context, mode=mode, event=info)
            selection.require(set(plan["expected_lane_ids"]) <= LANES.keys(), "inventory profile lacks Actions worker")
            selection.write_json(directory / "plan.json", plan)
            run = route in {"checks", "validation_only"} or bool(args.tag)
            outputs({"sha": args.sha, "route": route, "run": str(run).lower(), "plan_id": plan["plan_id"],
                     "lanes": plan["expected_lane_ids"], "broad": str(run and plan["effective_mode"] == "broad").lower()})
            return 0
        needs = selection.read_json(args.needs)
        if args.command == "aggregate":
            plan = selection.read_json(directory / "plan.json")
            receipts = [selection.read_json(p) for p in sorted((directory / "receipts").glob("**/receipt.json"))]
            result = aggregate(ROOT, policy, plan, needs, receipts)
            selection.write_json(directory / "validation.json", result)
        else:
            release = selection.read_json(directory / "release.json")
            expected_release = selection.release_plan(ROOT, policy, head=args.sha, tag=release['tag'],
                                                       component=release['identity'], dry_run=release['dry_run'],
                                                       run_id=release.get('run_id'))
            selection.require(release == expected_release, "release identity artifact does not match exact checkout/policy")
            validation_path = directory / "validation.json"
            validation = selection.read_json(validation_path) if validation_path.exists() else None
            result = publish_gate(release, args.publisher, validation, needs, args.release_mode)
            pages = selection.read_json(args.artifacts)
            artifacts = [a for page in pages for a in page["artifacts"]]
            # Use actual applicability here, including deliberately skipped jobs.
            ids = package_artifacts(release, args.publisher, artifacts)
            result.update(gate="publication", package_artifact_ids=ids, tested_sha=args.sha, tag=release['tag'])
            selection.write_json(directory / "publication.json", result)
            outputs({"publish": str(result["publish"]).lower(), "artifact_ids": ",".join(ids)})
            summary = os.environ.get('GITHUB_STEP_SUMMARY')
            if summary:
                with open(summary, 'a') as stream:
                    stream.write(f"Publication gate: {result['validation_status']}; validation required: {result['validation_required']}\\n")
        print(json.dumps(result, indent=2))
        return 0 if result["passed"] else 1
    except (selection.ContractError, selection.CheckFailure, OSError, ValueError, KeyError) as exc:
        print(f"dependency-workflows: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
