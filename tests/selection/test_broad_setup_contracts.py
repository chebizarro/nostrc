"""Bounded prerequisite contracts; these do not attest hosted execution."""
import os
from pathlib import Path
import re
import subprocess
import tempfile
import unittest


WORKFLOWS = Path(__file__).resolve().parents[2] / ".github/workflows"


def job(workflow, name):
    text = (WORKFLOWS / workflow).read_text()
    return re.search(r"^  " + re.escape(name) + r":\n(?:(?!^  [\w-]+:).)*",
                     text, re.M | re.S).group()


def step(block, name):
    return re.search(r"^      - name: " + re.escape(name) + r"\n(?:(?!^      - ).)*",
                     block, re.M | re.S).group()


class BroadPrerequisiteTests(unittest.TestCase):
    def test_bp_installs_required_sodium_without_narrowing_build(self):
        for workflow, name in (("bp-matrix.yml", "bp-matrix"),
                               ("bp-matrix-nightly.yml", "nightly-matrix")):
            with self.subTest(workflow=workflow):
                block = job(workflow, name)
                install = step(block, "Install dependencies")
                for package in ("libsodium-dev", "libnsync-dev", "libwebsockets-dev", "libgit2-dev",
                                "libgtk-4-dev", "libpeas-2-dev", "libmicrohttpd-dev", "gettext"):
                    self.assertIn(package, install)
                self.assertIn("run: cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DNOSTR_ENABLE_METRICS=ON\n", block)
                self.assertIn("run: cmake --build build -j2\n", block)
                self.assertIn('--caps "1,2,4,8,16,32,64"', block)
                self.assertIn("--modes try,block", block)
                self.assertIn("scripts/aggregate_bp_matrix.py", block)
                self.assertIn("scripts/generate_bp_report.py", block)

    def test_fuzz_and_coverage_install_git2_and_keep_real_targets(self):
        for name in ("fuzz-test", "coverage-report"):
            with self.subTest(job=name):
                block = job("grotto-fuzz.yml", name)
                install = step(block, "Install dependencies")
                for package in ("libgit2-dev", "libclang-rt-dev", "libgtk-4-dev", "libadwaita-1-dev", "libpeas-2-dev",
                                "libqrencode-dev", "libzbar-dev", "libgstreamer-plugins-base1.0-dev"):
                    self.assertIn(package, install)
                self.assertIn("-DBUILD_FUZZ_TARGETS=ON", block)
                self.assertIn("-DFUZZ_ENGINE=LIBFUZZER", block)
                self.assertIn("-DENABLE_NIP49=ON", block)
                self.assertIn("cmake --build build --target fuzz-", block)

    def test_json_fuzz_uses_real_seed_directory_without_renaming_artifact(self):
        block = job("grotto-fuzz.yml", "fuzz-test")
        self.assertIn("FUZZ_CORPUS: ${{ matrix.target == 'json-event' && 'json_event' || matrix.target }}", block)
        run = step(block, "Run fuzz-${{ matrix.target }}")
        self.assertIn('CORPUS_DIR="build/apps/grotto/tests/fuzz/corpus/${FUZZ_CORPUS}"', run)
        upload = step(block, "Upload updated corpus")
        self.assertIn("fuzz-corpus-${{ matrix.target }}", upload)
        self.assertIn("corpus/${{ env.FUZZ_CORPUS }}/", upload)
        seeds = WORKFLOWS.parents[1] / "apps/grotto/tests/fuzz/corpus/json_event"
        self.assertTrue(any(seeds.iterdir()))

    def test_blueprint_setup_is_pinned_and_verified_in_each_build_job(self):
        for workflow, name in (("bp-matrix.yml", "bp-matrix"),
                               ("bp-matrix-nightly.yml", "nightly-matrix"),
                               ("grotto-fuzz.yml", "fuzz-test"),
                               ("grotto-fuzz.yml", "coverage-report")):
            with self.subTest(workflow=workflow, job=name):
                setup = step(job(workflow, name), "Install pinned Blueprint compiler")
                self.assertIn("blueprint-compiler-0.22.2.tar.xz", setup)
                self.assertIn("231d72efbac931c235ac3e022fe94982095c20d88721d9a8dcf60152f2017e07", setup)
                self.assertIn("sha256sum -c", setup)
                self.assertIn("blueprint-compiler --version | grep -x 0.22.2", setup)

    def test_bp_metrics_are_opted_in_without_changing_library_default(self):
        cmake = (WORKFLOWS.parents[1] / "libnostr/CMakeLists.txt").read_text()
        self.assertIn('option(NOSTR_ENABLE_METRICS "Enable internal metrics instrumentation" OFF)', cmake)
        for workflow, name in (("bp-matrix.yml", "bp-matrix"),
                               ("bp-matrix-nightly.yml", "nightly-matrix")):
            self.assertIn("-DNOSTR_ENABLE_METRICS=ON", step(job(workflow, name), "Configure (CMake)"))

    def test_homed_trust_is_limited_to_container_checkout_read(self):
        block = job("nostr-homed-ci.yml", "build-and-test")
        for distro in ("ubuntu-24.04", "debian-bookworm", "fedora-40", "archlinux"):
            self.assertIn("- distro: " + distro, block)
        verify = step(block, "Verify requested checkout")
        self.assertIn("if: inputs.sha != ''", verify)
        self.assertIn("REQUESTED_SHA: ${{ inputs.sha }}", verify)
        self.assertIn('git -c safe.directory="$GITHUB_WORKSPACE" rev-parse HEAD', verify)
        self.assertNotIn("safe.directory", job("nostr-homed-ci.yml", "systemd-opensession"))
        self.assertNotIn("--global", verify)
        self.assertNotIn("safe.directory=*", block)


class ContainerCheckoutVerificationTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="nostrc-checkout-trust-")
        self.addCleanup(self.temp.cleanup)
        root = Path(self.temp.name)
        self.checkout = root / "checkout"
        self.other = root / "other"
        self.env = dict(os.environ, HOME=str(root), XDG_CONFIG_HOME=str(root / "xdg"),
                        GIT_CONFIG_NOSYSTEM="1", GIT_CONFIG_GLOBAL=str(root / "gitconfig"))
        # Do not inherit command-scope Git configuration from the invoking harness.
        for key in list(self.env):
            if key.startswith("GIT_CONFIG_") and key not in ("GIT_CONFIG_NOSYSTEM", "GIT_CONFIG_GLOBAL"):
                del self.env[key]
        for repo in (self.checkout, self.other):
            subprocess.run(["git", "init", "-q", str(repo)], env=self.env, check=True)
            subprocess.run(["git", "-C", str(repo), "-c", "user.name=Test", "-c",
                            "user.email=test@example.invalid", "commit", "-qm", "fixture", "--allow-empty"],
                           env=self.env, check=True)
        self.sha = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=self.checkout,
                                           env=self.env, text=True).strip()
        self.env.update(GIT_TEST_ASSUME_DIFFERENT_OWNER="1", GITHUB_WORKSPACE=str(self.checkout),
                        REQUESTED_SHA=self.sha)
        verify = step(job("nostr-homed-ci.yml", "build-and-test"), "Verify requested checkout")
        self.command = re.search(r"^        run: (.+)$", verify, re.M).group(1)

    def run_verify(self, cwd=None):
        return subprocess.run(["bash", "-e", "-c", self.command], cwd=cwd or self.checkout,
                              env=self.env, capture_output=True, text=True)

    def assert_untrusted(self, repo):
        result = subprocess.run(["git", "rev-parse", "HEAD"], cwd=repo, env=self.env,
                                capture_output=True, text=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("dubious ownership", result.stderr)

    def test_untrusted_checkout_exact_sha_succeeds_without_persistent_trust(self):
        self.assert_untrusted(self.checkout)
        self.assertEqual(self.run_verify().returncode, 0)
        self.assert_untrusted(self.checkout)
        self.assertFalse(Path(self.env["GIT_CONFIG_GLOBAL"]).exists())

    def test_wrong_sha_still_fails(self):
        self.env["REQUESTED_SHA"] = "0" * 40
        self.assertNotEqual(self.run_verify().returncode, 0)

    def test_unrelated_repository_remains_untrusted(self):
        self.assert_untrusted(self.other)
        result = self.run_verify(self.other)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("dubious ownership", result.stderr)


if __name__ == "__main__":
    unittest.main()
