"""Build-local realization and exact execution for the selection contract.

Public functions take/return JSON-compatible objects; they do not change policy,
materialize trees, provision platforms, or silently recover dependency drift.
RealizationError means a failed validation (exit 1); malformed calls use ValueError
(exit 2). Every unresolved registration is retained as an explicit blocking gap.
"""
from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
import platform
import re
import shutil
import signal
import subprocess
import tempfile
import time
import uuid
import xml.etree.ElementTree as ET


class RealizationError(RuntimeError):
    pass


def _hash(value):
    return hashlib.sha256(json.dumps(value, sort_keys=True, separators=(",", ":"),
                                     ensure_ascii=True, allow_nan=False).encode()).hexdigest()


def _read(path):
    try:
        return json.loads(Path(path).read_text())
    except (OSError, ValueError) as exc:
        raise RealizationError(f"invalid/missing JSON {path}: {exc}") from exc


def _atomic(path, value):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, name = tempfile.mkstemp(dir=path.parent, prefix=path.name + ".")
    try:
        with os.fdopen(fd, "w") as stream:
            json.dump(value, stream, sort_keys=True, indent=2, allow_nan=False)
            stream.write("\n")
        os.replace(name, path)
    finally:
        if os.path.exists(name):
            os.unlink(name)


def _execute(argv, cwd=None, env=None, log_path=None):
    try:
        process = subprocess.Popen([str(x) for x in argv], cwd=cwd, env=env,
            text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            start_new_session=os.name != "nt")
    except OSError as exc:
        raise RealizationError(f"cannot execute {argv[0]}: {exc}") from exc
    try:
        output, _ = process.communicate()
    except BaseException:
        # Only this command's process group, never sibling jobs or global prune.
        if os.name != "nt":
            try:
                os.killpg(process.pid, signal.SIGTERM)
            except ProcessLookupError:
                pass
        else:
            process.terminate()
        try:
            output, _ = process.communicate(timeout=5)
        except subprocess.TimeoutExpired:
            if os.name != "nt":
                os.killpg(process.pid, signal.SIGKILL)
            else:
                process.kill()
            output, _ = process.communicate()
        if log_path:
            Path(log_path).write_text(output)
        raise
    if log_path:
        Path(log_path).write_text(output)
    return subprocess.CompletedProcess(argv, process.returncode, output)


def _command(argv, cwd=None, env=None):
    result = _execute(argv, cwd=cwd, env=env)
    if result.returncode:
        raise RealizationError(f"command failed ({result.returncode}): {argv!r}\n{result.stdout}")
    return result.stdout


def _registration_hash(tests):
    # CTest omits command entirely until a freshly configured executable exists.
    # Commands already enumerable are bound separately; generated CTestTestfiles
    # are content-bound in load_realization, including commands not yet runnable.
    return _hash([{k: v for k, v in test.items() if k != "command"} for test in tests])


def _properties(test):
    return {p["name"]: p["value"] for p in test.get("properties", [])}


def _list(value):
    if value is None or value == "":
        return []
    return list(value) if isinstance(value, list) else str(value).split(";")


def _path(root, value):
    p = Path(value)
    return str(p if p.is_absolute() else Path(root) / p)


def _relative(path, root):
    try:
        return Path(os.path.abspath(path)).relative_to(Path(os.path.abspath(root))).as_posix()
    except ValueError:
        return None


def _identity(source_identity):
    if not isinstance(source_identity, dict) or not source_identity:
        raise ValueError("explicit execution source identity is required (including dirty digest)")
    # The materialization owner defines the identity shape. Preserve it byte for
    # byte and fingerprint the complete object, never just the committed SHA.
    return json.loads(json.dumps(source_identity, sort_keys=True))


def _cmake_literal_arguments(text, position):
    """Read only literal arguments emitted in generated CTest add_test calls.

    This is not a source-CMake evaluator. Variable expansion, lists in unquoted
    arguments and unsupported escapes fail closed; no shell is involved.
    """
    arguments = []
    while position < len(text):
        while position < len(text) and text[position].isspace():
            position += 1
        if position >= len(text):
            break
        if text[position] == ")":
            return arguments
        bracket = re.match(r"\[(=*)\[", text[position:])
        if bracket:
            position += bracket.end()
            end = text.find("]" + bracket[1] + "]", position)
            if end < 0:
                raise RealizationError("unterminated generated CTest bracket argument")
            arguments.append(text[position:end])
            position = end + len(bracket[1]) + 2
        elif text[position] == '"':
            position += 1
            value = ""
            while position < len(text) and text[position] != '"':
                char = text[position]
                position += 1
                if char == "$":
                    raise RealizationError("nonliteral generated CTest command requires an explicit descriptor")
                if char == "\\":
                    if position >= len(text):
                        break
                    char = text[position]
                    position += 1
                    if char in "nrt":
                        char = {"n": "\n", "r": "\r", "t": "\t"}[char]
                    elif char == "\n":
                        continue
                    elif char not in '\\";$[]()# ':
                        raise RealizationError("unsupported escape in generated CTest command")
                value += char
            if position >= len(text):
                raise RealizationError("unterminated generated CTest quoted argument")
            arguments.append(value)
            position += 1
        else:
            end = position
            while end < len(text) and not text[end].isspace() and text[end] != ")":
                end += 1
            value = text[position:end]
            if not value or any(char in value for char in "$;\\(\"#"):
                raise RealizationError("nonliteral generated CTest argument")
            arguments.append(value)
            position = end
    raise RealizationError("unterminated generated CTest add_test")


