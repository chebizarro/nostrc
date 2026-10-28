#!/usr/bin/env python3
"""Focused Linux fixture checks; no package manager or network access required.

Run with python3 scripts/test-ci-package-cache.py on Linux (GNU sed).
Real archive reuse and hosted cache-service proof are separate execution checks.
"""
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("workflow_yaml", ROOT / "scripts/sanitizer-gate-ci.py")
yaml = importlib.util.module_from_spec(spec)
spec.loader.exec_module(yaml)


def load(path):
    return yaml.parse_yaml((ROOT / path).read_text())


class WorkflowTests(unittest.TestCase):
    def test_every_apt_installing_job_has_cache_before_install(self):
        found = []
        for path in sorted((ROOT / ".github/workflows").glob("*.yml")):
            workflow = yaml.parse_yaml(path.read_text())
            for job_id, job in workflow.get("jobs", {}).items():
                cached = False
                for step in job.get("steps", []):
                    if str(step.get("uses", "")).endswith("/.github/actions/cache-apt"):
                        cached = True
                        self.assertEqual(step.get("id"), "apt-cache")
                        saves = [s for s in job["steps"] if s.get("uses") == "actions/cache/save@v4"]
                        self.assertEqual(len(saves), 1, (path.name, job_id))
                        save = saves[0]
                        self.assertIn("always()", save["if"])
                        self.assertIn("steps.apt-cache.outputs.key != ''", save["if"])
                        self.assertNotIn("continue-on-error", save)
                        self.assertEqual(save["with"]["path"], "/var/cache/nostrc-apt/archives/*.deb")
                        key = save["with"]["key"]
                        self.assertTrue(key.startswith("${{ steps.apt-cache.outputs.key }}"))
                        self.assertIn("job.status != 'success'", key)
                        self.assertIn("-partial-", key)
                        self.assertIn("github.run_id", key)
                        self.assertIn("github.run_attempt", key)
                        self.assertIn("strategy.job-index", key)
                    run = step.get("run", "")
                    if "apt-get" in run or "install-linux-build-deps.sh" in run or "${{ matrix.install }}" in run:
                        self.assertTrue(cached, (path.name, job_id, step.get("name")))
                        self.assertLess(job["steps"].index(step), job["steps"].index(save))
                        found.append((path.name, job_id))
        self.assertTrue(found)

    def test_cache_contains_only_downloads_and_keys_install_sources(self):
        action = load(".github/actions/cache-apt/action.yml")
        cache = next(s for s in action["runs"]["steps"] if s.get("uses") == "actions/cache/restore@v4")
        # Restore-only has no deferred action lookup after Checkout deletes the
        # temporary bootstrap snapshot; the explicit remote save owns no tree.
        self.assertFalse(any(s.get("uses") == "actions/cache@v4" for s in action["runs"]["steps"]))
        self.assertEqual(cache["with"]["path"], "/var/cache/nostrc-apt/archives/*.deb")
        self.assertIn("steps.identity.outputs.compatible", cache["with"]["restore-keys"])
        self.assertIn("steps.identity.outputs.image", cache["with"]["key"])
        self.assertIn("steps.identity.outputs.packages", cache["with"]["key"])
        prepare = action["runs"]["steps"][0]["run"]
        for source in ("ImageVersion", "os-release", "uname -m", ".github/workflows/*.yml", "scripts/ci/*.sh", "-name control"):
            self.assertIn(source, prepare)

    def test_runner_image_rotation_preserves_compatible_archive_identity(self):
        prepare = load(".github/actions/cache-apt/action.yml")["runs"]["steps"][0]["run"]
        # Run the actual identity/hash portion without the privileged setup.
        identity_script = prepare[prepare.index("# Includes the runner image release"):]
        with tempfile.TemporaryDirectory() as temp:
            values = []
            for version in ("20261021.78.2", "20261027.1.0"):
                output = Path(temp) / version
                env = dict(os.environ, source_root=str(ROOT), ACTION_PATH=str(ROOT / ".github/actions/cache-apt"),
                           VARIANT="ubuntu-24.04-asan-ubsan", ImageOS="ubuntu24", ImageVersion=version,
                           GITHUB_OUTPUT=str(output))
                subprocess.run(["bash", "-ec", identity_script], env=env, check=True, capture_output=True)
                values.append(dict(line.split("=", 1) for line in output.read_text().splitlines()))
            self.assertNotEqual(values[0]["image"], values[1]["image"])
            self.assertEqual(values[0]["compatible"], values[1]["compatible"])
            self.assertEqual(values[0]["packages"], values[1]["packages"])

    def test_bootstrap_save_needs_no_deleted_local_action(self):
        job = load(".github/workflows/nostr-homed-ci.yml")["jobs"]["build-and-test"]
        steps = job["steps"]
        bootstrap = next(i for i, s in enumerate(steps) if s.get("uses") == "./.ci-bootstrap/.github/actions/cache-apt")
        checkout = next(i for i, s in enumerate(steps) if i > bootstrap and s.get("uses") == "actions/checkout@v4")
        save = next(i for i, s in enumerate(steps) if s.get("uses") == "actions/cache/save@v4")
        self.assertLess(bootstrap, checkout)
        self.assertLess(checkout, save)
        self.assertFalse(any(".ci-bootstrap" in s.get("uses", "") or ".ci-bootstrap" in s.get("run", "") for s in steps[checkout + 1:]))

    def test_windows_entrypoint_keeps_one_list_and_fatal_final_attempt(self):
        workflow = load(".github/workflows/windows-validation.yml")
        setup = next(s for s in workflow["jobs"]["build"]["steps"] if s.get("uses") == "./.github/actions/setup-msys2")
        self.assertIn("mingw-w64-ucrt-x86_64-toolchain", setup["with"]["install"])
        action = load(".github/actions/setup-msys2/action.yml")
        installs = [s for s in action["runs"]["steps"] if "install" in s.get("with", {})]
        self.assertEqual(len(installs), 2)
        for step in installs:
            self.assertEqual(step["with"]["install"], "${{ inputs.install }}")
            self.assertEqual(str(step["with"]["cache"]).lower(), "true")
            self.assertEqual(str(step["with"]["update"]).lower(), "false")
        self.assertNotIn("continue-on-error", installs[-1])
        steps = action["runs"]["steps"]
        policy = next(i for i, s in enumerate(steps) if s.get("name") == "Reapply mirror policy after core upgrade")
        first_install = steps.index(installs[0])
        self.assertLess(policy, first_install)
        self.assertTrue(all(i < policy for i, s in enumerate(steps) if str(s.get("with", {}).get("update")).lower() == "true"))
        self.assertIn("timeout-minutes", setup)


class TransportTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.sources = self.root / "apt/sources.list.d"
        self.sources.mkdir(parents=True)
        self.source = self.sources / "ubuntu.sources"
        self.source.write_text("URIs: http://azure.archive.ubuntu.com/ubuntu/ https://ports.ubuntu.com/ubuntu-ports\n")
        (self.root / "apt/sources.list").write_text(
            "deb https://deb.debian.org/debian bookworm main\n"
            "deb https://deb.debian.org/debian-security bookworm-security main\n"
            "deb https://packages.example.org/debian stable main\n")
        self.log = self.root / "calls.jsonl"
        self.backoff = self.root / "backoff.log"
        sleep = self.root / "sleep"
        sleep.write_text('#!/bin/sh\nprintf "%s\\n" "$1" >> "$BACKOFF_LOG"\n')
        sleep.chmod(0o755)
        self.fake = self.root / "fake-apt"
        self.fake.write_text("#!" + sys.executable + "\n" + """
import json, os, sys
from pathlib import Path
log = Path(os.environ['CALL_LOG'])
calls = log.read_text().splitlines() if log.exists() else []
with log.open('a') as stream:
    stream.write(json.dumps(sys.argv[1:]) + '\\n')
results = json.loads(os.environ['RESULTS'])
sys.exit(results[min(len(calls), len(results)-1)])
""")
        self.fake.chmod(0o755)
        self.cache = self.root / "archives"
        self.cache.mkdir()
        self.metadata = self.root / "signed-metadata"
        apt_cache = self.root / "apt-cache"
        apt_cache.write_text(f'#!/bin/sh\ncat "{self.metadata}"\n')
        apt_cache.chmod(0o755)
        self.wrapper = self.root / "apt-get"
        script = (ROOT / "scripts/ci-apt-get.sh").read_text()
        self.wrapper.write_text(script.replace("/usr/bin/apt-get", str(self.fake))
                                .replace("/usr/bin/apt-cache", str(apt_cache))
                                .replace("/etc/apt", str(self.root / "apt"))
                                .replace("/var/cache/nostrc-apt/archives", str(self.cache)))

    def run_apt(self, results, *args):
        env = dict(os.environ, CALL_LOG=str(self.log), BACKOFF_LOG=str(self.backoff),
                   RESULTS=json.dumps(results), PATH=str(self.root) + os.pathsep + os.environ["PATH"])
        result = subprocess.run(["bash", str(self.wrapper), *args], env=env, capture_output=True, text=True)
        calls = [json.loads(line) for line in self.log.read_text().splitlines()] if self.log.exists() else []
        return result, calls

    def cached_archive(self, version="1", filename="foo_1_all.deb"):
        archive = self.cache / filename
        archive.write_bytes(b"archive bytes for SHA256 fixture")
        digest = hashlib.sha256(archive.read_bytes()).hexdigest()
        self.metadata.write_text(f"Package: foo\nVersion: {version}\nArchitecture: all\nSHA256: {digest}\n\n")
        return archive

    def test_verified_archive_is_retained_for_install(self):
        archive = self.cached_archive()
        result, calls = self.run_apt([0], "install", "foo")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(len(calls), 1)
        self.assertTrue(archive.exists())

    def test_same_size_corrupt_archive_is_discarded_before_install(self):
        archive = self.cached_archive()
        original = archive.read_bytes()
        archive.write_bytes(b"X" + original[1:])
        result, calls = self.run_apt([0], "install", "foo")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(len(calls), 1)
        self.assertFalse(archive.exists())
        self.assertIn("not verified by current signed indexes", result.stderr)

    def test_epoch_encoded_archive_names_are_supported(self):
        archive = self.cached_archive("1:1", "foo_1%3a1_all.deb")
        result, _ = self.run_apt([0], "install", "foo")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertTrue(archive.exists())

    def test_epoch_is_not_dropped_from_archive_identity(self):
        archive = self.cached_archive("1:1", "foo_1_all.deb")
        result, _ = self.run_apt([0], "install", "foo")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertFalse(archive.exists())

    def test_unavailable_signed_metadata_fails_before_install(self):
        self.cached_archive()
        self.metadata.unlink()
        result, calls = self.run_apt([0], "install", "foo")
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(calls, [])

    def test_success_executes_install_without_retry(self):
        result, calls = self.run_apt([0], "install", "-y", "libfoo=1.2")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(len(calls), 1)
        self.assertIn("libfoo=1.2", calls[0])
        self.assertIn("APT::Update::Error-Mode=any", calls[0])

    def test_install_failure_refreshes_alternate_mirror_and_retries_same_args(self):
        result, calls = self.run_apt([100, 0, 0], "install", "-y", "libfoo=1.2")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(len(calls), 3)
        self.assertIn("update", calls[1])
        self.assertEqual(calls[0], calls[2])
        self.assertIn("mirrors.edge.kernel.org/ubuntu/", self.source.read_text())
        self.assertIn("mirrors.mit.edu/ubuntu-ports", self.source.read_text())
        debian = (self.root / "apt/sources.list").read_text()
        self.assertIn("ftp.us.debian.org/debian bookworm", debian)
        self.assertIn("deb.debian.org/debian-security", debian)
        self.assertIn("packages.example.org/debian", debian)

    def test_failed_refresh_never_proceeds_to_install(self):
        result, calls = self.run_apt([100, 100], "install", "missing-capability")
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(len(calls), 5)
        self.assertTrue(all("update" in call for call in calls[1:]))
        self.assertEqual(self.backoff.read_text().splitlines(), ["5", "10", "20"])

    def test_permanent_install_failure_is_fatal(self):
        result, calls = self.run_apt([100, 0, 100], "install", "missing-capability")
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(len(calls), 3)

    def test_sync_failure_retries_with_backoff_then_fails_visibly(self):
        result, calls = self.run_apt([100], "update", "-qq")
        self.assertEqual(result.returncode, 100)
        self.assertEqual(len(calls), 4)
        self.assertEqual(self.backoff.read_text().splitlines(), ["5", "10", "20"])
        self.assertIn("required indexes remain unavailable", result.stderr)

    def test_ddebs_index_mismatch_can_recover_on_sixth_attempt(self):
        ddebs = self.sources / "ddebs.list"
        ddebs.write_text("deb http://ddebs.ubuntu.com noble-updates main universe\n")
        result, calls = self.run_apt([100, 100, 100, 100, 100, 0], "update")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(len(calls), 6)
        self.assertEqual(self.backoff.read_text().splitlines(), ["5", "10", "20", "40", "60"])
        self.assertIn("ddebs.ubuntu.com", ddebs.read_text())
        self.assertIn("APT::Update::Error-Mode=any", calls[-1])
        self.assertIn("Acquire::http::No-Cache=true", calls[-1])

    def test_ddebs_permanent_failure_is_not_skipped(self):
        (self.sources / "ddebs.list").write_text("deb http://ddebs.ubuntu.com noble-updates main universe\n")
        result, calls = self.run_apt([100], "update")
        self.assertEqual(result.returncode, 100)
        self.assertEqual(len(calls), 6)
        self.assertIn("failed after 6 attempts", result.stderr)

    def test_interrupt_is_not_retried(self):
        result, calls = self.run_apt([130], "update")
        self.assertEqual(result.returncode, 130)
        self.assertEqual(len(calls), 1)
        self.assertFalse(self.backoff.exists())

    def test_msys2_mirrors_keep_literal_variables_and_bounded_curl(self):
        pacman = self.root / "pacman"
        (pacman / "pacman.d").mkdir(parents=True)
        (pacman / "pacman.conf").write_text("[options]\nSigLevel = Required DatabaseOptional\n[msys]\nInclude = /etc/pacman.d/mirrorlist.msys\n")
        script = (ROOT / "scripts/ci-msys2-mirrors.sh").read_text().replace("/etc/pacman", str(pacman / "pacman"))
        # Map the production /etc/pacman.conf and /etc/pacman.d together.
        result = subprocess.run(["bash", "-c", script], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        mingw = (pacman / "pacman.d/mirrorlist.mingw").read_text()
        self.assertEqual(mingw.splitlines()[0], "Server = https://repo.msys2.org/mingw/$repo/")
        self.assertIn("mirror.msys2.org", mingw)
        conf = (pacman / "pacman.conf").read_text()
        self.assertIn("SigLevel = Required DatabaseOptional", conf)
        self.assertLess(conf.index("XferCommand"), conf.index("[msys]"))
        for option in ("--retry 2", "--connect-timeout 15", "--max-time 180", "--speed-time 30"):
            self.assertIn(option, conf)


if __name__ == "__main__":
    if sys.platform != "linux":
        sys.exit("This focused fixture suite requires Linux/GNU sed; use an explicit Linux container.")
    unittest.main()
