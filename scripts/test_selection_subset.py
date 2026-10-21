"""Exact ordinary libgo diagnostic subset authority; never a full partition.

Historical review provenance and current realization bindings are independent.
"""
from __future__ import annotations

import hashlib
from pathlib import Path
import re

AUTHORITY = "reviewed_diagnostic_nonmember_subset_v1"
IDENT = "ordinary_libgo_nonmember_subset"
PROFILES = {
    "groundhog.linux": (".github/workflows/groundhog-ci.yml", "^(groundhog-|nostrc-test-bus-selftest$)", "^groundhog-mdk011-"),
    "grotto.linux": (".github/workflows/grotto-ci.yml", "^(signer/|nip55l_|nostr_signer_webext_host_)", ""),
}
REGISTRATIONS = tuple(sorted((
    "GoChannelCloseTest", "GoChannelMpmcSlotTest", "GoChannelStressTest", "GoChannelTest",
    "GoContextCancelTest", "GoContextStressTest", "GoContextTimeoutTest", "GoErrorTest",
    "GoFiberBasicTest", "GoFiberBasicTestW4", "GoFiberChanExtTest", "GoFiberChanExtTestW4",
    "GoFiberChanTest", "GoFiberChanTestW4", "GoFiberContextTest", "GoFiberIoDeadlockTest",
    "GoFiberIoDeadlockTestW4", "GoFiberIoTest", "GoFiberIoTimeoutTest", "GoFiberIoTimeoutTestW4",
    "GoFiberStarvationTest", "GoFiberStarvationTestW4", "GoHashMapTest2", "GoIntArrayTest",
    "GoLongAdderStressTest", "GoRefPtrTest", "GoSelectCancelTest", "GoSelectClosedTest",
    "GoSelectMultiTest", "GoSelectTest", "GoStringArrayTest", "GoTickerTest", "GoWaitGroupTest",
)))
CONTRACT_KEYS = {"schema_version", "profile_hash", "workflow", "registrations", "registration_universe",
                 "universe_hash", "primary_registrations", "required_closure", "candidate_contract_hash", "toolchain_contract_hash", "review"}
REVIEW_KEYS = {"reviewer", "approval_document", "tracking", "source_sha", "pre_audit_sha256",
               "post_audit_sha256", "realization_sha256", "profile_fingerprint", "source_inputs",
               "platform", "architecture", "compiler", "feature_variants", "instrumentation"}


def require(condition, message):
    if not condition:
        raise ValueError("ordinary nonmember subset: " + message)


def _sha(value, length=64):
    return isinstance(value, str) and re.fullmatch(r"[0-9a-f]{%d}" % length, value) is not None