def _generated_ctest_commands(build):
    commands = {}
    for path in sorted(Path(build).rglob("CTestTestfile.cmake")):
        text = path.read_text()
        for match in re.finditer(r"(?m)^add_test\(", text):
            args = _cmake_literal_arguments(text, match.end())
            if len(args) < 2:
                raise RealizationError(f"invalid generated CTest registration in {path}")
            name, command = args[0], args[1:]
            if name in commands and commands[name] != command:
                raise RealizationError(f"ambiguous generated CTest command: {name}")
            commands[name] = command
    return commands


def _validate_native_profile(profile, toolchains=None, configuration=None):
    system = {"Darwin": "macos", "Linux": "linux", "Windows": "windows"}.get(platform.system(), platform.system().lower())
    requested = profile.get("platform")
    if requested and {"darwin": "macos"}.get(requested, requested) != system:
        raise RealizationError(f"profile platform {requested} does not match actual native host {system}")
    aliases = {"aarch64": "arm64", "amd64": "x86_64", "AMD64": "x86_64"}
    architecture = aliases.get(platform.machine(), platform.machine())
    expected = profile.get("architecture")
    if expected and expected != "native" and aliases.get(expected, expected) != architecture:
        raise RealizationError(f"profile architecture {expected} does not match actual native host {architecture}")
    if configuration is not None and profile.get("build_type") and profile["build_type"] != configuration:
        raise RealizationError(f"profile build type {profile['build_type']} differs from realized {configuration!r}")
    if toolchains is not None and profile.get("compiler"):
        wanted = {"gcc": {"GNU"}, "clang": {"Clang", "AppleClang"}, "msvc": {"MSVC"}}.get(profile["compiler"], {profile["compiler"]})
        actual = {t.get("compiler", {}).get("id") for t in toolchains.get("toolchains", []) if t.get("language") in ("C", "CXX")}
        if not actual or not actual.issubset(wanted):
            raise RealizationError(f"profile compiler {profile['compiler']} differs from realized {sorted(actual)}")


def configure_profile(repo_root, build_dir, profile_id, profile, source_identity,
                      inventory_hash):
    """Query BEFORE configure; consume replies only AFTER successful generate.

    profile: source_dir (repo-relative), configure_args (argv), environment (map),
    configuration (optional multi-config name), capabilities (list). Extra profile
    fields are included in the fingerprint. Builds must be dedicated per profile.
    """
    root, build = Path(repo_root).resolve(), Path(build_dir).resolve()
    identity = _identity(source_identity)
    _validate_native_profile(profile)
    build.mkdir(parents=True, exist_ok=True)
    record = build / "nostrc-test-realization.json"
    record.unlink(missing_ok=True)  # failed configure must invalidate old success
    query = build / ".cmake/api/v1/query/client-nostrc-test-selection"
    query.mkdir(parents=True, exist_ok=True)
    (query / "codemodel-v2").touch()
    (query / "toolchains-v1").touch()
    token = uuid.uuid4().hex
    env = dict(os.environ, **profile.get("environment", {}))
    args = profile.get("configure_args", [])
    if not isinstance(args, list) or any(not isinstance(x, str) for x in args):
        raise ValueError("configure_args must be an argument array")
    source = (root / profile.get("source_dir", ".")).absolute()
    cmd = ["cmake", "-S", str(source), "-B", str(build), *args,
           f"-DNOSTRC_TEST_SELECTION_TOKEN={token}"]
    before = {p.name: p.read_bytes() for p in (build / ".cmake/api/v1/reply").glob("index-*.json")}
    started = time.monotonic()
    output = _command(cmd, env=env)
    (build / "nostrc-selection-configure.log").write_text(output)
    reply_dir = build / ".cmake/api/v1/reply"
    indexes = sorted(reply_dir.glob("index-*.json"), key=lambda p: p.stat().st_mtime_ns)
    if not indexes or (indexes[-1].name in before and before[indexes[-1].name] == indexes[-1].read_bytes()):
        raise RealizationError("missing/stale File API generation reply")
    index = _read(indexes[-1])
    client = index.get("reply", {}).get("client-nostrc-test-selection", {})
    response = client.get("codemodel-v2", {})
    if "error" in response or "jsonFile" not in response:
        raise RealizationError(f"missing/errored File API codemodel reply: {response}")
    codemodel = _read(reply_dir / response["jsonFile"])
    config_name = profile.get("configuration", "")
    configs = codemodel.get("configurations", [])
    if len(configs) > 1:
        configs = [c for c in configs if c["name"] == config_name]
    if len(configs) != 1:
        raise RealizationError("ambiguous/missing codemodel configuration; specify configuration")
    config_name = configs[0]["name"]
    descriptor_path = build / f"nostrc-test-descriptors-{config_name}.json"
    descriptors = _read(descriptor_path)
    if descriptors.get("schema_version") != 1 or descriptors.get("generation_token") != token:
        raise RealizationError("stale/missing descriptor export")
    ctest_cmd = ["ctest", "--test-dir", str(build), "--show-only=json-v1"]
    if config_name:
        ctest_cmd += ["-C", config_name]
    ctest = json.loads(_command(ctest_cmd, env=env))
    # CTest suppresses command argv when a fresh executable is absent. Recover
    # the literal generated registration, not an executable guessed from its name.
    literal_commands = _generated_ctest_commands(build) if index.get("cmake", {}).get("generator", {}).get("name") == "Ninja" else {}
    for test in ctest.get("tests", []):
        if "command" not in test and test["name"] in literal_commands:
            test["command"] = literal_commands[test["name"]]
    targets = {}
    target_settings = {}
    for ref in configs[0].get("targets", []):
        target = _read(reply_dir / ref["jsonFile"])
        target_settings[target["id"]] = _hash(target)
        targets[target["id"]] = {
            "id": target["id"], "name": target["name"], "type": target["type"],
            "artifacts": [_path(build, a["path"]) for a in target.get("artifacts", [])],
            "sources": [{"path": _path(build if s.get("isGenerated") else source, s["path"]),
                         "generated": s.get("isGenerated", False),
                         "compiled": "compileGroupIndex" in s} for s in target.get("sources", [])],
            "dependencies": [d["id"] for d in target.get("dependencies", [])],
        }
    toolchains = client.get("toolchains-v1", {})
    tools = _read(reply_dir / toolchains["jsonFile"]) if "jsonFile" in toolchains else {}
    _validate_native_profile(profile, tools, config_name)
    runtime_manifest = {}
    for desc in descriptors["tests"]:
        files = []
        for value in desc.get("inputs", []):
            path = Path(value)
            if path.is_dir() and _relative(path.resolve(), root) is not None:
                files.extend(str(p) for p in path.rglob("*") if p.is_file() or p.is_symlink())
            else:
                files.append(value)
        runtime_manifest[desc["name"]] = sorted(set(files))
    realization = {
        "schema_version": 1, "repo_root": str(root), "source_dir": str(source),
        "build_dir": str(build), "profile_id": profile_id, "profile": profile,
        "source_identity": identity, "inventory_hash": inventory_hash,
        "configuration": config_name, "targets": targets,
        "tests": ctest.get("tests", []), "descriptors": descriptors["tests"],
        "runtime_file_manifest": runtime_manifest,
        "toolchains": tools, "cmake": index.get("cmake", {}),
        "target_settings": target_settings, "environment_hash": _hash(env),
        "tool_versions": {"ctest": _command(["ctest", "--version"]).strip(),
                          "ninja": _command(["ninja", "--version"]).strip()
                          if index.get("cmake", {}).get("generator", {}).get("name") == "Ninja" else None},
        "commands": [cmd, ctest_cmd], "generation_token": token,
        "configure_seconds": time.monotonic() - started,
    }
    realization["profile_fingerprint"] = _hash({"profile_id": profile_id, "profile": profile,
        "source_identity": identity, "inventory_hash": inventory_hash,
        "cmake": realization["cmake"], "toolchains": tools,
        "target_settings": target_settings, "environment_hash": realization["environment_hash"],
        "tool_versions": realization["tool_versions"]})
    # Bind all generated metadata. A later native configure invalidates this file.
    bound = [descriptor_path, indexes[-1], reply_dir / response["jsonFile"]]
    bound += [reply_dir / t["jsonFile"] for t in configs[0].get("targets", [])]
    bound += list(build.rglob("CTestTestfile.cmake"))
    realization["generation_files"] = {str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in bound}
    _atomic(record, realization)
    return realization


