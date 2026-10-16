"""Explicit production command inputs; not general Python import inference."""
import copy
import hashlib
import importlib.util
import os
from pathlib import Path
import shutil
import tempfile
import unittest
from types import SimpleNamespace

from realization_support import ROOT, r
from selector_support import change, s


# These are the external sources read/copied/executed by the current suite.
# Workflows, version sources and selection helpers have separately enumerated
# discovery boundaries below. Never recursively audit mutable __pycache__.
SUPPORT_INPUTS = {
    ".github/workflows/dependency_contract.py",
    "CMakeLists.txt",
    "VERSION_MANIFEST.md",
    "cmake/NostrcTestInventory.cmake",
    "Testing/CMakeLists.txt",
    "Testing/TestInventory.cmake",
    "tests/CMakeLists.txt",
    "tests/TestInventory.cmake",
    "nostr-gtk/TestInventory.cmake",
    "gnome/groundhog/TestInventory.cmake",
    "gnome/nostr-homed/tests/unit/test_manifest.c",
    "gnome/nostr-homed/tests/unit/test_cache.c",
    "gnome/nostr-homed/tests/unit/test_cache_config.c",
    "gnome/nostr-homed/tests/unit/test_uid_map.c",
    "gnome/nostr-homed/tests/unit/test_groups.c",
    "gnome/nostr-homed/src/common/nostr_manifest.c",
    "gnome/nostr-homed/src/common/nostr_cache.c",
    "scripts/check-macos-keychain.sh",
    "scripts/linux-gate.sh",
    "scripts/mdk-gate.py",
    "scripts/sanitizer-gate-ci.py",
    "scripts/test-gate.sh",
    "scripts/test-plan.py",
    "scripts/test_selection.py",
    "scripts/test_selection_realization.py",
    "scripts/contained-gate.py",
    "scripts/contained-runner.py",
    "scripts/test_selection_custody.py",
    "tests/selection/test_contained_custody_contract.py",
    "tests/custody/workload.py",
    "tests/custody/test_contained_linux.py",
    "tests/custody/test_contained_engine.py",
}


def expected_inputs(policy, root):
    tracked = {os.fsdecode(p) for p in s.git(root, "ls-files", "-z", "--",
               "tests/selection", ".github/workflows", ":(glob)**/TestInventory.cmake").stdout.split(b"\0") if p}
    paths = SUPPORT_INPUTS | {p for p in tracked if p.startswith("tests/selection/")
                            or Path(p).suffix in {".yml", ".yaml"}
                            or Path(p).name == "TestInventory.cmake"}
    for component in policy["release_components"].values():
        paths.update(source["path"] for source in component["version_sources"])
        if component.get("full_version_source"):
            paths.add(component["full_version_source"]["path"])
    return paths


def validate_source_closure(policy, root):
    inputs = policy["checks"]["selection.contract"]["runtime_inputs"]
    expected = {"{source}/" + p for p in expected_inputs(policy, root)}
    if len(inputs) != len(set(inputs)) or set(inputs) != expected:
        raise ValueError("command source closure changed: " + str(sorted(set(inputs) ^ expected)))
    # Ignored Python/workflow sources can still be discovered by unittest or
    # source_jobs; reject them rather than silently importing unaudited code.
    discovered = {"{source}/" + p.relative_to(root).as_posix()
                  for p in (root / "tests/selection").rglob("*.py")
                  if "__pycache__" not in p.parts}
    discovered.update("{source}/" + p.relative_to(root).as_posix()
                      for p in (root / ".github/workflows").glob("*")
                      if p.suffix in {".yml", ".yaml"})
    # The collector loads descriptors beside registered source CMakeLists.
    # Check those source directories even when a descriptor is not tracked yet.
    cmake_sources = s.git(root, "ls-files", "-z", "--", "CMakeLists.txt",
                         ":(glob)**/CMakeLists.txt").stdout.split(b"\0")
    discovered.update("{source}/" + descriptor.relative_to(root).as_posix()
                      for source in cmake_sources if source
                      for descriptor in [root / Path(os.fsdecode(source)).parent / "TestInventory.cmake"]
                      if descriptor.exists())
    if discovered - set(inputs):
        raise ValueError("undeclared discoverable command source: " + str(sorted(discovered - set(inputs))))
    for value in inputs:
        path = root / value.removeprefix("{source}/")
        if not path.is_file() or path.is_symlink():
            raise ValueError("missing/non-file command input: " + value)


