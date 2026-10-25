"""Operational CMake/CTest adapter; deliberately not an attestation receipt.

The logical plan and exact registration names come from test_selection. Builds
use the original default target graph, and CTest owns fixture scheduling. This
route never normalizes a legacy result into a diagnostic or release receipt.
"""
import contextlib
import fcntl
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import runpy
import shutil
import subprocess
import tempfile
import xml.etree.ElementTree as ET

import test_selection as selection


DEFAULT_PROFILES = {
    "Linux": {"core.linux", "grotto.linux", "groundhog.linux", "homed.linux.units",
              "groundhog.mdk08", "groundhog.mdk011", "nostr_seal.linux", "desktop.signer.linux",
              "groundhog.identity_compat.linux", "selection.operational.linux"},
    "Darwin": {"core.macos.asan", "libgo.macos.asan", "selection.operational.macos"},
}
GUARDS = (
    ("scripts/check-unsequenced-args.py", ["--root", "{source}"]),
    ("scripts/check-gui-test-gdk-frame.py", ["--root", "{source}"]),
    ("scripts/check-linux-ci-packages.py", ["--root", "{source}"]),
    ("gnome/groundhog/tests/check_privacy.py", ["{source}/gnome/groundhog"]),
)


def fixture_closure(export, selected):
    tests = {test["name"]: test for test in export["tests"]}
    if len(tests) != len(export["tests"]):
        raise RuntimeError("duplicate CTest registration names cannot be selected unambiguously")
    missing = set(selected) - tests.keys()
    if missing:
        raise RuntimeError("selected CTest registrations missing after configure: " + ", ".join(sorted(missing)))
    properties = {name: {p["name"]: p["value"] for p in test.get("properties", [])}
                  for name, test in tests.items()}
    def values(name, key):
        value = properties[name].get(key, [])
        return value.split(";") if isinstance(value, str) else value
    closure = set(selected)
    while True:
        fixtures = {f for name in closure for f in values(name, "FIXTURES_REQUIRED")}
        added = {name for name in tests if fixtures & set(values(name, "FIXTURES_SETUP") + values(name, "FIXTURES_CLEANUP"))}
        # DEPENDS is ordering, not a request to execute an unrelated test.
        if added <= closure:
            return sorted(closure)
        closure |= added


def fixture_capabilities(build, configuration, names):
    path = build / f"nostrc-test-descriptors-{configuration}.json"
    if not path.is_file():
        return set()  # Older standalone projects rely on the check contract.
    descriptors = selection.read_json(path)["tests"]
    return {cap for item in descriptors if item["name"] in names for cap in item.get("requires", [])}


def ctest_regex(names):
    # CTest uses POSIX extended regular expressions, not Python's full dialect.
    def escape(name):
        return re.sub(r"([.\[\]\\()*+?^$|{}])", r"\\\1", name)
    return "^(" + "|".join(escape(n) for n in sorted(names)) + ")$"


def check_junit(path, expected, allowed_skips=()):
    root = ET.parse(path).getroot()
    entries = list(root.iter("testcase"))
    cases = {case.attrib["name"]: case for case in entries}
    if len(cases) != len(entries):
        raise RuntimeError("duplicate CTest result names cannot establish successful coverage")
    if set(cases) != set(expected):
        raise RuntimeError("CTest result membership differs from selected fixture closure")
    outcomes = {}
    for name, case in cases.items():
        if case.find("failure") is not None or case.find("error") is not None:
            raise RuntimeError("CTest failed: " + name)
        skipped = case.find("skipped")
        if skipped is not None:
            # The original probe emits this only after its precise refusal
            # assertions, then exits 77. Missing peers/disabled tests are not it.
            marker = "XFAIL [unsupported]: MDK 0.9.0 (0xF2F1 v1 proof) is incompatible:"
            if (name not in allowed_skips or skipped.get("message") != "SKIP_RETURN_CODE=77"
                    or marker not in (case.findtext("system-out") or "")):
                raise RuntimeError("selected test skipped, not successful coverage: " + name)
            outcomes[name] = "expected_incompatibility"
        else:
            if case.get("status") not in {None, "run"}:
                raise RuntimeError("selected test was not run: " + name)
            outcomes[name] = "passed"
    return outcomes


def _expand(value, source, build):
    return value.replace("{source_root}", str(source)).replace("{source}", str(source)).replace("{build}", str(build))