def contracts(policy):
    from test_selection import digest, _nonmember_names, normalized_path
    records = policy.get("diagnostic_nonmember_subsets", {})
    require(isinstance(records, dict) and not set(records) - set(PROFILES), "unsupported profile")
    for profile_id, record in records.items():
        profile = policy.get("profiles", {}).get(profile_id, {})
        require(isinstance(record, dict) and set(record) == CONTRACT_KEYS, "malformed contract fields")
        require(type(record["schema_version"]) is int and record["schema_version"] == 1, "unknown schema version")
        require(record["profile_hash"] == digest(profile), "stale profile hash")
        require(all(profile.get(k) == v for k, v in
                    {"platform": "linux", "architecture": "x86_64", "compiler": "gcc", "instrumentation": "none"}.items()),
                "only authored ordinary Linux/x86_64 GCC profiles are admitted")
        require(record["registrations"] == list(REGISTRATIONS), "only the exact reviewed 33-name family is admitted")
        for key in ("registration_universe", "primary_registrations", "required_closure"):
            require(_nonmember_names(record[key]), "invalid " + key)
        universe, primary, required = (set(record[k]) for k in ("registration_universe", "primary_registrations", "required_closure"))
        require(set(REGISTRATIONS) < universe and primary <= required <= universe
                and not set(REGISTRATIONS) & required, "universe/primary/fixture collision")
        require(record["universe_hash"] == digest(record["registration_universe"])
                and _sha(record["candidate_contract_hash"]) and _sha(record["toolchain_contract_hash"]), "invalid graph/toolchain hash")
        path, include, exclude = PROFILES[profile_id]
        workflow = record["workflow"]
        require(isinstance(workflow, dict) and set(workflow) == {"path", "sha256", "include", "exclude"}
                and workflow == dict(path=path, sha256=workflow.get("sha256"), include=include, exclude=exclude)
                and _sha(workflow["sha256"]), "invalid source workflow/filter")
        review = record["review"]
        require(isinstance(review, dict) and set(review) == REVIEW_KEYS, "malformed review provenance")
        require(isinstance(review["reviewer"], str) and bool(review["reviewer"].strip())
                and review["approval_document"] == "docs/plans/ordinary-profile-nonmember-policy-proposal-2026-10-19.md"
                and review["tracking"] == ["nostrc-0cfsg.2", "nostrc-0cfsg.9"]
                and _sha(review["source_sha"], 40), "missing review identity")
        for key in ("pre_audit_sha256", "post_audit_sha256", "realization_sha256", "profile_fingerprint"):
            require(_sha(review[key]), "missing evidence hash: " + key)
        require(all(review[k] == profile.get(k) for k in
                    ("platform", "architecture", "compiler", "feature_variants", "instrumentation")), "review profile evidence differs")
        inputs = review["source_inputs"]
        require(isinstance(inputs, dict) and {path, "CMakeLists.txt", "libgo/CMakeLists.txt",
                "cmake/NostrcTestInventory.cmake"} <= set(inputs), "source proof incomplete")
        for name, value in inputs.items():
            require(isinstance(name, str) and normalized_path(name) == name and _sha(value), "invalid source proof")
        require(inputs[path] == workflow["sha256"], "workflow/source proof disagreement")
    return records


def marked(value):
    """Recognize stripped wrappers and unknown authority versions, not just flags."""
    return isinstance(value, dict) and (
        str(value.get("authority", "")).startswith("reviewed_diagnostic_nonmember_subset")
        or value.get("id") == IDENT or "candidate_contract_hash" in value
        or value.get("registrations") == list(REGISTRATIONS)
        or "diagnostic_nonmember_subset_profiles" in value
        or bool(value.get("diagnostic_nonmember_subsets")))


def nested(value):
    if isinstance(value, dict):
        return marked(value) or any(nested(v) for v in value.values())
    return isinstance(value, list) and any(nested(v) for v in value)


def references_profiles(value, profiles):
    """Recognize embedded ordinary receipts even after omission records are stripped."""
    if isinstance(value, dict):
        if any(isinstance(value.get(k), str) and value[k] in profiles for k in ("profile", "profile_id")):
            return True
        return any(references_profiles(child, profiles) for child in value.values())
    return isinstance(value, list) and any(references_profiles(child, profiles) for child in value)


def validate_record(record, contract, identities, profile_id):
    from test_selection import digest, NONMEMBER_BINDING_KEYS, _nonmember_names
    keys = CONTRACT_KEYS | {"id", "profile", "status", "authority", "target_ids", "audit_build_targets", "binding"}
    require(profile_id in PROFILES and isinstance(record, dict) and set(record) == keys, "missing/malformed accounting")
    require(all(digest(record[k]) == digest(v) for k, v in contract.items())
            and record["id"] == IDENT and record["profile"] == profile_id
            and record["status"] == "not_executed" and record["authority"] == AUTHORITY, "accounting differs from approval")
    binding = record["binding"]
    require(isinstance(binding, dict) and set(binding) == NONMEMBER_BINDING_KEYS, "invalid binding fields")
    require(binding["profile_id"] == profile_id and binding["profile_hash"] == contract["profile_hash"], "binding profile differs")
    for key in NONMEMBER_BINDING_KEYS - {"source_identity", "profile_id", "configuration"}:
        require(_sha(binding[key]), "invalid binding hash: " + key)
    require(isinstance(binding["configuration"], str), "invalid configuration")
    require(all(digest(binding.get(k)) == digest(v) for k, v in identities.items()), "stale fresh binding")
    for key in ("target_ids", "audit_build_targets"):
        require(_nonmember_names(record[key]), "missing/invalid transitive targets")