def load_realization(build_dir, expected_identity=None):
    realization = _read(Path(build_dir) / "nostrc-test-realization.json")
    if realization.get("schema_version") != 1:
        raise RealizationError("unsupported realization schema")
    if expected_identity is not None and realization["source_identity"] != _identity(expected_identity):
        raise RealizationError("execution source identity changed (including dirty snapshot digest)")
    for name, digest in realization.get("generation_files", {}).items():
        p = Path(name)
        if not p.is_file() or hashlib.sha256(p.read_bytes()).hexdigest() != digest:
            raise RealizationError(f"stale realization metadata: {name}")
    if not realization.get("generation_files"):
        raise RealizationError("unbound generation metadata")
    _validate_native_profile(realization["profile"], realization["toolchains"], realization["configuration"])
    return realization


def _test_maps(realization):
    tests = {}
    for test in realization["tests"]:
        if test["name"] in tests:
            raise RealizationError(f"duplicate CTest registration: {test['name']}")
        tests[test["name"]] = test
    descriptors = {}
    for desc in realization["descriptors"]:
        name = desc["name"]
        if name not in tests or name in descriptors:
            raise RealizationError(f"missing test or duplicate descriptor: {name}")
        descriptors[name] = desc
    return tests, descriptors


def _membership(check, profile_id):
    value = check.get("ctest_tests", {})
    if isinstance(value, dict):
        return value.get(profile_id, [])
    return value


def _eligible(check, profile_id):
    return profile_id in check.get("profiles", check.get("eligible_profiles", []))


def _metadata(inventory, realization):
    tests, descriptors = _test_maps(realization)
    for override in inventory.get("test_overrides", []):
        if override.get("profile") not in (None, realization["profile_id"]):
            continue
        name = override.get("name", override.get("test"))
        if name not in tests:
            continue
        existing = descriptors.setdefault(name, {"name": name})
        for key in ("owner", "targets", "inputs", "requires", "expected"):
            if key in override:
                if existing.get(key):
                    raise RealizationError(f"duplicate descriptor/override authority: {name}.{key}")
                existing[key] = _list(override[key])
    return tests, descriptors


def _mapped_targets(test, desc, realization):
    by_name = {t["name"]: t["id"] for t in realization["targets"].values()}
    explicit = desc.get("targets", [])
    result = set()
    for name in explicit:
        if name not in by_name and name not in realization["targets"]:
            raise RealizationError(f"unknown descriptor target {name} for {test['name']}")
        result.add(by_name.get(name, name))
    artifacts = {}
    for target in realization["targets"].values():
        if target["type"] == "EXECUTABLE":
            for path in target["artifacts"]:
                artifacts.setdefault(os.path.normpath(path), []).append(target["id"])
    command = test.get("command", [])
    matches = artifacts.get(os.path.normpath(command[0]), []) if command else []
    if not explicit:
        if len(matches) != 1:
            # Script commands and wrappers need explicit descriptors, even when
            # they mention an executable later in argv.
            if not desc:
                raise RealizationError(f"ambiguous/unmapped command: {test['name']}")
            if not desc.get("inputs") and desc.get("expected") != ["fixture"]:
                raise RealizationError(f"script/wrapper lacks explicit inputs/targets: {test['name']}")
        result.update(matches)
    elif matches and not set(matches).issubset(result):
        raise RealizationError(f"command/descriptor target disagreement: {test['name']}")
    return sorted(result)


