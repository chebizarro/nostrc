"""Dependency-selection policy, Git ranges and receipts (stdlib only).

Build realization/execution belongs to test_selection_realization, not this module.
"""
from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import subprocess
import tempfile
from collections import deque

SCHEMA_VERSION = 1
ID = re.compile(r"[a-z0-9_]+(?:\.[a-z0-9_]+)*\Z")
SEMVER = re.compile(r"(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)(?:-([0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*))?(?:\+([0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*))?\Z")
OUTCOMES = frozenset({"passed", "failed", "cancelled", "missing", "unexpected_skip", "expected_incompatibility", "quarantined_failure", "advisory_failure", "not_applicable"})


class ContractError(ValueError):
    """Malformed/uninterpretable contract; CLI exit 2."""


class CheckFailure(ValueError):
    """Interpretable contract failed verification; CLI exit 1."""


def require(condition, message):
    if not condition:
        raise ContractError(message)


def canonical(value):
    return json.dumps(value, sort_keys=True, ensure_ascii=True, separators=(",", ":"), allow_nan=False).encode("ascii")


def digest(value):
    return hashlib.sha256(canonical(value)).hexdigest()


def read_json(path):
    try:
        return json.loads(Path(path).read_text(), object_pairs_hook=_unique_object)
    except (OSError, ValueError) as exc:
        raise ContractError(f"cannot read JSON {path}: {exc}") from exc


def _unique_object(pairs):
    result = {}
    for key, value in pairs:
        require(key not in result, f"duplicate JSON key: {key}")
        result[key] = value
    return result


def write_json(path, value):
    """A killed writer must not leave an apparently complete receipt."""
    target = Path(path)
    target.parent.mkdir(parents=True, exist_ok=True)
    fd, tmp = tempfile.mkstemp(prefix=f".{target.name}.", dir=target.parent)
    try:
        with os.fdopen(fd, "w") as stream:
            json.dump(value, stream, sort_keys=True, indent=2, ensure_ascii=True, allow_nan=False)
            stream.write("\n")
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(tmp, target)
    finally:
        if os.path.exists(tmp):
            os.unlink(tmp)


def normalized_path(path):
    require(isinstance(path, str) and path and "\x00" not in path, "invalid repository path")
    require(not path.startswith("/") and all(p not in {"", ".", ".."} for p in path.split("/")), f"non-normalized path: {path!r}")
    return path


def _ids(value, known, description):
    require(isinstance(value, list) and all(isinstance(x, str) for x in value), f"{description}: expected string array")
    require(len(value) == len(set(value)), f"{description}: duplicate ID")
    require(not (set(value) - set(known)), f"{description}: unknown IDs {sorted(set(value) - set(known))}")