def validate_nested(value, expected):
    from test_selection import digest
    if isinstance(value, dict):
        if marked(value):
            require(expected is not None and digest(value) == digest(expected), "unknown/malformed nested accounting")
        for child in value.values():
            validate_nested(child, expected)
    elif isinstance(value, list):
        for child in value:
            validate_nested(child, expected)


def candidate_contract(realization, tests, descriptors):
    """Retain commands/properties/inputs/flags with explicit, distinct custody roots.

    Export-local backtraces are excluded; unique target names replace opaque IDs.
    External absolute paths are deliberately not erased.
    """
    import test_selection_realization as backend
    roots = sorted(((str(Path(realization[k]).resolve()), token) for k, token in
                    (("repo_root", "{source}"), ("build_dir", "{build}"))), key=lambda item: -len(item[0]))
    def normalize(value):
        if isinstance(value, str):
            for root, token in roots:
                value = value.replace(root + "/", token + "/")
                if value == root:
                    value = token
            return value
        if isinstance(value, dict):
            return {k: normalize(v) for k, v in value.items()}
        if isinstance(value, list):
            return [normalize(v) for v in value]
        return value
    names = [t["name"] for t in realization["targets"].values()]
    require(len(names) == len(set(names)), "ambiguous target names")
    result = {}
    for name in REGISTRATIONS:
        require(name in tests and descriptors.get(name, {}).get("owner") == ["libgo"], "missing/changed libgo owner: " + name)
        desc = descriptors[name]
        require(Path(desc.get("source_dir", "")).resolve() == Path(realization["repo_root"]).resolve() / "libgo",
                "candidate left the reviewed libgo source directory: " + name)
        props = backend._properties(tests[name])
        require(not any(props.get(k) for k in ("FIXTURES_SETUP", "FIXTURES_CLEANUP", "FIXTURES_REQUIRED", "DEPENDS"))
                and not desc.get("declared_depends") and desc.get("expected") in ([], ["pass"]), "candidate fixture/dependency changed: " + name)
        ids = backend._target_closure(backend._mapped_targets(tests[name], desc, realization), realization)
        require(bool(ids) and any(realization["targets"][i]["type"] == "EXECUTABLE" for i in ids), "missing real executable target: " + name)
        targets, inputs = [], {}
        for ident in ids:
            target = dict(realization["targets"][ident])
            target.pop("id")
            target["dependencies"] = sorted(realization["targets"][i]["name"] for i in target["dependencies"])
            targets.append(target)
            for source in target["sources"]:
                if not source["generated"]:
                    path = Path(source["path"])
                    require(path.is_file() and path.resolve().is_relative_to(Path(realization["repo_root"]).resolve()), "unresolved candidate source: " + str(path))
                    inputs[normalize(str(path))] = hashlib.sha256(path.read_bytes()).hexdigest()
        runtime = sorted(set(desc.get("inputs", []) + realization.get("runtime_file_manifest", {}).get(name, [])))
        for value in runtime:
            path = Path(value)
            require(path.is_file(), "unresolved candidate runtime input")
            inputs[normalize(str(path))] = hashlib.sha256(path.read_bytes()).hexdigest()
        result[name] = normalize({"command": tests[name].get("command"), "properties": props,
                                  "descriptor": desc, "runtime_inputs": runtime,
                                  "targets": sorted(targets, key=lambda t: t["name"]), "input_hashes": inputs})
    return result


def toolchain_contract(realization):
    # System/compiler paths remain explicit; none of these are checkout-local
    # IDs. A different compiler/tool version or generator needs fresh review.
    return {k: realization[k] for k in ("toolchains", "tool_versions", "cmake")}


