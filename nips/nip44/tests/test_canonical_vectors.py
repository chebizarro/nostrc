#!/usr/bin/env python3
"""Independent conformance/refusal regressions; run against an actual built binary.

python3 nips/nip44/tests/test_canonical_vectors.py --binary BUILD/nips/nip44/test_nip44_vectors
No inventory enrollment or historical-fixture projection is performed here.
"""

import argparse
import hashlib
import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[3]
WITNESS = json.loads((ROOT / "tests/selection/fixtures/nip44_canonical_vectors_20261022.json").read_text())
VECTORS = ROOT / WITNESS["input"]


class CanonicalVectors(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="nip44-canonical-", dir=ARGS.evidence_dir)
        self.work = Path(self.temp.name)
        self.sequence = 0

    def tearDown(self):
        if ARGS.evidence_dir:
            # Retain input bytes, configure logs, and refusals, not only passing summaries.
            shutil.copytree(self.work, Path(ARGS.evidence_dir) / self.id().split(".")[-1])
        self.temp.cleanup()

    def run_command(self, argv, **kwargs):
        result = subprocess.run([str(a) for a in argv], text=True, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, timeout=120, **kwargs)
        self.sequence += 1
        (self.work / f"process-{self.sequence}.json").write_text(json.dumps({
            "argv": [str(a) for a in argv], "exit_code": result.returncode,
            "output": result.stdout,
        }, indent=2) + "\n")
        return result

    def refuse(self, result, reason):
        self.assertNotEqual(result.returncode, 0, result.stdout)
        self.assertIn(reason, result.stdout)
        self.assertNotIn("(official json): OK", result.stdout)

    def run_input(self, data):
        path = self.work / "vectors.json"
        path.write_bytes(data)
        return self.run_command([ARGS.binary, path])

    def configure(self, mutation=None):
        source = self.work / "source"
        nip = source / "nips/nip44"
        shutil.copytree(ROOT / "nips/nip44", nip)
        if mutation:
            mutation(nip / "tests/nip44.vectors.json")
        (source / "CMakeLists.txt").write_text(
            'cmake_minimum_required(VERSION 3.22)\nproject(Nip44Fixture C)\n'
            'enable_testing()\n'
            f'include("{ROOT}/cmake/NostrcTestInventory.cmake")\n'
            'add_library(nostr INTERFACE)\nadd_subdirectory(nips/nip44)\n'
            'nostrc_finalize_test_inventory()\n')
        return self.run_command(["cmake", "-S", source, "-B", self.work / "build"])

    def test_pinned_input_and_all_section_counts(self):
        data = VECTORS.read_bytes()
        self.assertEqual(len(data), WITNESS["input_bytes"])
        self.assertEqual(hashlib.sha256(data).hexdigest(), WITNESS["input_sha256"])
        v2 = json.loads(data)["v2"]
        self.assertEqual(list(map(len, [
            v2["valid"]["get_conversation_key"], v2["valid"]["get_message_keys"]["keys"],
            v2["valid"]["calc_padded_len"], v2["valid"]["encrypt_decrypt"],
            v2["valid"]["encrypt_decrypt_long_msg"], v2["invalid"]["encrypt_msg_lengths"],
            v2["invalid"]["get_conversation_key"], v2["invalid"]["decrypt"],
        ])), list(WITNESS["executed_cases"].values()))
        self.assertEqual(sum(WITNESS["executed_cases"].values()), 128)

    def test_official_default_execution_is_observable(self):
        result = self.run_command([ARGS.binary])
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertIn("sha256=" + WITNESS["input_sha256"], result.stdout)
        for key, count in WITNESS["executed_cases"].items():
            self.assertIn(f"{key}={count}", result.stdout)
        self.assertIn("test_nip44_vectors (official json): OK", result.stdout)

    def test_identical_copy_is_still_official(self):
        result = self.run_input(VECTORS.read_bytes())
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertIn("(official json): OK", result.stdout)

    def test_missing_file_refuses(self):
        self.refuse(self.run_command([ARGS.binary, self.work / "missing.json"]), "unreadable")

    def test_unreadable_file_refuses(self):
        self.assertNotEqual(os.geteuid(), 0, "Run as nonroot to prove permission denial, not a skip")
        path = self.work / "denied.json"
        path.write_bytes(VECTORS.read_bytes())
        path.chmod(0)
        try:
            self.refuse(self.run_command([ARGS.binary, path]), "unreadable")
        finally:
            path.chmod(0o600)

    def test_directory_read_refuses(self):
        self.refuse(self.run_command([ARGS.binary, self.work]), "read failed")

    def test_malformed_file_refuses(self):
        self.refuse(self.run_input(b'{"v2":'), "malformed")

    def test_duplicate_keys_refuse(self):
        self.refuse(self.run_input(b'{"v2":{},"v2":{}}'), "malformed")

    def test_changed_valid_json_refuses(self):
        self.refuse(self.run_input(VECTORS.read_bytes() + b"\n"), "SHA256 mismatch")

    def test_empty_sections_cannot_claim_execution(self):
        self.refuse(self.run_input(b'{"v2":{"valid":{"encrypt_decrypt":[]}}}'), "SHA256 mismatch")

    def test_truncated_input_refuses(self):
        self.refuse(self.run_input(VECTORS.read_bytes()[:128]), "malformed")

    def test_builtin_only_request_cannot_succeed(self):
        self.refuse(self.run_command([ARGS.binary, "--builtin"]), "unreadable")
        self.refuse(self.run_command([ARGS.binary, VECTORS, "--builtin"]), "usage:")

    def test_compile_without_input_bindings_refuses(self):
        flags = shlex.split(subprocess.check_output(
            ["pkg-config", "--cflags", "openssl", "jansson"], text=True))
        for defines in ([], ['-DVECTORS_PATH="unused"']):
            result = self.run_command([os.environ.get("CC", "cc"), "-E", *flags,
                "-I" + str(ROOT / "nips/nip44/include"), *defines,
                ROOT / "nips/nip44/tests/test_nip44_vectors.c", "-o", os.devnull])
            self.refuse(result, "no builtin fallback")

    def test_canonical_descriptor_exports_exact_input(self):
        result = self.configure()
        self.assertEqual(result.returncode, 0, result.stdout)
        exports = list((self.work / "build").glob("nostrc-test-descriptors-*.json"))
        self.assertEqual(len(exports), 1)
        tests = json.loads(exports[0].read_text())["tests"]
        self.assertEqual(sorted(t["name"] for t in tests),
                         ["test_nip44_convkey", "test_nip44_roundtrip", "test_nip44_vectors"])
        vector = next(t for t in tests if t["name"] == "test_nip44_vectors")
        self.assertEqual(vector["owner_authority"], "descriptor")
        self.assertEqual(vector["owner"], ["nip44", "nip44.test_inputs"])
        self.assertEqual(vector["targets"], ["test_nip44_vectors"])
        self.assertEqual(vector["inputs"],
                         [str(self.work / "source/nips/nip44/tests/nip44.vectors.json")])
        self.assertEqual(hashlib.sha256(Path(vector["inputs"][0]).read_bytes()).hexdigest(),
                         WITNESS["input_sha256"])
        for smoke in tests:
            if smoke["name"] != vector["name"]:
                self.assertEqual(smoke["inputs"], [])

    def test_configure_missing_json_refuses(self):
        self.refuse(self.configure(lambda p: p.unlink()), "Official NIP-44 JSON vectors missing")

    def test_configure_changed_json_refuses(self):
        self.refuse(self.configure(lambda p: p.write_bytes(b"{}")), "SHA256 mismatch")

    def test_configure_unreadable_json_refuses(self):
        self.assertNotEqual(os.geteuid(), 0, "Run as nonroot to prove permission denial, not a skip")
        try:
            self.refuse(self.configure(lambda p: p.chmod(0)), "SHA256")
        finally:
            (self.work / "source/nips/nip44/tests/nip44.vectors.json").chmod(0o600)

    def test_configure_missing_jansson_refuses(self):
        real = shutil.which("pkg-config")
        self.assertIsNotNone(real)
        wrapper = self.work / "pkg-config"
        wrapper.write_text('#!/usr/bin/env python3\nimport os,sys\n'
                           'if any("jansson" in a for a in sys.argv[1:]):\n'
                           ' print("jansson deliberately unavailable", file=sys.stderr)\n'
                           ' sys.exit(1)\n'
                           f'os.execv({real!r}, [{real!r}] + sys.argv[1:])\n')
        wrapper.chmod(0o700)
        with unittest.mock.patch.dict(os.environ, {"PKG_CONFIG": str(wrapper)}):
            self.refuse(self.configure(), "required package")


if __name__ == "__main__":
    from unittest import mock  # Required only by the explicit executable suite.
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=lambda s: str(Path(s).resolve()), required=True)
    parser.add_argument("--evidence-dir", type=lambda s: str(Path(s).resolve()))
    ARGS, remaining = parser.parse_known_args()
    if ARGS.evidence_dir:
        Path(ARGS.evidence_dir).mkdir(parents=True, exist_ok=True)
    unittest.main(argv=[__file__, *remaining])