def _target_closure(ids, realization):
    found, todo = set(), list(ids)
    while todo:
        ident = todo.pop()
        if ident in found:
            continue
        if ident not in realization["targets"]:
            raise RealizationError(f"unresolved target dependency: {ident}")
        found.add(ident)
        todo.extend(realization["targets"][ident]["dependencies"])
    return sorted(found)


def _fixture_closure(names, tests, descriptors):
    selected, todo = set(), list(names)
    while todo:
        name = todo.pop()
        if name in selected:
            continue
        if name not in tests:
            raise RealizationError(f"missing CTest registration/dependency: {name}")
        selected.add(name)
        props = _properties(tests[name])
        # CTest JSON synthesizes fixture cleanup DEPENDS on *all* consumers,
        # including unselected ones. Exported raw DEPENDS is ordering authority.
        todo.extend(descriptors.get(name, {}).get("declared_depends", _list(props.get("DEPENDS"))))
        fixtures = _list(props.get("FIXTURES_REQUIRED"))
        # Selecting a setup directly also retains its cleanup.
        fixtures += _list(props.get("FIXTURES_SETUP"))
        for fixture in fixtures:
            setups = [n for n, t in tests.items() if fixture in _list(_properties(t).get("FIXTURES_SETUP"))]
            cleanups = [n for n, t in tests.items() if fixture in _list(_properties(t).get("FIXTURES_CLEANUP"))]
            if not setups:
                raise RealizationError(f"missing setup for fixture {fixture}")
            todo.extend(setups + cleanups)
    return sorted(selected)


def resolve_checks(inventory, plan, realization, profile_id):
    """Resolve exact check membership plus fixture/order/runtime/build closure.

    plan.selected_checks is a list of stable IDs (or plan.checks). No name regex
    matching, alias expansion, profile projection or policy broadening happens here.
    """
    if realization["profile_id"] != profile_id:
        raise RealizationError("profile mismatch")
    if plan.get("source_identity", realization["source_identity"]) != realization["source_identity"]:
        raise RealizationError("plan/realization source identity mismatch")
    if plan.get("inventory_hash", realization["inventory_hash"]) != realization["inventory_hash"]:
        raise RealizationError("plan/realization inventory mismatch")
    tests, descriptors = _metadata(inventory, realization)
    claims = {}
    for ident, check in inventory["checks"].items():
        if check.get("kind") == "ctest" and _eligible(check, profile_id):
            for name in _membership(check, profile_id):
                if name in claims:
                    raise RealizationError(f"multiply claimed primary execution: {name}")
                claims[name] = ident
    checks, primary = {}, set()
    for ident in plan.get("selected_checks", plan.get("checks", [])):
        check = inventory["checks"][ident]
        if not _eligible(check, profile_id):
            continue
        if check.get("kind") != "ctest":
            raise RealizationError(f"non-CTest execution requires existing adapter: {ident}")
        names = _membership(check, profile_id)
        if not names:
            raise RealizationError(f"empty selected check: {ident}")
        for name in names:
            if name not in tests:
                raise RealizationError(f"missing selected registration: {name}")
            props = _properties(tests[name])
            if props.get("FIXTURES_SETUP") or props.get("FIXTURES_CLEANUP") or descriptors.get(name, {}).get("expected") == ["fixture"]:
                raise RealizationError(f"fixture claimed as primary execution: {name}")
        checks[ident] = {"tests": sorted(names), "requirement": check.get("requirement", "blocking")}
        primary.update(names)
    selected = _fixture_closure(primary, tests, descriptors)
    ids, inputs, capabilities = set(), set(), set()
    for name in selected:
        desc = descriptors.get(name, {})
        ids.update(_mapped_targets(tests[name], desc, realization))
        inputs.update(desc.get("inputs", []))
        capabilities.update(desc.get("requires", []))
    closure = _target_closure(ids, realization)
    available = set(realization["profile"].get("capabilities", []))
    if not capabilities.issubset(available):
        raise RealizationError(f"unavailable selected capabilities: {sorted(capabilities - available)}")
    result = {"schema_version": 1, "profile_id": profile_id,
        "profile_fingerprint": realization["profile_fingerprint"],
        "source_identity": realization["source_identity"], "inventory_hash": realization["inventory_hash"],
        "plan_hash": _hash(plan), "plan_id": plan.get("plan_id", _hash(plan)),
        "profile_hash": _hash(realization["profile"]), "configuration": realization["configuration"],
        "build_dir": realization["build_dir"], "checks": checks,
        "tests": selected, "primary_tests": sorted(primary), "support_tests": sorted(set(selected) - primary),
        "target_ids": closure, "build_targets": sorted({realization["targets"][i]["name"] for i in ids}),
        "runtime_inputs": sorted(inputs), "required_capabilities": sorted(capabilities),
        "expected_outcomes": {n: descriptors.get(n, {}).get("expected", []) for n in selected},
        "registration_hash": _registration_hash(realization["tests"]),
        "registered_commands": {t["name"]: t["command"] for t in realization["tests"] if "command" in t}}
    result["resolved_hash"] = _hash(result)
    return result


def _owners(inventory, relative):
    if relative is None:
        return set(), "external"
    matches = []
    for rule in inventory.get("path_rules", []):
        exact, prefix = rule.get("exact"), rule.get("prefix", "").rstrip("/")
        if exact == relative or (prefix and (relative == prefix or relative.startswith(prefix + "/"))):
            matches.append((len(exact or prefix), rule))
    if not matches:
        return set(), "unclassified"
    longest = max(n for n, _ in matches)
    rules = [r for n, r in matches if n == longest]
    return {n for r in rules for n in r.get("nodes", [])}, rules[0].get("effect", "impact")