def _environment(profile, source, phase, build=None):
    # Do not pass agent-control credentials or a developer's session bus into tests.
    env = {k: v for k, v in os.environ.items() if not k.startswith(("CLAUDE_CODE_", "CODEX_"))
           and k not in {"SSH_AUTH_SOCK", "DBUS_SESSION_BUS_ADDRESS", "NOSTR_SIGNER_ENDPOINT", "NOSTR_SIGNER_TOKEN", "NOSTR_SIGNER_SOCK"}}
    if profile.get("compiler") in {"gcc", "clang"}:
        env.update(CC=profile["compiler"], CXX="g++" if profile["compiler"] == "gcc" else "clang++")
    for values in (profile.get("environment", {}), profile.get("phase_environments", {}).get(phase, {})):
        env.update({k: _expand(v, source, build or source) for k, v in values.items()})
    return env


def _prefix(capabilities):
    unknown = capabilities - {"private_bus", "display", "docker", "nak", "gnome_keyring",
                              "signer_endpoint", "external_peer", "prepared_peer"}
    if unknown:
        raise RuntimeError("selected capabilities require their specialized adapter: " + ", ".join(sorted(unknown)))
    prefix = []
    if "private_bus" in capabilities:
        if not shutil.which("dbus-run-session"):
            raise RuntimeError("private_bus requires dbus-run-session")
        prefix += ["dbus-run-session", "--"]
    if "display" in capabilities:
        if not shutil.which("xvfb-run"):
            raise RuntimeError("display requires a private xvfb-run display")
        prefix += ["xvfb-run", "-a", "-s", "-screen 0 1280x800x24"]
    if "signer_endpoint" in capabilities or "external_peer" in capabilities or "prepared_peer" in capabilities:
        raise RuntimeError("selected external signer/peer requires its provisioned adapter; not skipped")
    return prefix


@contextlib.contextmanager
def _signer_fixture(profile, source, build, lane_dir, state):
    # Reuse the existing approved fixture and credential-delivery boundary,
    # including its live caller fingerprints, private Secret Service and pidfds.
    if profile.get("environment", {}).get("NOSTRC_TEST_NIP5F_APPROVAL") != "unidentified-get-public-key-once":
        raise RuntimeError("explicit unidentified get_public_key approval is required")
    if any(os.environ.get(name) for name in ("NOSTR_SIGNER_ENDPOINT", "NOSTR_SIGNER_TOKEN", "NOSTR_SIGNER_SOCK")):
        raise RuntimeError("ambient signer authority is not permitted for the isolated fixture")
    from test_selection_services import signer_resource_delivery, signer_fixture_exposure
    fixture = runpy.run_path(str(source / "tests/runtime/test_provisioned_nip5f.py"))
    with tempfile.TemporaryDirectory(prefix="nostrc-sign-", dir="/tmp") as root:
        config = fixture["prepare_fixture"](source, build, lane_dir / "signer", consent=True,
                    private_root=root, prebuilt=True, environment=_environment(profile, source, "test", build))
        owned = fixture["Signer"]("legacy", config=config)
        runtime = state["signer"] = {"status": "failed", "complete": False, "complete_gate_eligible": False,
                                     "scope": "legacy provisioned fixture, not profile attestation", "inputs": config.inputs}
        in_flight = False
        try:
            owned.start()
            with signer_resource_delivery(owned.env["NOSTR_SIGNER_ENDPOINT"], owned.env["NOSTR_SIGNER_TOKEN"],
                                          root, [lane_dir]) as (spec, delivery):
                runtime["delivery"] = delivery
                yield ["--resource-spec-file", spec]
            runtime["approval_snapshot"] = owned.snapshot()
            if runtime["approval_snapshot"] != "(uint32 1, uint32 1, uint32 0)":
                raise RuntimeError("signer did not observe exactly one completed explicit consent")
            runtime["status"] = "passed"
        except BaseException:
            in_flight = True
            raise
        finally:
            try:
                owned.finish()
                if any(fixture["identity"](item["path"]) != item for item in config.inputs.values()):
                    raise RuntimeError("signer fixture input identity changed")
            except BaseException:
                runtime["status"] = "failed"
                in_flight = True
                raise
            finally:
                runtime["cleanup"] = owned.result
                signer_fixture_exposure(runtime, getattr(owned, "token", None), lane_dir, in_flight)


def _lock_profile(profile_id):
    locks = Path("/tmp") / f"nostrc-test-gate-{os.getuid()}"
    locks.mkdir(mode=0o700, exist_ok=True)
    if locks.is_symlink() or locks.stat().st_uid != os.getuid() or locks.stat().st_mode & 0o077:
        raise RuntimeError("unsafe machine-wide gate lock directory")
    path = locks / (hashlib.sha256(profile_id.encode()).hexdigest() + ".lock")
    fd = os.open(path, os.O_CREAT | os.O_RDWR | os.O_NOFOLLOW, 0o600)
    try:
        fcntl.flock(fd, fcntl.LOCK_EX)
        return fd
    except BaseException:
        os.close(fd)
        raise


