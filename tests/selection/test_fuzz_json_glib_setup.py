"""Static contract for the pinned JSON-GLib fuzz setup (nostrc-0cfsg.29).

Ubuntu 24.04's libjson-glib 1.8.0 leaks the nodes of extra root statements in
json_parser_load_from_data() (GNOME/json-glib#74, fixed by MR !82, first
released in 1.10.0). The fuzz jobs must therefore link an official, hash-pinned
JSON-GLib release without weakening leak detection. These checks do not attest
hosted execution; they keep the setup from regressing silently.
"""
from pathlib import Path
import re
import subprocess
import unittest


ROOT = Path(__file__).resolve().parents[2]
WORKFLOW = ROOT / ".github/workflows/grotto-fuzz.yml"
FUZZ = ROOT / "apps/grotto/tests/fuzz"
SETUP = "apps/grotto/tests/fuzz/json-glib-setup.sh"
PINNED_SHA256 = "55c5c141a564245b8f8fbe7698663c87a45a7333c2a2c56f06f811ab73b212dd"


def job(name):
    return re.search(r"^  " + re.escape(name) + r":\n(?:(?!^  [\w-]+:).)*",
                     WORKFLOW.read_text(), re.M | re.S).group()


def step(block, name):
    return re.search(r"^      - name: " + re.escape(name) + r"\n(?:(?!^      - ).)*",
                     block, re.M | re.S).group()


def step_index(block, name):
    return block.index("      - name: " + name + "\n")


class FuzzJsonGlibSetupTests(unittest.TestCase):
    def test_setup_script_pins_official_release_by_hash(self):
        script = (FUZZ / "json-glib-setup.sh").read_text()
        self.assertIn("VERSION=1.10.8", script)
        self.assertIn("SHA256=" + PINNED_SHA256, script)
        self.assertIn("https://download.gnome.org/sources/json-glib/1.10/json-glib-${VERSION}.tar.xz", script)
        self.assertIn("sha256sum -c", script)
        self.assertIn("pkg-config --modversion json-glib-1.0 | grep -x", script)

    def test_setup_does_not_weaken_leak_detection_or_patch_dependency(self):
        text = "\n".join(p.read_text() for p in (
            FUZZ / "json-glib-setup.sh", FUZZ / "json_glib_leak_probe.c", WORKFLOW))
        for forbidden in ("detect_leaks=0", "LSAN_OPTIONS", "lsan.supp", "suppressions=",
                          "ASAN_OPTIONS", "patch -p", "git apply", "-ignore_ooms",
                          "-max_total_time=1"):
            self.assertNotIn(forbidden, text)

    def test_json_event_job_installs_before_configure_and_verifies_after_build(self):
        block = job("fuzz-test")
        install = step(block, "Install pinned JSON-GLib")
        self.assertIn("if: matrix.target == 'json-event'", install)
        self.assertIn(SETUP + ' install "$RUNNER_TEMP/json-glib-1.10.8" "$GITHUB_ENV"', install)
        verify = step(block, "Verify pinned JSON-GLib")
        self.assertIn("if: matrix.target == 'json-event'", verify)
        self.assertIn(SETUP + ' verify "$JSON_GLIB_PREFIX" build/apps/grotto/tests/fuzz/fuzz-json-event', verify)
        order = [step_index(block, n) for n in (
            "Install pinned JSON-GLib", "Configure with fuzz targets", "Build fuzz targets",
            "Verify pinned JSON-GLib", "Run fuzz-${{ matrix.target }}")]
        self.assertEqual(order, sorted(order))

    def test_coverage_job_uses_same_pinned_library(self):
        block = job("coverage-report")
        self.assertIn(SETUP + ' install "$RUNNER_TEMP/json-glib-1.10.8" "$GITHUB_ENV"',
                      step(block, "Install pinned JSON-GLib"))
        self.assertIn(SETUP + ' verify "$JSON_GLIB_PREFIX" build/apps/grotto/tests/fuzz/fuzz-json-event',
                      step(block, "Verify pinned JSON-GLib"))
        order = [step_index(block, n) for n in (
            "Install pinned JSON-GLib", "Configure with coverage", "Build fuzz targets",
            "Verify pinned JSON-GLib", "Run corpus for coverage")]
        self.assertEqual(order, sorted(order))

    def test_fuzz_run_commands_and_duration_are_unchanged(self):
        run = step(job("fuzz-test"), "Run fuzz-${{ matrix.target }}")
        self.assertIn('${FUZZ_BIN} "${CORPUS_DIR}" \\\n', run)
        self.assertIn("-max_total_time=${{ needs.select-fuzz.outputs.duration }}", run)
        self.assertIn('-artifact_prefix="${ARTIFACTS_DIR}/"', run)
        self.assertIn("-print_final_stats=1", run)
        self.assertIn("DEFAULT_FUZZ_DURATION: 60", WORKFLOW.read_text())

    def test_verify_checks_loaded_library_and_link_resolution(self):
        script = (FUZZ / "json-glib-setup.sh").read_text()
        self.assertIn("fsanitize=address", script)
        self.assertIn("loaded_library=", script)
        self.assertIn("/proc/self/maps", (FUZZ / "json_glib_leak_probe.c").read_text())
        self.assertIn('ldd "$bin"', script)
        self.assertIn('-Wl,-rpath,$prefix/lib', script)  # via LDFLAGS exported to GITHUB_ENV

    def test_probe_inputs_are_multi_root_documents(self):
        probe = (FUZZ / "json_glib_leak_probe.c").read_text()
        for literal in ('"{}{}"', '"[][]"', '"[][]{}{}"'):
            self.assertIn(literal, probe)

    def test_setup_script_is_valid_shell(self):
        subprocess.run(["bash", "-n", str(FUZZ / "json-glib-setup.sh")], check=True)