def _impact_prerequisites(inventory, owners):
    seen, todo = set(), list(owners)
    while todo:
        node = todo.pop()
        if node in seen:
            continue
        if node not in inventory["nodes"]:
            raise RealizationError(f"unknown impact owner {node}")
        seen.add(node)
        todo.extend(inventory["nodes"][node].get("depends_on", []))
    return seen


def _ninja_headers(realization):
    if realization.get("cmake", {}).get("generator", {}).get("name") != "Ninja":
        raise RealizationError("discovered-header audit requires existing Ninja profile")
    output = _command(["ninja", "-C", realization["build_dir"], "-t", "deps"])
    headers, current = {}, None
    for line in output.splitlines():
        if line and not line[0].isspace():
            current = line.split(": #deps", 1)[0]
            headers[current] = []
        elif current is not None and line.startswith("    "):
            headers[current].append(line.strip())
    return headers


def check_inputs(inventory, realization, source_identity=None, *, after_build=False):
    """Audit EVERY registered execution, including omitted tests; return gaps.

    A failed audit is never a passing selective receipt, even after broad recovery.
    after_build additionally checks Ninja inputs/discovered headers. An absent
    header index is an explicit gap, not evidence that a target uses no headers.
    """
    if source_identity is not None and _identity(source_identity) != realization["source_identity"]:
        raise RealizationError("input audit identity mismatch")
    tests, descriptors = _metadata(inventory, realization)
    gaps, fingerprints = [], {}
    def gap(kind, **detail):
        gaps.append({"kind": kind, **detail})
    headers = _ninja_headers(realization) if after_build else {}
    claims = {}
    for ident, check in inventory["checks"].items():
        if check.get("kind") != "ctest" or not _eligible(check, realization["profile_id"]):
            continue
        for name in _membership(check, realization["profile_id"]):
            if name not in tests:
                gap("missing_registration", check=ident, test=name)
            if name in claims:
                gap("multiply_claimed_registration", check=ident, test=name)
            claims[name] = ident
    header_count = 0
    audited_targets = set()
    ninja_inputs = {}
    for name, test in sorted(tests.items()):
        desc = descriptors.get(name, {})
        owners = set(desc.get("owner", []))
        if not owners:
            gap("unowned_registration", test=name)
            continue
        covered = _impact_prerequisites(inventory, owners)
        check_owners = set()
        for check in inventory["checks"].values():
            if _eligible(check, realization["profile_id"]) and name in _membership(check, realization["profile_id"]):
                check_owners.update(check.get("nodes", check.get("impact_nodes", [])))
        expected = desc.get("expected", [])
        if expected not in ([], ["pass"], ["fixture"], ["mdk09_incompatibility"]):
            gap("unsupported_expected_outcome", test=name, expected=expected)
        if expected == ["mdk09_incompatibility"]:
            check = inventory["checks"].get(claims.get(name), {})
            spec = check.get("expected_outcome", {})
            if name != "groundhog-mdk011-interop-mdk09-probe" or spec != {"case": "mdk09_incompatibility", "exit_code": 77}:
                gap("unsupported_expected_outcome", test=name, expected=expected)
        elif name in claims and inventory["checks"][claims[name]].get("expected_outcome", "pass") != "pass":
            gap("expected_outcome_disagreement", test=name)
        # Logical checks may add test-only input nodes without making a
        # production component depend on its own fixtures/vectors. Audit that
        # actual selection coverage as well as the registration's owning node.
        covered.update(_impact_prerequisites(inventory, check_owners))
        props = _properties(test)
        support = bool(props.get("FIXTURES_SETUP") or props.get("FIXTURES_CLEANUP") or desc.get("expected") == ["fixture"])
        if not support and not check_owners:
            gap("unclaimed_registration", test=name)
        elif not support and not owners.issubset(_impact_prerequisites(inventory, check_owners)):
            gap("check_descriptor_owner_disagreement", test=name, owners=sorted(owners))
        try:
            ids = _target_closure(_mapped_targets(test, desc, realization), realization)
        except RealizationError as exc:
            gap("target_mapping", test=name, reason=str(exc))
            continue
        inputs = [(p, "runtime") for p in set(desc.get("inputs", []) + realization.get("runtime_file_manifest", {}).get(name, []))]
        for ident in ids:
            target = realization["targets"][ident]
            inputs += [(s["path"], "generated" if s["generated"] else "source") for s in target["sources"]]
            if after_build:
                if ident not in ninja_inputs:
                    output = _command(["ninja", "-C", realization["build_dir"], "-t", "inputs", target["name"]])
                    ninja_inputs[ident] = [(_path(realization["build_dir"], p), "ninja") for p in output.splitlines()]
                inputs += ninja_inputs[ident]
                target_headers = 0
                for obj, dependencies in headers.items():
                    if f"CMakeFiles/{target['name']}.dir/" in obj.replace("\\", "/"):
                        header_count += len(dependencies)
                        target_headers += len(dependencies)
                        inputs += [(_path(realization["build_dir"], p), "header") for p in dependencies]
                if not target_headers and ident not in audited_targets and any(s.get("compiled") for s in target["sources"]):
                    gap("missing_discovered_header_index", target=target["name"])
                audited_targets.add(ident)
        expanded = []
        for value, kind in inputs:
            path = Path(value)
            if kind == "runtime" and _relative(value, realization["repo_root"]) is not None and _relative(path.resolve(), realization["repo_root"]) is None:
                gap("external_symlink_input", test=name, input=value)
                continue
            if path.is_dir() and kind == "runtime":
                files = [p for p in sorted(path.rglob("*")) if p.is_file() or p.is_symlink()]
                if not files:
                    gap("empty_runtime_directory", test=name, input=value)
                expanded += [(str(p), kind) for p in files]
            else:
                expanded.append((value, kind))
        for value, kind in sorted(set(expanded)):
            path = Path(value)
            relative = _relative(value, realization["repo_root"])
            build_relative = _relative(value, realization["build_dir"])
            if kind == "runtime" and not path.exists():
                if build_relative is not None and ids and not after_build:
                    continue  # declared build targets must produce this before execution
                gap("missing_input", test=name, input=value)
                continue
            if kind == "generated" or (build_relative is not None and kind != "header"):
                # Generated files are audited through producer target sources and
                # declared runtime source inputs. Unresolved provenance stays a gap.
                if kind == "runtime" and not ids and not support:
                    gap("generated_runtime_without_target", test=name, input=value)
                continue
            if relative is None:
                if kind in ("runtime", "source"):
                    gap("external_input", test=name, input=value)
                if path.is_file() and kind in ("header", "ninja") and value not in fingerprints:
                    fingerprints[value] = hashlib.sha256(path.read_bytes()).hexdigest()
                continue  # bind system build inputs, but do not invent repository ownership
            if not path.exists():
                gap("missing_input", test=name, input=relative)
                continue
            if _relative(path.resolve(), realization["repo_root"]) is None:
                gap("external_symlink_input", test=name, input=relative)
                continue
            if value not in fingerprints:
                fingerprints[value] = hashlib.sha256(path.read_bytes()).hexdigest()
            input_owners, effect = _owners(inventory, relative)
            if effect == "global":
                continue
            if effect == "non_execution" or not input_owners:
                gap("unowned_input", test=name, input=relative, input_kind=kind)
            elif input_owners.isdisjoint(covered):
                gap("missing_impact_edge", test=name, input=relative,
                    input_kind=kind, owners=sorted(owners), missing_nodes=sorted(input_owners - covered))
    if after_build and not header_count and any(t["type"] in ("EXECUTABLE", "STATIC_LIBRARY", "SHARED_LIBRARY") for t in realization["targets"].values()):
        gap("missing_discovered_header_index")
    result = {"schema_version": 1, "passed": not gaps, "requires_broad": bool(gaps),
        "selective_enforcement": False if gaps or not after_build else True,
        "source_identity": realization["source_identity"], "inventory_hash": realization["inventory_hash"],
        "profile_fingerprint": realization["profile_fingerprint"], "after_build": after_build,
        "gaps": gaps, "input_hashes": fingerprints, "header_entries": header_count,
        "header_index_hash": _hash(headers) if after_build else None}
    result["audit_hash"] = _hash(result)
    return result