def validate_graph(contract, realization, tests, descriptors, authored):
    from test_selection import digest
    import test_selection_realization as backend
    require(digest(toolchain_contract(realization)) == contract["toolchain_contract_hash"], "reviewed compiler/toolchain/generator changed")
    require(sorted(tests) == contract["registration_universe"], "fresh registration universe changed")
    require(sorted(authored) == contract["primary_registrations"]
            and backend._fixture_closure(authored, tests, descriptors) == contract["required_closure"], "authored primary/fixture closure changed")
    _, include, exclude = PROFILES[realization["profile_id"]]
    require(all(not re.search(include, n) or (exclude and re.search(exclude, n)) for n in REGISTRATIONS), "candidate matches workflow execution")
    root = Path(realization["repo_root"])
    for name, expected in contract["review"]["source_inputs"].items():
        source = root / name
        require(source.is_file() and source.resolve().is_relative_to(root.resolve())
                and hashlib.sha256(source.read_bytes()).hexdigest() == expected, "reviewed source input changed: " + name)
    require(digest(candidate_contract(realization, tests, descriptors)) == contract["candidate_contract_hash"], "candidate command/property/target/input contract changed")


def admit(inventory, realization, tests, descriptors):
    from test_selection import digest
    import test_selection_realization as backend
    contract = contracts(inventory).get(realization["profile_id"])
    if contract is None:
        return []
    require(descriptors == {d["name"]: d for d in realization["descriptors"]}, "generated descriptor authority differs")
    require(inventory["profiles"][realization["profile_id"]] == realization["profile"]
            and digest(inventory) == realization["inventory_hash"], "inventory/profile identity changed")
    require(backend.load_realization(realization["build_dir"], realization["source_identity"]) == realization, "realization changed")
    import json
    config = ["-C", realization["configuration"]] if realization["configuration"] else []
    fresh = json.loads(backend._command(["ctest", "--test-dir", realization["build_dir"], *config, "--show-only=json-v1"],
        env=backend.phase_environment(realization["profile"], realization["repo_root"], "test")))["tests"]
    require(len({t["name"] for t in fresh}) == len(fresh)
            and backend._registration_hash(fresh) == backend._registration_hash(realization["tests"]), "fresh CTest export changed/duplicated")
    authored = [n for check in inventory["checks"].values() if check.get("kind") == "ctest"
                and backend._eligible(check, realization["profile_id"]) for n in backend._membership(check, realization["profile_id"])]
    require(len(set(authored)) == len(authored), "multiply claimed primary")
    validate_graph(contract, realization, tests, descriptors, authored)
    ids = backend._target_closure({i for n in REGISTRATIONS for i in backend._mapped_targets(tests[n], descriptors[n], realization)}, realization)
    record = dict(contract, id=IDENT, profile=realization["profile_id"], status="not_executed", authority=AUTHORITY,
                  target_ids=ids, audit_build_targets=sorted({realization["targets"][i]["name"] for i in ids}),
                  binding=backend._nonmember_binding(realization, descriptors))
    validate_record(record, contract, record["binding"], realization["profile_id"])
    return [record]


def validate_resolved(resolved, realization):
    """Refuse malformed closure before launching even the prerequisite build."""
    import test_selection_realization as backend
    for record in resolved.get("baseline_exclusions", []):
        if not marked(record):
            continue
        contract = {k: record[k] for k in CONTRACT_KEYS}
        profile_id = realization["profile_id"]
        contracts({"profiles": {profile_id: realization["profile"]},
                   "diagnostic_nonmember_subsets": {profile_id: contract}})
        descriptors = {d["name"]: d for d in realization["descriptors"]}
        tests = {t["name"]: t for t in realization["tests"]}
        validate_record(record, contract, backend._nonmember_binding(realization, descriptors), profile_id)
        validate_graph(contract, realization, tests, descriptors, contract["primary_registrations"])
        ids = backend._target_closure({i for name in REGISTRATIONS for i in backend._mapped_targets(tests[name], descriptors[name], realization)}, realization)
        targets = sorted({realization["targets"][i]["name"] for i in ids})
        require(ids == record["target_ids"] and targets == record["audit_build_targets"]
                and set(ids) <= set(resolved["target_ids"]) and set(targets) <= set(resolved["build_targets"])
                and not set(REGISTRATIONS) & set(resolved["tests"]), "execution collision or missing audit build closure")
        require(resolved.get("complete") is False and resolved.get("complete_gate_eligible") is False,
                "resolved diagnostic incomplete flags missing or contradictory")