def run(policy, plan, source, directory, record, *, command, materialize, profiles=None):
    selection.validate_plan(plan, policy)
    unknown = set(profiles or []) - policy["profiles"].keys()
    if unknown:
        raise RuntimeError("unknown explicit profiles: " + ", ".join(sorted(unknown)))
    source, directory = Path(source), Path(directory)
    initialized = set()
    record.update(execution_kind="legacy_selected_v1", complete=False, complete_gate_eligible=False,
                  scope="explicit profiles" if profiles else "host local defaults", guards=[], omitted_lanes=[])
    # Cheap guards run even for prose-only changes. They are not inferred from
    # an old namespace-controller check and never counted as that check passing.
    for index, (relative, arguments) in enumerate(GUARDS):
        script = source / relative
        if script.is_file():
            argv = ["python3", str(script), *[_expand(a, source, directory) for a in arguments]]
            command(argv, cwd=source, env=_environment({}, source, "test"), log=directory / f"guard-{index}.log")
            record["guards"].append({"argv": argv, "status": "passed"})
    if "selection.contract" in plan["selected_checks"]:
        argv = ["python3", "-m", "unittest", "discover", "-s", "tests/selection", "-p", "test_selector*.py"]
        command(argv, cwd=source, env=_environment({}, source, "test"), log=directory / "selector-guards.log")
        record["guards"].append({"argv": argv, "status": "passed", "scope": "selector regressions, not namespace policy qualification"})
    requested = set(profiles) if profiles else DEFAULT_PROFILES.get(platform.system(), set())
    if not profiles and not requested:
        raise RuntimeError("no local defaults for this host; select an explicit supported profile")
    for lane in plan["lanes"]:
        profile_id = lane["profile"]
        mdk_unneeded = (not profiles and profile_id in {"groundhog.mdk08", "groundhog.mdk011"}
                        and not any(ident.startswith("mdk") for ident in lane["checks"]))
        if profile_id not in requested or mdk_unneeded:
            record["omitted_lanes"].append({"profile": profile_id, "checks": lane["checks"], "status": "not_executed",
                                            "reason": "no affected MDK check" if mdk_unneeded else "outside host-local profile projection"})
            print(f"test-gate: {profile_id}: not_executed (local profile projection)", flush=True)
            continue
        profile = policy["profiles"][profile_id]
        lane_dir = directory / ("legacy-" + hashlib.sha256(profile_id.encode()).hexdigest()[:16])
        lane_dir.mkdir()
        state = {"profile": profile_id, "status": "failed", "checks": lane["checks"], "commands": [],
                 "complete": False, "complete_gate_eligible": False}
        record["lanes"].append(state)
        def execute(argv, phase="test", cwd=source, prefix=(), environment=None):
            argv = [*prefix, *map(str, argv)]
            state["commands"].append(argv)
            env = _environment(profile, source, phase, lane_dir / "build")
            env.update({k: _expand(v, source, lane_dir / "build") for k, v in (environment or {}).items()})
            command(argv, cwd=cwd, env=env, log=lane_dir / f"command-{len(state['commands'])}.log")
            return lane_dir / f"command-{len(state['commands'])}.log"
        lock = None
        try:
            lock = _lock_profile(profile_id)
            host = {"Darwin": "macos", "Linux": "linux"}.get(platform.system())
            arch = {"aarch64": "arm64", "amd64": "x86_64"}.get(platform.machine().lower(), platform.machine().lower())
            authored_arch = {"aarch64": "arm64", "amd64": "x86_64"}.get(profile["architecture"], profile["architecture"])
            if profile["platform"] != host or authored_arch not in {arch, "any"}:
                raise RuntimeError("profile does not match native host; use its CI/container route")
            checks = [policy["checks"][ident] for ident in lane["checks"]]
            caps = {cap for check in checks for cap in check.get("requires", [])}
            signer = "signer_endpoint" in caps
            if signer:
                consumers = {name for check in checks if "signer_endpoint" in check.get("requires", [])
                             for name in check.get("ctest_tests", {}).get(profile_id, [])}
                if consumers != {"test_nip5f_tcp"} or any(check["kind"] != "ctest" and "signer_endpoint" in check.get("requires", []) for check in checks):
                    raise RuntimeError("signer fixture supports only the exact test_nip5f_tcp consumer")
            prefix = _prefix(caps - {"signer_endpoint"})
            for tool, executable in (("docker", "docker"), ("nak", "nak"), ("gnome_keyring", "gnome-keyring-daemon")):
                if tool not in caps:
                    continue
                path = shutil.which(executable)
                if not path:
                    raise RuntimeError("selected capability executable unavailable: " + executable)
                version_log = execute([path, "--version"])
                version = version_log.read_text().strip()
                if tool == "nak" and (not version or "debug" in version.lower()):
                    raise RuntimeError("nak executable must stamp its actual module version")
                state.setdefault("tools", {})[tool] = {"path": str(Path(path).resolve()), "version": version,
                    "sha256": hashlib.sha256(Path(path).read_bytes()).hexdigest()}
            if "docker" in caps:
                execute(["docker", "info"])
            if any(check["kind"] != "ctest" and "gnome_keyring" in check.get("requires", []) for check in checks):
                raise RuntimeError("keyring commands require their throwaway service adapter; not an ambient service")
            build = lane_dir / "build"
            has_build = any(check["kind"] != "command" or check.get("build_targets") for check in checks)
            configuration = profile["build_type"]
            if has_build:
                materialize(profile, lane_dir, initialized)
                flags = [_expand(a, source, build) for a in profile["configure_args"]]
                if host == "macos" and profile["source_dir"] != "libgo":
                    flags += ["-DNIP55L_SECRET_BACKEND=libsecret", "-DGROUNDHOG_STORE_KEY_DEFAULT_BACKEND=secret-service"]
                execute(["cmake", "-S", source / profile["source_dir"], "-B", build, *flags], "configure")
                jobs = profile.get("build_parallelism", profile.get("parallelism", 1))
                execute(["cmake", "--build", build, "--config", configuration, "--parallel", jobs], "build")
                targets = sorted({target for check in checks for target in check.get("build_targets", [])})
                if targets:
                    execute(["cmake", "--build", build, "--config", configuration, "--parallel", jobs, "--target", *targets], "build")
                for check in checks:
                    if check["kind"] == "build_contract" and check.get("build_scope") != "default":
                        execute([_expand(a, source, build) for a in check["command"]], "build",
                                cwd=Path(_expand(check["working_directory"], source, build)),
                                prefix=_prefix(set(check.get("requires", []))), environment=check.get("environment"))
            names = sorted({name for check in checks if check["kind"] == "ctest" for name in check["ctest_tests"][profile_id]})
            if names:
                path = execute(["ctest", "--test-dir", build, "-C", configuration, "--show-only=json-v1"])
                export = json.loads(path.read_text())
                # A conservative broad fallback executes the whole configured
                # universe, including new registrations absent from inventory.
                closure = fixture_closure(export, names)
                if plan["effective_mode"] == "broad" or profile_id in plan.get("metadata_broad_profiles", []):
                    closure = sorted(test["name"] for test in export["tests"])
                fixture_caps = fixture_capabilities(build, configuration, closure)
                extra = fixture_caps - caps - {"private_bus", "display"}
                if extra:
                    raise RuntimeError("fixture closure adds unprovisioned capabilities: " + ", ".join(sorted(extra)))
                prefix = _prefix((caps | fixture_caps) - {"signer_endpoint"})
                state["required_capabilities"] = sorted(caps | fixture_caps)
                state["selected_tests"], state["fixture_closure"] = names, closure
                junit = lane_dir / "ctest.xml"
                resources = _signer_fixture(profile, source, build, lane_dir, state) if signer else contextlib.nullcontext([])
                with resources as resource_args:
                    execute(["ctest", "--test-dir", build, "-C", configuration, "--output-on-failure", "--no-tests=error",
                             "--timeout", profile.get("timeout_seconds", 3600),
                             "--parallel", profile.get("test_parallelism", profile.get("parallelism", 1)),
                             "-R", ctest_regex(closure), "--output-junit", junit, *resource_args], prefix=prefix)
                allowed = {name for check in checks if isinstance(check.get("expected_outcome"), dict)
                           and check["expected_outcome"].get("case") == "mdk09_incompatibility"
                           for name in check["ctest_tests"][profile_id]}
                state["test_outcomes"] = check_junit(junit, closure, allowed)
            for ident in lane["checks"]:
                check = policy["checks"][ident]
                if check["kind"] != "command":
                    continue  # Default build already physically ran; not a synthetic all target.
                if check.get("required_evidence"):
                    raise RuntimeError("command requires specialized evidence adapter: " + ident)
                argv = [_expand(a, source, build) for a in check["command"]]
                execute(argv, cwd=Path(_expand(check["working_directory"], source, build)),
                        prefix=_prefix(set(check.get("requires", []))), environment=check.get("environment"))
            state["status"] = "passed"
        except (RuntimeError, OSError, ValueError, subprocess.SubprocessError, ET.ParseError) as exc:
            state["error"] = str(exc)
        finally:
            if lock is not None:
                os.close(lock)
            selection.write_json(lane_dir / "legacy-result.json", state)
    record["status"] = "failed" if any(lane["status"] != "passed" for lane in record["lanes"]) else "partial"
    selection.write_json(directory / "legacy-result.json", record)