def copy_inputs(policy, root):
    for value in policy["checks"]["selection.contract"]["runtime_inputs"]:
        relative = value.removeprefix("{source}/")
        target = root / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(ROOT / relative, target)


class SelectionCommandContractTests(unittest.TestCase):
    def setUp(self):
        self.policy = s.load_inventory(ROOT / "tests/selection/inventory.json")

    def test_production_explicit_source_closure_and_nonmutating_environment(self):
        validate_source_closure(self.policy, ROOT)
        check = self.policy["checks"]["selection.contract"]
        self.assertEqual(check["command"], ["python3", "-m", "unittest", "discover", "-s", "tests/selection"])
        self.assertEqual(check["working_directory"], "{source}")
        self.assertEqual(check["environment"], {"PYTHONDONTWRITEBYTECODE": "1", "PYTHONNOUSERSITE": "1", "PYTHONPATH": ""})
        self.assertFalse(any("__pycache__" in p or p.endswith(".pyc") for p in check["runtime_inputs"]))
        self.assertFalse(self.policy["enforcement_ready"])
        s.validate_inventory(self.policy)

    def test_required_suite_tools_cannot_turn_missing_capabilities_into_skipped_success(self):
        for name in ("git", "cmake", "ninja", os.environ.get("CC", "cc")):
            with self.subTest(tool=name):
                self.assertIsNotNone(shutil.which(name), "selection.contract requires " + name)

    def test_new_tracked_test_or_helper_requires_explicit_runtime_input(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            s.git(root, "init", "-q")
            copy_inputs(self.policy, root)
            s.git(root, "add", ".")
            validate_source_closure(self.policy, root)
            for name in ("test_new_contract.py", "new_support.py", "fixtures/new-input.json"):
                path = root / "tests/selection" / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text("new tracked source\n")
                s.git(root, "add", str(path))
                self.assertIn(path.relative_to(root).as_posix(), expected_inputs(self.policy, root))
                with self.assertRaisesRegex(ValueError, name):
                    validate_source_closure(self.policy, root)

    def test_untracked_discoverable_helper_is_not_hidden_by_git_inventory(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            s.git(root, "init", "-q")
            copy_inputs(self.policy, root)
            s.git(root, "add", ".")
            validate_source_closure(self.policy, root)
            path = root / "tests/selection/ignored_support.py"
            path.write_text("# untracked or ignored imports are not audit inputs\n")
            with self.assertRaisesRegex(ValueError, "undeclared discoverable"):
                validate_source_closure(self.policy, root)

    def test_new_tracked_or_untracked_descriptor_requires_explicit_runtime_input(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            s.git(root, "init", "-q")
            copy_inputs(self.policy, root)
            source_dir = root / "nips/new-component"
            source_dir.mkdir(parents=True)
            (source_dir / "CMakeLists.txt").write_text("# descriptor source directory\n")
            s.git(root, "add", ".")
            validate_source_closure(self.policy, root)
            descriptor = source_dir / "TestInventory.cmake"
            descriptor.write_text("include_guard(DIRECTORY)\n")
            with self.assertRaisesRegex(ValueError, "undeclared discoverable.*TestInventory"):
                validate_source_closure(self.policy, root)
            s.git(root, "add", str(descriptor))
            self.assertIn(descriptor.relative_to(root).as_posix(), expected_inputs(self.policy, root))
            with self.assertRaisesRegex(ValueError, "command source closure changed.*TestInventory"):
                validate_source_closure(self.policy, root)

    def test_each_actual_input_has_semantic_impact_on_command(self):
        covered = r._impact_prerequisites(self.policy, self.policy["checks"]["selection.contract"]["nodes"])
        for path in sorted(expected_inputs(self.policy, ROOT)):
            with self.subTest(path=path):
                owners, effect = r._owners(self.policy, path)
                self.assertTrue(effect == "global" or (effect == "impact" and owners & covered), (path, owners, effect))
                plan = s.make_plan(self.policy, [change(path)], head="a" * 40, base="b" * 40)
                self.assertIn("selection.contract", plan["selected_checks"])


class SelectionCommandInputAuditTests(unittest.TestCase):
    """Actual production input list in a small committed synthetic audit tree.

    No production platform/execution claim: only the fixture profile is realized.
    The full production command is exercised separately on its supported host.
    """
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = (Path(self.temp.name) / "source").resolve()
        self.root.mkdir()
        self.build = (Path(self.temp.name) / "build").resolve()
        self.policy = copy.deepcopy(s.load_inventory(ROOT / "tests/selection/inventory.json"))
        check = self.policy["checks"]["selection.contract"]
        self.policy["checks"] = {"selection.contract": check}
        check["profiles"] = ["closure.fixture"]
        copy_inputs(self.policy, self.root)
        r._git(self.root, "init", "-q")
        # Keep automatic maintenance, but join it before copying/removing .git.
        # A detached first-commit repack can otherwise outlive the fixture.
        r._git(self.root, "config", "gc.autoDetach", "false")
        r._git(self.root, "config", "maintenance.autoDetach", "false")
        r._git(self.root, "add", ".")
        r._git(self.root, "-c", "user.name=Input fixture", "-c", "user.email=input@example.invalid",
               "-c", "core.hooksPath=/dev/null", "commit", "-qm", "fixture")
        self.identity = {"kind": "committed", "sha": r._git(self.root, "rev-parse", "HEAD").decode().strip()}
        profile = {"source_dir": ".", "configure_args": [], "environment": {}}
        self.policy["profiles"]["closure.fixture"] = profile
        self.realized = r.configure_command_profile(self.root, self.build, "closure.fixture",
            profile, self.identity, s.digest(self.policy))
        self.resolved = r.resolve_checks(self.policy,
            {"selected_checks": ["selection.contract"], "source_identity": self.identity,
             "inventory_hash": s.digest(self.policy)}, self.realized, "closure.fixture")

    def audit(self):
        return r.check_inputs(self.policy, self.realized, after_build=True)

    def test_complete_explicit_inputs_are_hashed_without_cache_directory(self):
        audit = self.audit()
        self.assertTrue(audit["passed"], audit["gaps"])
        for value in self.policy["checks"]["selection.contract"]["runtime_inputs"]:
            path = self.root / value.removeprefix("{source}/")
            self.assertEqual(audit["input_hashes"][str(path)], hashlib.sha256(path.read_bytes()).hexdigest())

    def test_missing_or_moved_helper_fails_closed(self):
        helper = self.root / "tests/selection/profile_contract_support.py"
        helper.rename(helper.with_name("moved_support.py"))
        audit = self.audit()
        self.assertFalse(audit["passed"])
        self.assertIn({"kind": "missing_input", "check": "selection.contract", "input": str(helper)}, audit["gaps"])
        with self.assertRaises(r.RealizationError):
            r.run_resolved(self.resolved, self.build, Path(self.temp.name) / "evidence", input_audit=audit, build=False)

    def test_input_drift_fails_closed(self):
        audit = self.audit()
        self.assertTrue(audit["passed"], audit["gaps"])
        helper = self.root / "tests/selection/fixtures/profile_exports.json"
        helper.write_text(helper.read_text() + "\n")
        with self.assertRaisesRegex(r.RealizationError, "source|checkout|identity|modified|dirty"):
            r.run_resolved(self.resolved, self.build, Path(self.temp.name) / "evidence", input_audit=audit, build=False)

    def test_wrong_source_checkout_fails_closed(self):
        wrong = (Path(self.temp.name) / "other-source").resolve()
        shutil.copytree(self.root, wrong)
        spec = importlib.util.spec_from_file_location("closure_cli", ROOT / "scripts/test-plan.py")
        cli = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(cli)
        args = SimpleNamespace(plan=None, repo=str(wrong), build_dir=str(self.build),
                               profile="closure.fixture", configure=False, phase="all", command="run")
        with self.assertRaisesRegex(s.CheckFailure, "repo_root does not match admitted execution checkout"):
            cli.realization_command(args, self.policy, r)

    def test_new_ownership_without_command_edge_is_rejected(self):
        path = "tests/selection/profile_contract_support.py"
        self.policy["nodes"]["new.helper.owner"] = {"depends_on": []}
        self.policy["path_rules"].append({"exact": path, "effect": "impact", "nodes": ["new.helper.owner"]})
        audit = self.audit()
        self.assertFalse(audit["passed"])
        self.assertIn({"kind": "missing_impact_edge", "check": "selection.contract", "input": path}, audit["gaps"])