def validate_receipt(receipt, backend, exclusions, contract, plan, lane, profile):
    from test_selection import digest, validate_phase_parallelism_evidence
    require(len(exclusions) == 1 and receipt.get("baseline_exclusions") == exclusions, "exact accounting required")
    evidence = backend if backend else receipt
    expected = {"source_identity": plan["source_identity"], "inventory_hash": plan["inventory_hash"],
                "profile_id": lane["profile"], "profile_hash": lane["profile_hash"],
                "profile_fingerprint": receipt.get("profile_fingerprint"), "configuration": profile["build_type"],
                "phase_context_hash": evidence.get("phase_context_hash")}
    validate_record(exclusions[0], contract, expected, lane["profile"])
    validate_nested(receipt, exclusions[0])
    binding = exclusions[0]["binding"]
    for value in (receipt, evidence):
        require(value.get("complete") is False and value.get("complete_gate_eligible") is False
                and value.get("diagnostic_eligible") is True and value.get("baseline_exclusions") == exclusions,
                "incomplete/diagnostic flags missing or contradictory")
    phase = evidence.get("phase_context", {})
    require(evidence.get("configuration") == profile["build_type"] == phase.get("configuration")
            and receipt.get("phase_context") == phase and digest(phase) == expected["phase_context_hash"]
            and evidence.get("plan_hash") == digest(plan) and evidence.get("nonmember_binding") == binding,
            "retained execution context differs")
    audit = evidence.get("input_audit_details", evidence.get("input_audit"))
    require(isinstance(audit, dict) and audit.get("passed") is True and audit.get("after_build") is True
            and audit.get("gaps") == [] and audit.get("selective_enforcement") is False
            and audit.get("baseline_exclusions") == exclusions and audit.get("nonmember_binding") == binding
            and audit.get("complete") is False and audit.get("complete_gate_eligible") is False and audit.get("phase_context") == phase
            and audit.get("audit_hash") == digest({k: v for k, v in audit.items() if k != "audit_hash"}), "full post-build audit missing/stale")
    for key in ("source_identity", "inventory_hash", "profile_fingerprint", "phase_context_hash"):
        require(audit.get(key) == expected[key], "audit identity differs: " + key)
    require(set(exclusions[0]["target_ids"]) <= set(evidence.get("target_ids", []))
            and set(exclusions[0]["audit_build_targets"]) <= set(evidence.get("build_targets", [])), "audit target custody missing")
    executed = evidence.get("expected_executions", [])
    require(isinstance(executed, list) and set(executed) <= set(contract["required_closure"])
            and not set(REGISTRATIONS) & set(evidence.get("observed_executions", {})), "omission executed/relabelled")
    import test_selection_realization as realization
    jobs = realization.authored_phase_parallelism(profile) or dict.fromkeys(("build", "test"), profile.get("parallelism", 1))
    if realization.authored_phase_parallelism(profile) is not None:
        validate_phase_parallelism_evidence(profile, evidence, require_test=True)
    options = dict(build_parallel=jobs["build"], test_parallel=jobs["test"], timeout_seconds=profile.get("timeout_seconds"))
    require(evidence.get("execution_options") == options, "execution options differ from authored phases")
    commands = evidence.get("commands", [])
    require(any(c and c[0] == "ctest" for c in commands), "physical CTest argv missing")
    require(any(c[:2] == ["cmake", "--build"] for c in commands), "physical prerequisite build argv missing")
    for command in commands:
        if command and command[0] in ("cmake", "ctest"):
            phase = "test" if command[0] == "ctest" else "build"
            require(command.count("--parallel") == 1 and command[command.index("--parallel") + 1:command.index("--parallel") + 2]
                    == [str(jobs[phase])] and not any(v.startswith("-j") or v.startswith("--parallel=") for v in command), "physical phase parallelism differs")
            if phase == "build" and "--target" in command:
                require(set(exclusions[0]["audit_build_targets"]) <= set(command[command.index("--target") + 1:]), "physical build omits audit targets")
        if command and command[0] == "ctest" and options["timeout_seconds"] is not None:
            require(command.count("--timeout") == 1 and command[command.index("--timeout") + 1:command.index("--timeout") + 2]
                    == [str(options["timeout_seconds"])], "physical timeout differs")
