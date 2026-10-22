#!/usr/bin/env python3
"""Real CLI cache-admission negatives against a retained successful fetch receipt.

Positive original build/test/hello is executed separately by the cache builder.
These cases launch the real CLI (including real daemon/image identity reads for
wrong-label admission), never mock Docker or synthesize successful image proof.
"""
import argparse
import copy
import hashlib
import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import unittest

ARGS = None


class CacheAdmissionTests(unittest.TestCase):
    def setUp(self):
        self.out = ARGS.evidence_dir / self._testMethodName
        self.out.mkdir()
        self.receipt = copy.deepcopy(ARGS.receipt)

    def context_copy(self):
        import shutil
        original = Path(self.receipt["image_results"][0]["context"])
        target = self.out / "producer"
        shutil.copytree(original, target)
        self.receipt["image_results"][0]["context"] = str(target)
        return target

    def refuse(self, message):
        receipt = self.out / "candidate.json"
        receipt.write_text(json.dumps(self.receipt))
        evidence = self.out / "must-not-be-created"
        argv = [sys.executable, str(ARGS.source_root / "scripts/mdk011-cache-build.py"),
                "--source-root", str(ARGS.source_root), "--fetch-receipt", str(receipt),
                "--evidence-dir", str(evidence)]
        result = subprocess.run(argv, capture_output=True, timeout=60)
        (self.out / "stdout").write_bytes(result.stdout)
        (self.out / "stderr").write_bytes(result.stderr)
        (self.out / "result.json").write_text(json.dumps({"argv": argv, "exit_code": result.returncode,
            "evidence_directory_absent": not evidence.exists()}, indent=2) + "\n")
        self.assertEqual(result.returncode, 1)
        self.assertIn(message.encode(), result.stderr)
        self.assertFalse(evidence.exists(), "must refuse before cache export CID or build resources")

    def test_incomplete_receipt(self):
        self.receipt["cleanup_complete"] = False
        self.refuse("not a successful current guarded image build")

    def test_stale_helper(self):
        self.receipt["helper"]["sha256"] = "0" * 64
        self.refuse("not a successful current guarded image build")

    def test_changed_pins(self):
        context = self.context_copy()
        with (context / "Cargo.lock").open("a") as f: f.write("\n")
        self.refuse("Cargo pins differ")

    def test_unreviewed_recipe(self):
        context = self.context_copy()
        with (context / "Dockerfile").open("a") as f: f.write("\nRUN true\n")
        self.receipt["image_results"][0]["digest"] = ARGS.gate.digest(context)
        self.refuse("unreviewed Cargo fetch producer recipe")

    def test_actual_image_wrong_run_label(self):
        self.receipt["run_id"] = "0" * 32
        self.refuse("cache image immutable identity/labels/volume contract changed")


def main():
    global ARGS
    parser = argparse.ArgumentParser(description=__doc__)
    for flag in ("source-root", "fetch-receipt", "evidence-dir"):
        parser.add_argument("--" + flag, required=True, type=Path)
    ARGS = parser.parse_args()
    ARGS.source_root = ARGS.source_root.resolve(strict=True)
    ARGS.receipt = json.loads(ARGS.fetch_receipt.read_text())
    ARGS.evidence_dir = ARGS.evidence_dir.resolve(); ARGS.evidence_dir.mkdir(mode=0o700)
    spec = importlib.util.spec_from_file_location("mdk_gate", ARGS.source_root / "scripts/mdk-gate.py")
    ARGS.gate = importlib.util.module_from_spec(spec); spec.loader.exec_module(ARGS.gate)
    inputs = {str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in
              (Path(__file__), ARGS.source_root / "scripts/mdk011-cache-build.py", ARGS.fetch_receipt)}
    result = unittest.TextTestRunner(verbosity=2).run(unittest.defaultTestLoader.loadTestsFromTestCase(CacheAdmissionTests))
    unchanged = all(hashlib.sha256(Path(p).read_bytes()).hexdigest() == value for p, value in inputs.items())
    (ARGS.evidence_dir / "summary.json").write_text(json.dumps({"inputs": inputs,
        "inputs_unchanged": unchanged, "tests_run": result.testsRun, "failures": len(result.failures),
        "errors": len(result.errors), "skipped": len(result.skipped), "complete": False}, indent=2) + "\n")
    return 0 if result.wasSuccessful() and unchanged and not result.skipped else 1


if __name__ == "__main__":
    sys.exit(main())
