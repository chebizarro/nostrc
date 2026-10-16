"""Miniature real shared sanitizer phases; not production Groundhog/LSan acceptance."""
import importlib.util
import json
import os
from pathlib import Path
import platform
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("shared_sanitizer_adapter", ROOT / "scripts/sanitizer-gate-ci.py")
ADAPTER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(ADAPTER)


def miniature(repo, host=None):
    """Tracked, isolated fixture also usable for a real native Linux/Docker diagnostic."""
    host = host or {"Darwin": "macos", "Linux": "linux"}[platform.system()]
    repo.mkdir(parents=True)
    def write(path, text):
        target = repo / path
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(text)
    for name in ("test-plan.py", "test_selection.py", "test_selection_realization.py", "sanitizer-gate-ci.py"):
        write("scripts/" + name, (ROOT / "scripts" / name).read_text())
    write("cmake/NostrcTestInventory.cmake", (ROOT / "cmake/NostrcTestInventory.cmake").read_text())
    write(".gitignore", "__pycache__/\n")
    write("app/main.c", '''#include <stdlib.h>
#include <string.h>
#include <stdio.h>
int main(void) {
  const char *phase = getenv("ADAPTER_PHASE"), *test = getenv("ADAPTER_TEST_ONLY");
  if (!phase || strcmp(phase, "test") || !test || strcmp(test, "only-test")) return 2;
  puts("real miniature sanitized test");
  return 0;
}
''')
    write("app/build.py", '''import os, pathlib, sys
assert os.environ['ADAPTER_PHASE'] == 'build'
assert 'ADAPTER_TEST_ONLY' not in os.environ
assert 'GROUNDHOG_SANITIZER_TESTS' not in os.environ
pathlib.Path(sys.argv[1]).write_text('actual build phase\\n')
''')
    write("app/application.c", "int main(void) { return 0; }\n")
    write("app/lsan.supp", "# Deliberately no project leak suppressions.\n")
    write("CMakeLists.txt", '''cmake_minimum_required(VERSION 3.22)
project(shared_adapter_fixture C)
enable_testing()
include(cmake/NostrcTestInventory.cmake)
if(NOT "$ENV{ADAPTER_PHASE}" STREQUAL "configure" OR DEFINED ENV{ADAPTER_TEST_ONLY} OR DEFINED ENV{GROUNDHOG_SANITIZER_TESTS})
  message(FATAL_ERROR "wrong configure phase environment")
endif()
file(WRITE "${CMAKE_BINARY_DIR}/configure.phase" "$ENV{ADAPTER_PHASE}")
add_custom_command(OUTPUT "${CMAKE_BINARY_DIR}/build.phase"
  COMMAND python3 "${CMAKE_SOURCE_DIR}/app/build.py" "${CMAKE_BINARY_DIR}/build.phase"
  DEPENDS "${CMAKE_SOURCE_DIR}/app/build.py")
add_custom_target(phase-check DEPENDS "${CMAKE_BINARY_DIR}/build.phase")
add_executable(groundhog app/application.c)
add_dependencies(groundhog phase-check)
add_executable(test-groundhog-probe app/main.c)
foreach(target groundhog test-groundhog-probe)
  target_compile_options(${target} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
  target_link_options(${target} PRIVATE -fsanitize=address,undefined)
endforeach()
add_test(NAME groundhog-probe COMMAND test-groundhog-probe)
nostrc_describe_test(groundhog-probe OWNER app TARGETS test-groundhog-probe INPUTS app/build.py app/lsan.supp)
nostrc_finalize_test_inventory()
''')
    workflow = '''jobs:
  groundhog-sanitizers:
    runs-on: ubuntu-24.04
    env:
      GROUNDHOG_SANITIZER_TESTS: groundhog-probe
    steps:
      - name: Configure
        env:
          ADAPTER_PHASE: configure
        run: cmake -S . -B out -G Ninja -DCMAKE_BUILD_TYPE=Debug
      - name: Build
        env:
          ADAPTER_PHASE: build
        run: |
          targets=(groundhog)
          for test in $GROUNDHOG_SANITIZER_TESTS; do
            case "$test" in groundhog-*) targets+=("test-$test") ;; *) targets+=("$test") ;; esac
          done
          cmake --build out --parallel 1 --target "${targets[@]}"
      - name: Run
        env:
          ADAPTER_PHASE: test
          ADAPTER_TEST_ONLY: only-test
          ASAN_OPTIONS: detect_leaks=LEAKS:halt_on_error=1
          UBSAN_OPTIONS: print_stacktrace=1:halt_on_error=1
        run: |
          export LSAN_OPTIONS="suppressions=${PWD}/app/lsan.supp:print_suppressions=1"
          read -ra tests <<< "$GROUNDHOG_SANITIZER_TESTS"
          regex="^($(IFS='|'; echo "${tests[*]}"))\\$"
          ctest --test-dir out -R "$regex" --parallel 1 | tee out/ctest.log
          if grep -Eq '\\*\\*\\*Skipped|Not Run' out/ctest.log; then exit 1; fi
'''.replace("LEAKS", "1" if host == "linux" else "0")
    write(ADAPTER.WORKFLOW, workflow)
    config = ADAPTER.job_config(workflow, "{source_root}")
    profile = dict(platform=host, architecture="native", compiler="clang" if host == "macos" else "gcc",
        build_type="Debug", instrumentation="asan_ubsan", source_dir=".", configure_args=config["GH_SAN_CONFIGURE"],
        environment={}, phase_environments={phase: dict(item.split("=", 1) for item in config[field])
            for phase, field in (("configure", "GH_SAN_CONFIGURE_ENV"), ("build", "GH_SAN_BUILD_ENV"), ("test", "GH_SAN_ENV"))},
        sanitizer_compatibility=config, capabilities=[], parallelism=1, timeout_seconds=30,
        local_availability="host_or_container", setup="miniature")
    policy = dict(schema_version=1,
        nodes={"app": dict(owner_paths=["app"], depends_on=[], rationale="fixture")},
        path_rules=[dict(prefix="app/", nodes=["app"], effect="impact", reason="fixture")]
            + [dict(prefix=p, nodes=[], effect="global", reason="contract")
               for p in ("scripts/", "tests/", "cmake/", ".github/", "CMakeLists.txt", ".gitignore")],
        profiles={"mini.sanitizer": profile},
        checks={"mini.actual": dict(kind="ctest", nodes=["app"], profiles=["mini.sanitizer"],
            cost="ordinary", requirement="blocking", expected_outcome="pass",
            ctest_tests={"mini.sanitizer": ["groundhog-probe"]})},
        test_overrides=[], release_components={})
    write("tests/selection/inventory.json", json.dumps(policy))
    for args in (("init", "-q"), ("config", "user.name", "Adapter Fixture"),
                 ("config", "user.email", "adapter@example.invalid"), ("add", "-A"), ("commit", "-qm", "fixture")):
        subprocess.run(["git", "-C", str(repo), *args], check=True, capture_output=True)
    return policy


class SharedSanitizerExecutionTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="nostrc-shared-adapter-")
        self.addCleanup(self.tmp.cleanup)
        self.work = Path(self.tmp.name)
        self.repo = self.work / "repo"
        self.policy = miniature(self.repo)
        self.locks = self.work / "locks"
        self.locks.mkdir()
        self.number = 0

    def invoke(self, success=True, **extra_env):
        self.number += 1
        evidence = self.work / ("evidence-" + str(self.number))
        env = dict(os.environ, PYTHONDONTWRITEBYTECODE="1", **extra_env)
        result = subprocess.run([sys.executable, str(ROOT / "scripts/sanitizer-gate-ci.py"), "shared-run",
            "--source-root", str(self.repo), "--shared-profile", "mini.sanitizer",
            "--build-dir", str(self.work / ("build-" + str(self.number))), "--evidence-dir", str(evidence),
            "--lock-dir", str(self.locks)], capture_output=True, text=True, env=env, timeout=60)
        logs = "\n".join(str(p) + ": " + p.read_text(errors="replace")[-3500:] for p in evidence.rglob("*.log"))
        self.assertEqual(result.returncode == 0, success, result.stdout + result.stderr + logs)
        return result, evidence

    def test_real_phase_build_audit_single_ctest_and_receipt(self):
        result, evidence = self.invoke()
        self.assertIn("single attempt", result.stdout)
        receipt = json.loads((evidence / "receipt.json").read_text())
        raw = receipt["backend_receipt"]
        self.assertEqual(raw["observed_executions"], {"groundhog-probe": "passed"})
        self.assertTrue(raw["input_audit"]["after_build"])
        self.assertTrue((self.work / "build-1/groundhog").is_file())
        self.assertTrue(any(path.endswith("app/application.c") for path in raw["input_audit"]["input_hashes"]))
        self.assertEqual(sum(c[0] == "ctest" for c in raw["commands"]), 1)
        self.assertTrue(raw["phase_context_hash"])
        self.assertEqual((self.work / "build-1/configure.phase").read_text(), "configure")
        self.assertEqual((self.work / "build-1/build.phase").read_text(), "actual build phase\n")
        build = json.loads((evidence / "build.json").read_text())
        self.assertFalse(build["execution_complete"])
        resolved = json.loads((evidence / "resolved.json").read_text())
        self.assertTrue(set(self.policy["profiles"]["mini.sanitizer"]["sanitizer_compatibility"]["GH_SAN_TARGETS"]).issubset(resolved["build_targets"]))
        self.assertTrue(json.loads((evidence / "aggregate.json").read_text())["passed"])

    def test_missing_phase_api_has_no_unconfigured_ninja_fallback(self):
        core = self.repo / "scripts/test_selection_realization.py"
        text = core.read_text()
        self.assertIn("def phase_environment(", text)
        core.write_text(text.replace("def phase_environment(", "def unsupported_phase_environment("))
        subprocess.run(["git", "-C", str(self.repo), "add", str(core)], check=True)
        subprocess.run(["git", "-C", str(self.repo), "commit", "-qm", "missing API"], check=True)
        result, evidence = self.invoke(success=False)
        self.assertIn("phase-aware core API is unsupported", result.stderr)
        self.assertFalse((evidence / "resolved.json").exists())
        self.assertFalse((self.work / "build-1/build.ninja").exists())

    def test_dirty_source_fails_before_build_or_test_without_retry(self):
        (self.repo / "app/main.c").write_text("#error caller source changed\n")
        _, evidence = self.invoke(success=False)
        self.assertFalse((evidence / "build.json").exists())
        self.assertFalse((evidence / "receipt.json").exists())

    def test_quarantine_and_retry_overrides_cannot_silently_waive_core_failure(self):
        for extra in ({"NOSTRC_CTEST_QUARANTINE_PLATFORM": "linux"}, {"RERUN_MAX": "2"}):
            with self.subTest(extra=extra):
                result, evidence = self.invoke(success=False, **extra)
                self.assertIn("does not support quarantine/retry", result.stderr)
                self.assertFalse((evidence / "resolved.json").exists())

    def test_first_failure_is_not_retried_into_success(self):
        main = self.repo / "app/main.c"
        main.write_text(main.read_text().replace('puts("real miniature sanitized test");', '''
  FILE *file = fopen("attempt.marker", "r");
  if (file) { fclose(file); puts("would pass a retry"); return 0; }
  file = fopen("attempt.marker", "w");
  if (!file) return 9;
  fputs("one execution", file); fclose(file);
  puts("first execution fails"); return 7;
'''))
        subprocess.run(["git", "-C", str(self.repo), "add", "app/main.c"], check=True)
        subprocess.run(["git", "-C", str(self.repo), "commit", "-qm", "first failure"], check=True)
        _, evidence = self.invoke(success=False)
        raw = json.loads((evidence / "receipt.json").read_text())["backend_receipt"]
        self.assertEqual(raw["observed_executions"], {"groundhog-probe": "failed"})
        self.assertEqual(sum(c[0] == "ctest" for c in raw["commands"]), 1)
        self.assertFalse((evidence / "aggregate.json").exists())


if __name__ == "__main__":
    unittest.main()