class FuzzJsonGlibEnrollmentTests(unittest.TestCase):
    def test_exact_sources_jobs_and_finite_historical_inverse(self):
        import hashlib
        from selector_support import s
        from profile_contract_support import validate_job_accounting
        import w11_json_glib_support as wave
        policy = s.load_inventory(ROOT / 'tests/selection/inventory.json')
        data = wave.witness()
        validate_job_accounting(policy, ROOT)
        for name, expected in data['source_inputs'].items():
            self.assertEqual(hashlib.sha256((ROOT / name).read_bytes()).hexdigest(), expected, name)
            self.assertIn('{source}/' + name, policy['checks']['selection.contract']['runtime_inputs'])
        for rule in data['rules']:
            self.assertEqual(set(s.classify(policy, rule['exact'])['nodes']), {'grotto.gui', 'selection.contract'})
        previous = wave.before_policy(policy)
        self.assertEqual(s.digest(previous), data['before_policy_hash'])
        self.assertEqual(wave.before_policy(previous), previous)
        source = wave.before_source(data['workflow']['path'], WORKFLOW.read_bytes())
        self.assertEqual(hashlib.sha256(source).hexdigest(), data['workflow']['before_sha256'])
        self.assertEqual(data['producer']['tarball_sha256'], PINNED_SHA256)

    def test_changed_or_stripped_enrollment_never_resets_historical_goldens(self):
        import copy
        from selector_support import s
        import w11_json_glib_support as wave
        policy = s.load_inventory(ROOT / 'tests/selection/inventory.json')
        data = wave.witness()
        for mutate in (
            lambda p: p['checks']['selection.contract']['runtime_inputs'].remove(data['input_additions'][0]),
            lambda p: p['workflow_jobs'][next(iter(data['jobs']))].update(source_sha256='0' * 64),
            lambda p: p['path_rules'].remove(data['rules'][0]),
        ):
            changed = copy.deepcopy(policy)
            mutate(changed)
            with self.subTest(mutate=mutate), self.assertRaises(ValueError):
                wave.before_policy(changed)
        with self.assertRaises(ValueError):
            wave.before_source(data['workflow']['path'], WORKFLOW.read_bytes() + b'\n# unknown change\n')


if __name__ == "__main__":
    unittest.main()