def _execution_outcomes(junit, names, expected_outcomes):
    outcomes = {}
    if Path(junit).exists():
        try:
            cases = ET.parse(junit).getroot().iter("testcase")
        except ET.ParseError as exc:
            raise RealizationError(f"malformed JUnit: {exc}") from exc
        for case in cases:
            name = case.get("name")
            if name in outcomes:
                raise RealizationError(f"duplicate JUnit execution: {name}")
            failure = case.find("failure") is not None or case.find("error") is not None
            skipped = case.find("skipped")
            outcomes[name] = ("failed" if failure else "unexpected_skip"
                if skipped is not None or case.get("status") in ("notrun", "disabled") else "passed" if case.get("status") == "run" else "failed")
            if expected_outcomes.get(name) == ["mdk09_incompatibility"]:
                output = case.findtext("system-out", "")
                expected = (name == "groundhog-mdk011-interop-mdk09-probe" and not failure
                    and skipped is not None and skipped.get("message") == "SKIP_RETURN_CODE=77"
                    and "XFAIL [unsupported]: MDK 0.9.0" in output
                    and "unsupported proof version 1" in output and "XPASS" not in output)
                outcomes[name] = "expected_incompatibility" if expected else "failed"
    for name in names:
        outcomes.setdefault(name, "missing")
    if set(outcomes) != set(names):
        raise RealizationError("unexpected JUnit executions")
    return outcomes


def build_resolved(resolved, build_dir, parallel=1):
    """Build selected prerequisites before the mandatory post-build audit."""
    realization = load_realization(build_dir, resolved["source_identity"])
    if realization["profile_fingerprint"] != resolved["profile_fingerprint"]:
        raise RealizationError("build profile identity mismatch")
    if not resolved["build_targets"]:
        return {"commands": [], "build_seconds": 0}
    cmd = ["cmake", "--build", str(build_dir), "--parallel", str(parallel)]
    if resolved["configuration"]:
        cmd += ["--config", resolved["configuration"]]
    cmd += ["--target", *resolved["build_targets"]]
    start = time.monotonic()
    output = _command(cmd, env=dict(os.environ, **realization["profile"].get("environment", {})))
    return {"commands": [cmd], "build_seconds": time.monotonic() - start, "output": output}