def validate_inventory(policy):
    require(isinstance(policy, dict) and type(policy.get("schema_version")) is int and policy["schema_version"] == 1, "unsupported inventory schema")
    for key in ("nodes", "profiles", "checks", "release_components"):
        require(isinstance(policy.get(key), dict), f"missing map: {key}")
        for ident, value in policy[key].items():
            # Canonical release IDs retain their public hyphens; internal IDs do not.
            pattern = r"[a-z0-9]+(?:-[a-z0-9]+)*" if key == "release_components" else ID.pattern
            require(isinstance(ident, str) and re.fullmatch(pattern, ident) and isinstance(value, dict), f"invalid {key} ID: {ident}")
    nodes, profiles, checks = (policy[k] for k in ("nodes", "profiles", "checks"))
    for ident, node in nodes.items():
        require(node.get("id", ident) == ident, f"node ID mismatch: {ident}")
        _ids(node.get("depends_on", []), nodes, f"{ident}.depends_on")
        require(isinstance(node.get("owner_paths"), list) and node["owner_paths"], f"{ident}: missing owner_paths")
        for path in node["owner_paths"]:
            normalized_path(path.rstrip("/"))
        if node.get("depends_on"):
            require(bool(node.get("rationale")), f"{ident}: dependency rationale required")
    _validate_cycles(nodes)
    require(isinstance(policy.get("path_rules"), list), "missing path_rules")
    effects = {}
    for rule in policy["path_rules"]:
        require(isinstance(rule, dict) and (("exact" in rule) != ("prefix" in rule)), "rule needs exactly one exact/prefix")
        path = rule.get("exact", rule.get("prefix")).rstrip("/")
        normalized_path(path)
        require(rule.get("effect") in {"impact", "global", "non_execution"} and bool(rule.get("reason")), f"invalid rule: {path}")
        _ids(rule.get("nodes", []), nodes, f"rule {path}")
        require(rule["nodes"] or rule["effect"] != "impact", f"impact rule has no owner: {path}")
        require(path not in effects or effects[path] == rule["effect"], f"contradictory equal-specificity effects: {path}")
        effects[path] = rule["effect"]
    for ident, profile in profiles.items():
        for key in ("platform", "architecture", "compiler", "build_type", "instrumentation", "source_dir", "setup", "local_availability"):
            require(isinstance(profile.get(key), str) and profile[key], f"{ident}: missing {key}")
        require(isinstance(profile.get("configure_args"), list) and all(isinstance(x, str) for x in profile["configure_args"]), f"{ident}: configure_args")
        require(isinstance(profile.get("environment"), dict) and all(isinstance(k, str) and isinstance(v, str) for k, v in profile["environment"].items()), f"{ident}: environment")
        require(isinstance(profile.get("capabilities"), list), f"{ident}: capabilities")
    claimed = {}
    for ident, check in checks.items():
        _ids(check.get("nodes", []), nodes, f"{ident}.nodes")
        _ids(check.get("profiles", []), profiles, f"{ident}.profiles")
        require(check.get("nodes") and check.get("profiles"), f"{ident}: empty ownership/profiles")
        require(check.get("kind") in {"ctest", "command", "build_contract"}, f"{ident}: invalid kind")
        require(check.get("cost") in {"ordinary", "expensive"} and check.get("requirement") in {"blocking", "advisory"}, f"{ident}: invalid gating")
        expected = check.get("expected_outcome", "pass")
        require(expected == "pass" or (isinstance(expected, dict) and expected.get("case") == "mdk09_incompatibility" and expected.get("exit_code") == 77), f"{ident}: unsupported expected exception")
        if check["kind"] == "ctest":
            membership = check.get("ctest_tests")
            require(isinstance(membership, dict) and set(membership) == set(check["profiles"]), f"{ident}: exact per-profile CTest membership required")
            for profile, names in membership.items():
                require(isinstance(names, list) and names and all(isinstance(x, str) and x for x in names) and len(names) == len(set(names)), f"{ident}: empty/duplicate execution")
                for name in names:
                    require((profile, name) not in claimed, f"multiply claimed primary: {profile}/{name}")
                    claimed[profile, name] = ident
        else:
            require(isinstance(check.get("command"), list) and check["command"] and all(isinstance(x, str) for x in check["command"]), f"{ident}: argument-array command required")
    require(isinstance(policy.get("test_overrides", []), list), "invalid test_overrides")
    for alias, members in policy.get("aliases", {}).items():
        require(ID.fullmatch(alias) is not None, "invalid alias")
        _ids(members, checks, f"alias {alias}")
    return policy


def _validate_cycles(nodes):
    # Tarjan SCCs: only explicitly named conservative groups permit cycles.
    index, low, stack, active = {}, {}, [], set()

    def visit(node):
        index[node] = low[node] = len(index)
        stack.append(node)
        active.add(node)
        for dep in nodes[node].get("depends_on", []):
            if dep not in index:
                visit(dep)
                low[node] = min(low[node], low[dep])
            elif dep in active:
                low[node] = min(low[node], index[dep])
        if low[node] == index[node]:
            group = []
            while True:
                member = stack.pop()
                active.remove(member)
                group.append(member)
                if member == node:
                    break
            if len(group) > 1 or node in nodes[node].get("depends_on", []):
                names = {nodes[m].get("conservative_group") for m in group}
                require(len(names) == 1 and None not in names and all(nodes[m].get("conservative") for m in group), f"undeclared impact cycle: {sorted(group)}")

    for node in sorted(nodes):
        if node not in index:
            visit(node)


