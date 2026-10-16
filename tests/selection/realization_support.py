"""Small real CMake/CTest fixture shared only by realization tests."""
import hashlib
import importlib.util
import os
from pathlib import Path
import shutil
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("realization", ROOT / "scripts/test_selection_realization.py")
r = importlib.util.module_from_spec(spec)
spec.loader.exec_module(r)


class Project(unittest.TestCase):
    def setUp(self):
        if not shutil.which("cmake") or not shutil.which("ninja"):
            self.skipTest("synthetic native configure needs CMake and Ninja")
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name) / "source"
        self.root.mkdir()
        self.root = self.root.resolve()
        self.build = (Path(self.temp.name) / "build").resolve()
        self.evidence = Path(self.temp.name) / "evidence"
        (self.root / "cmake").mkdir()
        shutil.copy2(ROOT / "cmake/NostrcTestInventory.cmake", self.root / "cmake")
        (self.root / "main.c").write_text('''#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "shared.h"
int main(int argc, char **argv) {
  if (argc > 1 && !strcmp(argv[1], "--gui") && !getenv("TEST_DISPLAY")) return 77;
  FILE *f = fopen("vectors.jsonl", "a"); if (!f) return 1;
  fprintf(f, "evidence-%d\\n", SHARED); fclose(f); return 0;
}
''')
        (self.root / "shared.h").write_text("#define SHARED 42\n")
        (self.root / "vector.txt").write_text("test vector\n")
        (self.root / "setup.cmake").write_text('file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/vectors.jsonl" "")\n')
        (self.root / "noop.cmake").write_text('message(STATUS "support")\n')
        (self.root / "CMakeLists.txt").write_text('''cmake_minimum_required(VERSION 3.22)
project(selection_fixture C)
enable_testing()
include(cmake/NostrcTestInventory.cmake)
add_executable(shared main.c)
add_test(NAME z-first COMMAND ${CMAKE_COMMAND} -P ${CMAKE_CURRENT_SOURCE_DIR}/noop.cmake)
nostrc_describe_test(z-first OWNER testing INPUTS noop.cmake EXPECTED fixture)
add_test(NAME reset COMMAND ${CMAKE_COMMAND} -P ${CMAKE_CURRENT_SOURCE_DIR}/setup.cmake)
set_tests_properties(reset PROPERTIES FIXTURES_SETUP private)
nostrc_describe_test(reset OWNER testing INPUTS setup.cmake EXPECTED fixture)
add_test(NAME second-setup COMMAND ${CMAKE_COMMAND} -P ${CMAKE_CURRENT_SOURCE_DIR}/noop.cmake)
set_tests_properties(second-setup PROPERTIES FIXTURES_SETUP private)
nostrc_describe_test(second-setup OWNER testing INPUTS noop.cmake EXPECTED fixture)
add_test(NAME cleanup COMMAND ${CMAKE_COMMAND} -P ${CMAKE_CURRENT_SOURCE_DIR}/noop.cmake)
set_tests_properties(cleanup PROPERTIES FIXTURES_CLEANUP private)
nostrc_describe_test(cleanup OWNER testing INPUTS noop.cmake EXPECTED fixture)
add_test(NAME a-model COMMAND shared)
add_test(NAME gui COMMAND shared --gui)
add_test(NAME omitted COMMAND shared)
foreach(t a-model gui omitted)
  set_tests_properties(${t} PROPERTIES FIXTURES_REQUIRED private SKIP_RETURN_CODE 77)
  nostrc_describe_test(${t} OWNER application TARGETS shared INPUTS vector.txt)
endforeach()
set_tests_properties(a-model PROPERTIES DEPENDS z-first)
nostrc_finalize_test_inventory()
''')
        self.profile = {"source_dir": ".", "configure_args": ["-G", "Ninja"],
                        "environment": {}, "capabilities": ["display"]}
        r._command(["git", "init", "-q", str(self.root)])
        self.git("config", "user.name", "Realization fixture")
        self.git("config", "user.email", "fixture@example.invalid")
        self.git("add", ".")
        self.git("-c", "core.hooksPath=/dev/null", "commit", "-qm", "fixture base")
        self.refresh_identity()
        self.inventory = {"nodes": {"application": {"depends_on": ["core", "vectors"]},
                                    "core": {}, "vectors": {}, "testing": {}, "other": {}},
                          "path_rules": [
                              {"exact": "main.c", "nodes": ["application"]},
                              {"exact": "shared.h", "nodes": ["core"]},
                              {"exact": "vector.txt", "nodes": ["vectors"]},
                              {"exact": "setup.cmake", "nodes": ["testing"]},
                              {"exact": "noop.cmake", "nodes": ["testing"]},
                              {"exact": "CMakeLists.txt", "nodes": [], "effect": "global"},
                              {"prefix": "cmake/", "nodes": [], "effect": "global"}],
                          "checks": {n: {"kind": "ctest", "profiles": ["synthetic"],
                                          "ctest_tests": {"synthetic": [n]}, "nodes": ["application", "testing"]}
                                     for n in ("a-model", "gui", "omitted")}}
        self.plan = {"selected_checks": ["a-model"], "source_identity": self.identity,
                     "inventory_hash": "inventory-fixture"}

    def git(self, *args):
        return r._git(self.root, *args)

    def refresh_identity(self):
        self.git("add", "-A")
        tree = self.git("write-tree").decode().strip()
        digest = hashlib.sha256()
        for entry in self.git("ls-tree", "-rz", tree).split(b"\0"):
            if entry:
                meta, raw = entry.split(b"\t", 1)
                mode, kind, oid = meta.split()
                path = self.root / os.fsdecode(raw)
                content = oid if kind == b"commit" else os.fsencode(os.readlink(path)) if mode == b"120000" else path.read_bytes()
                digest.update(mode + b"\0" + raw + b"\0" + hashlib.sha256(content).digest())
        self.identity = {"kind": "dirty", "sha": self.git("rev-parse", "HEAD").decode().strip(),
                         "snapshot_digest": digest.hexdigest()}
        if hasattr(self, "plan"):
            self.plan["source_identity"] = self.identity

    def configure(self):
        self.refresh_identity()
        return r.configure_profile(self.root, self.build, "synthetic", self.profile,
                                   self.identity, "inventory-fixture")

    def build_all(self):
        r._command(["cmake", "--build", str(self.build)])

    def audit(self, realized):
        return r.check_inputs(self.inventory, realized, after_build=True)