def cli(args, policy):
    """Adapter used by test-plan.py; return its documented 0/1/2 exit status.

    This does not provision the runner or synthesize unavailable profile coverage.
    A non-CTest/live lane without an existing adapter fails explicitly.
    """
    import sys
    try:
        profile_id = args.profile
        profile = policy["profiles"][profile_id]
        plan = _read(args.plan) if args.plan else None
        identity = plan["source_identity"] if plan else None
        if args.configure or (args.command == "run" and args.phase == "configure"):
            if identity is None:
                status = _command(["git", "-C", args.repo, "status", "--porcelain"])
                if status:
                    raise RealizationError("dirty configure requires an explicit snapshot plan/source identity")
                identity = {"kind": "committed", "sha": _command(["git", "-C", args.repo, "rev-parse", "HEAD"]).strip()}
            realized = configure_profile(args.repo, args.build_dir, profile_id, profile, identity, _hash(policy))
        else:
            realized = load_realization(args.build_dir, identity)
        if realized["inventory_hash"] != _hash(policy) or realized["profile_id"] != profile_id or realized["profile"] != profile:
            raise RealizationError("realization inventory/profile policy changed; configure again")
        if args.command in ("validate", "check-inputs"):
            result = check_inputs(policy, realized, identity,
                                  after_build=args.command == "check-inputs" and args.phase != "configure")
            code = 0 if result["passed"] else 1
        elif args.command == "run" and args.phase == "configure":
            result, code = realized, 0
        else:
            if plan is None:
                raise ValueError("resolve/run requires a logical plan")
            result = resolve_checks(policy, plan, realized, profile_id)
            if args.resolved:
                previous = _read(args.resolved)
                if previous != result:
                    raise RealizationError("provided resolved plan is stale or differs from current resolution")
            code = 0
            if args.command == "run":
                pre = check_inputs(policy, realized, identity)
                if not pre["passed"]:
                    result, code = pre, 1
                else:
                    build_result = {"commands": [], "build_seconds": 0}
                    if args.phase in ("build", "all"):
                        build_result = build_resolved(result, args.build_dir, profile.get("parallelism", 1))
                    audit = check_inputs(policy, realized, identity, after_build=True)
                    if not audit["passed"]:
                        result, code = audit, 1
                    elif args.phase == "build":
                        result = dict(audit, build=build_result)
                    else:
                        evidence = Path(args.output).absolute().parent / "selection-evidence" if args.output else Path(args.build_dir) / "selection-evidence"
                        result = run_resolved(result, args.build_dir, evidence, args.attempt,
                            input_audit=audit, build=False, parallel=profile.get("parallelism", 1),
                            timeout=profile.get("timeout_seconds"),
                            evidence_paths=[Path(args.build_dir) / "mdk011-interop-artifacts"])
                        result["build"] = build_result
                        # cli() emits an aggregate-facing receipt; run_resolved()
                        # retains the actual post-build audit object for adapters.
                        result["input_audit_details"] = result["input_audit"]
                        result["input_audit"] = result["input_audit_status"]
                        code = 0 if result["outcome"] in ("passed", "not_applicable") else 1
        if args.output:
            _atomic(args.output, result)
        else:
            print(json.dumps(result, sort_keys=True, indent=2))
        return code
    except RealizationError as exc:
        print(f"test-plan: {exc}", file=sys.stderr)
        return 1
    except (OSError, ValueError, KeyError, TypeError) as exc:
        print(f"test-plan: malformed realization invocation: {exc}", file=sys.stderr)
        return 2