def load_inventory(path):
    return validate_inventory(read_json(path))


def classify(policy, path, executable=False):
    normalized_path(path)
    matches = []
    for rule in policy["path_rules"]:
        root = rule.get("exact", rule.get("prefix")).rstrip("/")
        if path == root or ("prefix" in rule and path.startswith(root + "/")):
            matches.append((len(root), rule))
    if not matches:
        return {"path": path, "effect": "global", "nodes": [], "reason": "unclassified path", "unclassified": True}
    size = max(m[0] for m in matches)
    rules = [r for n, r in matches if n == size]
    effect = rules[0]["effect"]
    if effect == "non_execution":
        # Only the narrow documented allowlist is exempt; executable prose is not.
        allowed = path == "LICENSE" or path.startswith(".beads/") or (path.startswith("docs/") and PurePosixPath(path).suffix in {".md", ".rst", ".txt"} and not executable)
        if not allowed:
            return {"path": path, "effect": "global", "nodes": [], "reason": "non-prose input under non-execution rule", "unclassified": True}
    return {"path": path, "effect": effect, "nodes": sorted({n for r in rules for n in r["nodes"]}), "reason": "; ".join(sorted({r["reason"] for r in rules})), "unclassified": False}


def impact_closure(policy, seeds):
    reverse = {node: [] for node in policy["nodes"]}
    for node, value in policy["nodes"].items():
        for dep in value.get("depends_on", []):
            reverse[dep].append(node)
    witnesses = {seed: [seed] for seed in sorted(seeds)}
    queue = deque(sorted(seeds))
    while queue:
        node = queue.popleft()
        for consumer in sorted(reverse[node]):
            if consumer not in witnesses:
                witnesses[consumer] = witnesses[node] + [consumer]
                queue.append(consumer)
    return witnesses


def make_plan(policy, changes, *, head, base=None, context="local", mode="affected", event=None, profiles=None, broadening=None, base_policies=None, source_identity=None):
    validate_inventory(policy)
    require(context in {"local", "ci", "release"} and mode in {"affected", "broad"}, "invalid context/mode")
    restriction = sorted(set(profiles or []))
    _ids(restriction, policy["profiles"], "profile restriction")
    reasons = list(broadening or [])
    if context == "release" and mode != "broad":
        reasons.append("release requires broad validation")
    classifications, seeds = [], set()
    records = sorted({canonical(c): c for c in changes}.values(), key=lambda c: canonical(c))
    for change in records:
        require(isinstance(change.get("status"), str) and change["status"], "missing changed status")
        for side in ("old_path", "new_path"):
            path = change.get(side)
            if path is None:
                continue
            candidates = [("head", policy)]
            if side == "old_path":
                candidates += [(f"base:{i}", p) for i, p in enumerate(base_policies or [])]
            for origin, candidate in candidates:
                item = classify(candidate, path, executable=change.get("old_executable" if side == "old_path" else "executable", False))
                item["policy"] = origin
                classifications.append(item)
                seeds.update(item["nodes"])
                if item["effect"] == "global":
                    reasons.append(f"{path}: {item['reason']}")
                for node in item["nodes"]:
                    if node not in policy["nodes"]:
                        reasons.append(f"removed ownership node: {node}")
                    elif policy["nodes"][node].get("coverage_gap"):
                        reasons.append(f"{node}: {policy['nodes'][node]['coverage_gap']}")
    witnesses = impact_closure(policy, seeds & set(policy["nodes"]))
    effective = "broad" if mode == "broad" or reasons else "affected"
    selected, excluded = [], []
    for ident, check in sorted(policy["checks"].items()):
        if effective == "broad" or set(check["nodes"]) & witnesses.keys():
            selected.append(ident)
        else:
            excluded.append({"check": ident, "reason": "unaffected"})
    lanes = []
    for profile, config in sorted(policy["profiles"].items()):
        members = [c for c in selected if profile in policy["checks"][c]["profiles"]]
        if members and (not restriction or profile in restriction):
            lanes.append({"id": profile, "profile": profile, "profile_hash": digest(config), "checks": members})
    active = {c for lane in lanes for c in lane["checks"]}
    excluded += [{"check": c, "reason": "outside explicit profile projection"} for c in selected if c not in active]
    result = {"schema_version": 1, "context": context, "requested_mode": mode, "effective_mode": effective,
              "base": base, "head": head, "tested_sha": head, "event": event or {},
              "source_identity": source_identity or {"kind": "committed", "sha": head},
              "inventory_hash": digest(policy), "changes": records, "classifications": classifications,
              "broadening_reasons": sorted(set(reasons)), "selected_checks": sorted(active),
              "excluded_checks": excluded, "witnesses": witnesses, "lanes": lanes,
              "expected_lane_ids": [lane["id"] for lane in lanes], "partial_profiles": restriction,
              "coverage_gaps": policy.get("coverage_gaps", []), "enforcement_ready": policy.get("enforcement_ready", False)}
    result["plan_id"] = digest(result)
    return result