def run_resolved(resolved, build_dir, evidence_dir, attempt=1, *, input_audit=None,
                 build=True, parallel=1, timeout=None, evidence_paths=()):
    """One CTest invocation per profile/attempt; no retries or quarantine policy.

    Existing platform/MDK adapters still own isolation locks, images and process
    cleanup. This runner refuses Docker/external-peer profiles until that adapter
    attests/provisions them; it must not race fixed-tag image lifecycles.
    """
    if attempt < 1 or parallel < 1:
        raise ValueError("attempt and parallel must be positive")
    build_dir = Path(build_dir).absolute()
    realization = load_realization(build_dir, resolved["source_identity"])
    if realization["profile_fingerprint"] != resolved["profile_fingerprint"]:
        raise RealizationError("resolved profile fingerprint mismatch")
    if set(resolved["required_capabilities"]) & {"docker", "external_peer"}:
        raise RealizationError("live peer/image lifecycle requires the existing guarded adapter; not implemented here")
    directory = Path(evidence_dir) / resolved["source_identity"].get("head", resolved["source_identity"].get("sha", resolved["source_identity"].get("tested_sha", "source"))) / resolved["profile_id"] / resolved["resolved_hash"] / f"attempt-{attempt}"
    previous_attempts = []
    for previous in range(1, attempt):
        path = directory.parent / f"attempt-{previous}" / "receipt.json"
        earlier = _read(path)
        if any(earlier.get(key) != resolved[key] for key in
               ("source_identity", "inventory_hash", "profile_fingerprint", "resolved_hash")):
            raise RealizationError("retry evidence identity mismatch")
        previous_attempts.append({"receipt": str(path), "outcome": earlier["outcome"],
                                  "checks": earlier.get("checks", {})})
    directory.mkdir(parents=True, exist_ok=False)
    started = time.time()
    commands = []
    receipt = {"schema_version": 1, "source_identity": resolved["source_identity"],
        "inventory_hash": resolved["inventory_hash"], "profile_id": resolved["profile_id"],
        "profile_fingerprint": resolved["profile_fingerprint"], "plan_hash": resolved["plan_hash"],
        "resolved_hash": resolved["resolved_hash"], "attempt": attempt,
        "lane_id": resolved["profile_id"], "profile": resolved["profile_id"],
        "plan_id": resolved["plan_id"], "profile_hash": resolved["profile_hash"],
        "registration_audit": "passed", "job_conclusion": "success",
        "expected_executions": resolved["tests"], "support_tests": resolved["support_tests"],
        "started": started, "commands": commands, "evidence_dir": str(directory)}
    if input_audit is None or not input_audit.get("passed") or not input_audit.get("after_build"):
        raise RealizationError("passing post-build input audit required before execution")
    for key in ("source_identity", "inventory_hash", "profile_fingerprint"):
        if input_audit.get(key) != resolved[key]:
            raise RealizationError(f"input audit identity mismatch: {key}")
    for name, digest in input_audit.get("input_hashes", {}).items():
        if not Path(name).is_file() or hashlib.sha256(Path(name).read_bytes()).hexdigest() != digest:
            raise RealizationError(f"input changed since audit: {name}")
    receipt["input_audit"] = {k: v for k, v in input_audit.items() if k != "input_hashes"}
    receipt["input_audit_status"] = "passed"
    env = dict(os.environ, **realization["profile"].get("environment", {}))
    if _hash(env) != realization["environment_hash"]:
        raise RealizationError("effective environment changed since configure; configure this profile again")
    config = ["-C", resolved["configuration"]] if resolved["configuration"] else []
    if build and resolved["build_targets"]:
        cmd = ["cmake", "--build", str(build_dir), "--parallel", str(parallel)]
        if resolved["configuration"]:
            cmd += ["--config", resolved["configuration"]]
        cmd += ["--target", *resolved["build_targets"]]
        commands.append(cmd)
        begin = time.monotonic()
        (directory / "build.log").write_text(_command(cmd, env=env))
        receipt["build_seconds"] = time.monotonic() - begin
    query = ["ctest", "--test-dir", str(build_dir), *config, "--show-only=json-v1"]
    if resolved["build_targets"]:
        if realization["cmake"].get("generator", {}).get("name") != "Ninja":
            raise RealizationError("execution build-freshness proof requires the existing Ninja profile")
        dry_run = _command(["ninja", "-C", str(build_dir), "-n", *resolved["build_targets"]], env=env)
        if "ninja: no work to do." not in dry_run:
            raise RealizationError("selected targets are not up to date; build and re-audit before execution")
    fresh = json.loads(_command(query, env=env))["tests"]
    if _registration_hash(fresh) != resolved["registration_hash"]:
        raise RealizationError("CTest registrations/properties changed after resolution")
    tests = {t["name"]: t for t in fresh}
    for name, command in resolved["registered_commands"].items():
        if tests[name].get("command") != command:
            raise RealizationError(f"CTest command changed after resolution: {name}")
    for name in resolved["tests"]:
        if not tests[name].get("command"):
            raise RealizationError(f"selected executable/command still unavailable: {name}")
    load_realization(build_dir, resolved["source_identity"])
    if input_audit.get("header_index_hash") != _hash(_ninja_headers(realization)):
        raise RealizationError("discovered header index changed since audit; re-audit after build")
    if _fixture_closure(resolved["primary_tests"], tests, {d["name"]: d for d in realization["descriptors"]}) != resolved["tests"]:
        raise RealizationError("fixture/order closure changed")
    if not resolved["tests"]:
        receipt.update(outcome="not_applicable", observed_executions=[], checks={})
    else:
        indices = [str(i + 1) for i, test in enumerate(fresh) if test["name"] in resolved["tests"]]
        selection = directory / "ctest-indices.txt"
        selection.write_text("0,0,1," + ",".join(indices) + "\n")
        args = ["ctest", "--test-dir", str(build_dir), *config, "-I", str(selection)]
        selected = json.loads(_command([*args, "--show-only=json-v1"], env=env))["tests"]
        if sorted(t["name"] for t in selected) != resolved["tests"]:
            raise RealizationError("CTest indexed selection differs from resolved closure")
        junit = directory / "results.xml"
        cmd = [*args, "--output-on-failure", "--output-junit", str(junit), "--parallel", str(parallel)]
        if timeout is not None:
            cmd += ["--timeout", str(timeout)]
        commands.append(cmd)
        begin = time.monotonic()
        # A nonzero exit is retained alongside structured results, never discarded.
        process = _execute(cmd, env=env, log_path=directory / "ctest.log")
        receipt["test_seconds"] = time.monotonic() - begin
        receipt["ctest_exit_code"] = process.returncode
        for value in evidence_paths:
            path = Path(value)
            if path.exists():
                destination = directory / "runtime-evidence" / path.name
                destination.parent.mkdir(parents=True, exist_ok=True)
                if path.is_dir():
                    shutil.copytree(path, destination)
                else:
                    shutil.copy2(path, destination)
        outcomes = _execution_outcomes(junit, resolved["tests"], resolved["expected_outcomes"])
        receipt["observed_executions"] = outcomes
        receipt["checks"] = {ident: {"outcome": next((outcomes[n] for n in check["tests"] if outcomes[n] not in ("passed", "expected_incompatibility")),
                        "expected_incompatibility" if any(outcomes[n] == "expected_incompatibility" for n in check["tests"]) else "passed"),
                                               "executions": check["tests"],
                                               "expected_executions": check["tests"],
                                               "observed_executions": [n for n in check["tests"] if outcomes[n] != "missing"],
                                               **({"exception_case": "mdk09_incompatibility", "exit_code": 77}
                                                  if any(outcomes[n] == "expected_incompatibility" for n in check["tests"]) else {})}
                             for ident, check in resolved["checks"].items()}
        receipt["outcome"] = "passed" if not process.returncode and all(o in ("passed", "expected_incompatibility") for o in outcomes.values()) else "failed"
    receipt["previous_attempts"] = previous_attempts
    receipt["clean_first_attempt"] = attempt == 1 and receipt["outcome"] == "passed"
    for ident, result in receipt["checks"].items():
        result["initial_failure"] = any(
            earlier["checks"].get(ident, {}).get("outcome", "missing") not in ("passed", "expected_incompatibility")
            for earlier in previous_attempts)
    if receipt["outcome"] not in ("passed", "not_applicable"):
        receipt["job_conclusion"] = "failure"
    receipt["ended"] = time.time()
    _atomic(directory / "receipt.json", receipt)
    return receipt