def git(repo, *args, check=True):
    try:
        proc = subprocess.run(["git", "-C", str(repo), *args], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    except OSError as exc:
        raise ContractError(f"git unavailable: {exc}") from exc
    if check and proc.returncode:
        raise ContractError(f"git {' '.join(args)} failed: {os.fsdecode(proc.stderr).strip()}")
    return proc


def commit_sha(repo, revision):
    require(isinstance(revision, str) and revision and not revision.startswith("-"), "invalid revision")
    return os.fsdecode(git(repo, "rev-parse", "--verify", revision + "^{commit}").stdout).strip()


def zero_oid(value):
    return isinstance(value, str) and bool(value) and set(value) == {"0"}


def diff_records(repo, base, head):
    fields = git(repo, "diff", "--no-ext-diff", "--no-textconv", "--name-status", "-z", "--find-renames", base, head, "--").stdout.split(b"\0")
    require(fields.pop() == b"", "unterminated git diff")
    records, i = [], 0
    while i < len(fields):
        status = fields[i].decode("ascii")
        i += 1
        require(status and status[0] in "ACDMRTUXB", "invalid git diff status")
        count = 2 if status[0] in "RC" else 1
        require(i + count <= len(fields), "truncated git diff")
        paths = [os.fsdecode(p) for p in fields[i:i + count]]
        i += count
        old = paths[0] if status[0] != "A" else None
        new = paths[-1] if status[0] != "D" else None
        records.append({"status": status, "old_path": old, "new_path": new})
    return records


def git_plan(repo, policy, *, head, base=None, context="local", mode="affected", event=None, profiles=None, inventory_path="tests/selection/inventory.json"):
    events = event if isinstance(event, list) else [event or {"kind": "manual"}]
    reasons, ranges, identities = [], [], []
    head_sha = commit_sha(repo, head) if not zero_oid(head) else head
    deleted = zero_oid(head)
    for metadata in events:
        kind = metadata.get("kind", "manual")
        require(kind in {"manual", "local", "push", "pull_request", "merge_group", "release"}, "unknown event kind")
        if metadata.get("tested_sha"):
            require(commit_sha(repo, metadata["tested_sha"]) == head_sha, "event tested SHA mismatch")
        identities.append(metadata)
        if deleted:
            require(kind in {"local", "push"}, "deletion outside push context")
            continue
        if kind == "pull_request":
            require(all(isinstance(metadata.get(key), str) and metadata[key] for key in ("base_sha", "pr_head_sha", "tested_sha")), "PR requires base, PR head and tested merge identities")
        if kind == "merge_group":
            require(bool(metadata.get("base_sha")), "merge group requires event base SHA")
        if kind == "release":
            mode = "broad"
            require(context == "release", "release event requires release context")
        try:
            if kind == "pull_request":
                pr_head = commit_sha(repo, metadata["pr_head_sha"])
                endpoint = commit_sha(repo, metadata["base_sha"])
                merge_base = os.fsdecode(git(repo, "merge-base", endpoint, pr_head).stdout).strip()
                ranges.extend([(merge_base, pr_head), (endpoint, head_sha)])
            else:
                endpoint = metadata.get("base_sha", base)
                if zero_oid(endpoint):
                    default = metadata.get("default_branch")
                    if not default:
                        reasons.append("new branch has no available default-branch base")
                        continue
                    endpoint = os.fsdecode(git(repo, "merge-base", default, head_sha).stdout).strip()
                if endpoint:
                    ranges.append((commit_sha(repo, endpoint), head_sha))
                elif kind != "release":
                    reasons.append("no trustworthy base; broad validation")
        except (ContractError, KeyError) as exc:
            reasons.append(f"range unavailable; broad validation: {exc}")
    changes, old_policies = [], []
    for before, after in sorted(set(ranges)):
        records = diff_records(repo, before, after)
        old_modes = {}
        for entry in git(repo, "ls-tree", "-rz", before).stdout.split(b"\0"):
            if entry:
                meta, path = entry.split(b"\t", 1)
                old_modes[os.fsdecode(path)] = meta.split()[0]
        for record in records:
            record["old_executable"] = old_modes.get(record.get("old_path")) == b"100755"
            record["old_gitlink"] = old_modes.get(record.get("old_path")) == b"160000"
        changes.extend(records)
        if any(inventory_path in (c.get("old_path"), c.get("new_path")) for c in records):
            reasons.append("inventory changed: broad before trusting candidate ownership")
        raw = git(repo, "show", f"{before}:{inventory_path}", check=False)
        if raw.returncode:
            if any(c.get("old_path") for c in changes):
                reasons.append("base inventory unavailable for old-path classification")
        else:
            try:
                old_policies.append(validate_inventory(json.loads(raw.stdout, object_pairs_hook=_unique_object)))
            except (ContractError, ValueError) as exc:
                reasons.append(f"base inventory uninterpretable: {exc}")
    # Git records executable docs and gitlinks without following symlinks.
    modes = {}
    if not deleted:
        for entry in git(repo, "ls-tree", "-rz", head_sha).stdout.split(b"\0"):
            if entry:
                meta, path = entry.split(b"\t", 1)
                modes[os.fsdecode(path)] = meta.split()[0]
    for record in changes:
        record["executable"] = modes.get(record.get("new_path")) == b"100755"
        record["gitlink"] = modes.get(record.get("new_path")) == b"160000"
    result = make_plan(policy, [] if deleted else changes, head=head_sha, base=base, context=context, mode=mode,
                       event=identities[0] if len(identities) == 1 else identities, profiles=profiles,
                       broadening=[] if deleted else reasons, base_policies=old_policies)
    if deleted:
        result.update({"deleted_ref": True, "lanes": [], "expected_lane_ids": [], "selected_checks": []})
        result.pop("plan_id")
        result["plan_id"] = digest(result)
    return result


def validate_plan(plan, policy):
    require(isinstance(plan, dict) and type(plan.get("schema_version")) is int and plan["schema_version"] == 1, "unsupported plan schema")
    expected = dict(plan)
    expected.pop("plan_id", None)
    require(plan.get("plan_id") == digest(expected), "plan identity hash mismatch")
    require(plan.get("inventory_hash") == digest(policy), "plan inventory mismatch")
    require(plan.get("source_identity", {}).get("sha") == plan.get("tested_sha") == plan.get("head"), "source/tested SHA mismatch")
    identity = plan["source_identity"]
    require(identity.get("kind") in {"committed", "dirty"}, "invalid source identity kind")
    require(identity["kind"] != "dirty" or bool(identity.get("snapshot_digest")), "dirty identity requires content digest")
    lanes = plan.get("lanes")
    require(isinstance(lanes, list), "missing lanes")
    seen = set()
    for lane in lanes:
        require(lane["id"] == lane["profile"] and lane["id"] not in seen, "duplicate/invalid lane")
        seen.add(lane["id"])
        require(lane["profile"] in policy["profiles"] and lane["profile_hash"] == digest(policy["profiles"][lane["profile"]]), "profile policy mismatch")
        _ids(lane["checks"], policy["checks"], "lane checks")
        require(all(lane["profile"] in policy["checks"][c]["profiles"] for c in lane["checks"]), "ineligible check/profile")
    require(sorted(seen) == plan.get("expected_lane_ids"), "expected lane identity mismatch")
    require(sorted({c for lane in lanes for c in lane["checks"]}) == plan.get("selected_checks"), "selected check accounting mismatch")
    return plan


def aggregate(policy, plan, receipts, *, complete=True, selection_conclusion=None, job_conclusions=None, repo=None):
    validate_plan(plan, policy)
    errors, visible = [], []
    if selection_conclusion != "success" and not (selection_conclusion is None and plan.get("context") == "local"):
        errors.append("selection job was not explicitly successful")
    if complete and (plan.get("partial_profiles") or plan["source_identity"].get("kind") != "committed"):
        errors.append("partial/dirty run cannot satisfy complete gate")
    expected = {lane["id"]: lane for lane in plan["lanes"]}
    seen = set()
    if job_conclusions is not None:
        require(isinstance(job_conclusions, dict), "malformed worker conclusions")
        if set(job_conclusions) != set(expected) or any(v != "success" for v in job_conclusions.values()):
            errors.append("selected worker job conclusions missing/skipped/failed/cancelled")
    elif complete and plan.get("context") == "ci":
        errors.append("complete CI aggregation requires independent worker job conclusions")
    quarantine = {}
    if repo is not None:
        raw = git(repo, "show", f"{plan['tested_sha']}:scripts/grotto-ci-quarantine.tsv", check=False)
        if raw.returncode == 0:
            for line in raw.stdout.decode().splitlines():
                if line and not line.startswith("#"):
                    parts = line.split("\t")
                    require(len(parts) == 3, "malformed named quarantine TSV")
                    quarantine[parts[0], parts[1]] = parts[2]
    for receipt in receipts:
        require(isinstance(receipt, dict) and type(receipt.get("schema_version")) is int and receipt["schema_version"] == 1, "malformed receipt")
        lane_id = receipt.get("lane_id")
        if lane_id in seen or lane_id not in expected:
            errors.append(f"duplicate/unexpected lane: {lane_id}")
            continue
        seen.add(lane_id)
        lane = expected[lane_id]
        for key in ("plan_id", "inventory_hash", "source_identity"):
            if receipt.get(key) != plan[key]:
                errors.append(f"{lane_id}: stale {key}")
        if receipt.get("profile") != lane["profile"] or receipt.get("profile_hash") != lane["profile_hash"] or not receipt.get("profile_fingerprint"):
            errors.append(f"{lane_id}: profile mismatch/missing fingerprint")
        for key, value in (("job_conclusion", "success"), ("registration_audit", "passed"), ("input_audit", "passed")):
            if receipt.get(key) != value:
                errors.append(f"{lane_id}: {key} did not pass")
        results = receipt.get("checks")
        require(isinstance(results, dict), f"{lane_id}: malformed checks")
        if set(results) != set(lane["checks"]):
            errors.append(f"{lane_id}: missing/unexpected check results")
        for check_id in sorted(set(results) & set(lane["checks"])):
            check, result = policy["checks"][check_id], results[check_id]
            require(isinstance(result, dict) and result.get("outcome") in OUTCOMES, f"{check_id}: invalid outcome")
            outcome = result["outcome"]
            names = check.get("ctest_tests", {}).get(lane["profile"], [check_id])
            expected_names, observed = result.get("expected_executions"), result.get("observed_executions")
            if not isinstance(observed, list) or expected_names != names or sorted(observed) != sorted(names):
                errors.append(f"{lane_id}/{check_id}: incomplete execution accounting")
            acceptable = outcome == "passed" and check.get("expected_outcome", "pass") == "pass"
            if result.get("sanitizer_reports") and check["requirement"] == "blocking":
                errors.append(f"{lane_id}/{check_id}: blocking sanitizer report")
            if outcome == "expected_incompatibility":
                spec = check.get("expected_outcome")
                acceptable = isinstance(spec, dict) and result.get("exception_case") == spec["case"] and result.get("exit_code") == spec["exit_code"]
            if outcome == "advisory_failure":
                acceptable = check["requirement"] == "advisory"
            if outcome == "quarantined_failure":
                # Read the tested tree's existing TSV; never duplicate its policy.
                profile = policy["profiles"][lane["profile"]]
                platform = profile["platform"]
                if profile["instrumentation"] != "none":
                    platform += "-" + profile["instrumentation"].split("_", 1)[0]
                failures = result.get("failed_executions", [])
                acceptable = (isinstance(failures, list) and bool(failures)
                              and set(failures) <= set(names)
                              and all((platform, name) in quarantine for name in failures)
                              and result.get("sanitizer_reports") == []
                              and result.get("quarantine_beads") == sorted({quarantine[platform, name] for name in failures if (platform, name) in quarantine}))
            if not acceptable:
                errors.append(f"{lane_id}/{check_id}: {outcome}")
            if outcome != "passed" or result.get("initial_failure"):
                visible.append({"lane": lane_id, "check": check_id, "outcome": outcome, "initial_failure": bool(result.get("initial_failure"))})
    errors += [f"missing lane: {lane}" for lane in sorted(set(expected) - seen)]
    return {"schema_version": 1, "plan_id": plan["plan_id"], "passed": not errors, "errors": errors, "non_clean_results": visible,
            "tested_sha": plan["tested_sha"], "source_identity": plan["source_identity"], "inventory_hash": plan["inventory_hash"],
            "mode": plan["effective_mode"], "partial_profiles": plan["partial_profiles"], "complete": complete,
            "tag": plan["event"].get("tag") if isinstance(plan["event"], dict) else None,
            "enforcement_ready": plan.get("enforcement_ready", False), "coverage_gaps": plan.get("coverage_gaps", [])}


def _semver(version):
    require(isinstance(version, str) and SEMVER.fullmatch(version) is not None, f"invalid SemVer: {version}")
    prerelease = SEMVER.fullmatch(version).group(4)
    if prerelease:
        require(all(not (part.isdigit() and len(part) > 1 and part.startswith("0")) for part in prerelease.split(".")), "leading zero prerelease identifier")
    return version.split("-", 1)[0].split("+", 1)[0]


def extract_version(source, content):
    matches = re.findall(source["pattern"], content, re.MULTILINE)
    require(len(matches) == 1, f"ambiguous/missing version in {source['path']}")
    match = matches[0]
    if source.get("join") == "." and isinstance(match, tuple):
        return ".".join(match)
    require(isinstance(match, str), f"invalid version capture in {source['path']}")
    return match


def release_plan(repo, policy, *, head, tag=None, component=None, dry_run=False, run_id=None):
    validate_inventory(policy)
    tested = commit_sha(repo, head)
    require(tag or dry_run, "release requires tag or explicit dry-run")
    version = None
    if tag:
        require(not tag.startswith("-") and "/" not in tag, "invalid release tag")
        peeled = commit_sha(repo, "refs/tags/" + tag)
        if peeled != tested:
            raise CheckFailure("tag does not identify tested SHA")
        if tag.startswith("v"):
            identity, version = "legacy_global", tag[1:]
        else:
            require("-v" in tag, "unknown release tag syntax")
            identity, version = tag.rsplit("-v", 1)
            require(identity in policy["release_components"], f"unknown component prefix: {identity}")
        base_version = _semver(version)
        require(component in {None, identity, "global" if identity == "legacy_global" else identity}, "component/tag mismatch")
        component = identity
    else:
        component = "legacy_global" if component == "global" else component
        require(component == "legacy_global" or component in policy["release_components"], "unsupported dry-run component")
        require(isinstance(run_id, str) and bool(run_id), "dry-run requires run identity")
    # Version files and manifest are read from the exact committed tree, never dirty cwd.
    manifest = git(repo, "show", f"{tested}:VERSION_MANIFEST.md").stdout.decode()
    components = policy["release_components"] if component == "legacy_global" else {component: policy["release_components"][component]}
    verified = {}
    for ident, spec in components.items():
        label = spec.get("manifest_name", ident)
        rows = [line.split("|") for line in manifest.splitlines() if line.startswith("|") and line.split("|")[1].strip() == label]
        require(len(rows) == 1, f"missing/duplicate manifest component: {ident}")
        declared = rows[0][3].strip().split()[0]
        _semver(declared)
        versions = []
        for source in spec.get("version_sources", []):
            path = normalized_path(source["path"])
            content = git(repo, "show", f"{tested}:{path}").stdout.decode()
            value = extract_version(source, content)
            if "expected_literal" in source:
                require(value == source["expected_literal"], f"derived version template changed: {path}")
                value = declared
            versions.append(value)
        require(versions, f"no authoritative sources: {ident}")
        if any(_semver(v) != declared for v in versions):
            raise CheckFailure(f"{ident}: source/manifest version disagreement")
        if tag and component != "legacy_global" and declared != base_version:
            raise CheckFailure(f"{ident}: tag/declared base-version mismatch")
        if tag and component != "legacy_global" and spec.get("full_version_source"):
            src = spec["full_version_source"]
            text = git(repo, "show", f"{tested}:{src['path']}").stdout.decode()
            suffix = re.findall(src["pattern"], text, re.MULTILINE)
            require(len(suffix) == 1, "missing prerelease source")
            full = declared + ("-" + suffix[0] if suffix[0] else "")
            if version.split("+", 1)[0] != full:
                raise CheckFailure(f"{ident}: prerelease source/tag mismatch")
        verified[ident] = declared
    route = policy.get("legacy_global", {}) if component == "legacy_global" else components[component]
    publishers = route.get("publishers", [route.get("publisher", "none")])
    jobs = sorted(route.get("package_jobs", []))
    return {"schema_version": 1, "identity": component, "tag": tag, "version": version, "tested_sha": tested,
            "source_identity": {"kind": "committed", "sha": tested}, "inventory_hash": digest(policy),
            "mode": "broad", "publishers": publishers, "required_package_jobs": jobs,
            "validation_only": publishers == ["none"], "dry_run": dry_run, "publish": bool(tag and not dry_run and publishers != ["none"]),
            "run_id": run_id, "verified_versions": verified}


def aggregate_release(release, validation, jobs):
    require(isinstance(jobs, dict), "malformed package conclusions")
    errors = []
    if (not validation.get("passed") or not validation.get("complete")
            or validation.get("source_identity") != release["source_identity"]
            or validation.get("tested_sha") != release["tested_sha"]
            or validation.get("inventory_hash") != release["inventory_hash"]
            or validation.get("mode") != "broad" or validation.get("partial_profiles")
            or validation.get("tag") != release.get("tag")):
        errors.append("missing/mismatched broad exact-tag validation")
    required = set(release["required_package_jobs"])
    for job in sorted(required):
        if jobs.get(job) != "success":
            errors.append(f"applicable package did not succeed: {job}")
    for job in sorted(set(jobs) - required):
        if jobs[job] != "not_applicable":
            errors.append(f"unexpected package applicability: {job}")
    return {"schema_version": 1, "passed": not errors, "publish": release["publish"] and not errors, "errors": errors}
