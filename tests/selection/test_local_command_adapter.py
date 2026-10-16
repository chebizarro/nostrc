"""Real outer adapter -> shared CLI command/build-contract execution, not fake receipts."""
import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
import signal
import socket
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class LocalCommandAdapterTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="nostrc-command-adapter-")
        self.addCleanup(self.tmp.cleanup)
        self.work = Path(self.tmp.name)
        self.repo = self.work / "repo"
        self.repo.mkdir()
        self.git("init", "-q")
        self.git("config", "user.name", "Adapter fixture")
        self.git("config", "user.email", "adapter@example.invalid")
        for name in ("test-gate.sh", "test-plan.py", "test_selection.py", "test_selection_realization.py",
                     "check-macos-keychain.sh"):
            target = self.repo / "scripts" / name
            target.parent.mkdir(exist_ok=True)
            shutil.copy2(ROOT / "scripts" / name, target)
        self.write(".gitignore", "__pycache__/\n")
        self.write("app/vector.txt", "committed\n")
        self.write("app/check.py", """import os, pathlib, sys
assert pathlib.Path.cwd() == pathlib.Path(sys.argv[1])
assert os.environ['COMMAND_POLICY'] == 'bound'
content = pathlib.Path('app/vector.txt').read_text()
assert content in ('committed\\n', 'snapshot\\n')
pathlib.Path(sys.argv[2]).write_text(content)
print('actual command: ' + content, end='', flush=True)
""")
        host = {"Darwin": "macos", "Linux": "linux"}[platform.system()]
        self.policy = dict(schema_version=1,
            nodes={"app": dict(owner_paths=["app"], depends_on=[], rationale="fixture")},
            path_rules=[dict(prefix="app/", nodes=["app"], effect="impact", reason="fixture")]
                + [dict(prefix=p, nodes=[], effect="global", reason="contract")
                   for p in ("scripts/", "tests/", "cmake/", "CMakeLists.txt", ".gitignore")],
            test_overrides=[], release_components={},
            profiles={"local.command": dict(platform=host, architecture="native", compiler="unavailable-unused-compiler",
                build_type="Debug", instrumentation="none", source_dir=".", configure_args=[],
                environment={}, capabilities=[], parallelism=1, timeout_seconds=30,
                setup="fixture", local_availability="host")},
            checks={"command.actual": dict(kind="command", profiles=["local.command"], nodes=["app"],
                cost="ordinary", requirement="blocking", expected_outcome="pass",
                command=["python3", "{source}/app/check.py", "{source}", "{build}/proof.txt"],
                working_directory="{source}", environment={"COMMAND_POLICY": "bound"},
                runtime_inputs=["{source}/app/check.py", "{source}/app/vector.txt"],
                build_targets=[], required_evidence=["proof.txt"])})
        self.save_policy()
        self.head = self.commit()

    def write(self, path, content):
        target = self.repo / path
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(content)

    def git(self, *args):
        return subprocess.check_output(["git", "-C", str(self.repo), *args], stderr=subprocess.PIPE).decode().strip()

    def save_policy(self):
        self.write("tests/selection/inventory.json", json.dumps(self.policy))

    def commit(self):
        self.git("add", "-A")
        self.git("commit", "-qm", "fixture")
        return self.git("rev-parse", "HEAD")

    def gate(self, *args, success=True):
        evidence = self.work / ("evidence-" + str(len(list(self.work.glob("evidence-*")))))
        result = subprocess.run(["bash", str(self.repo / "scripts/test-gate.sh"), "--repo", str(self.repo),
            "--evidence-dir", str(evidence), "--broad", *args], capture_output=True, text=True,
            env=dict(os.environ, PYTHONDONTWRITEBYTECODE="1"), timeout=60)
        receipts = list(evidence.glob("*/adapter-receipt.json"))
        self.assertEqual(len(receipts), 1, result.stdout + result.stderr)
        receipt = json.loads(receipts[0].read_text())
        logs = "\n".join(str(p) + ": " + p.read_text(errors="replace")[-3000:]
                         for p in evidence.rglob("*.log"))
        self.assertEqual(result.returncode == 0, success, result.stdout + result.stderr + logs)
        return receipt, receipts[0].parent

    def backend(self, receipt):
        lane = receipt["executions"][0]["lanes"][0]
        self.assertEqual(lane["status"], "passed", lane)
        return json.loads((Path(lane["evidence"]) / "receipt.json").read_text())["backend_receipt"]

    def test_exact_sha_and_dirty_snapshot_have_real_bound_evidence(self):
        self.write("app/vector.txt", "caller dirt must not execute\n")
        receipt, _ = self.gate("--head", self.head, "--base", self.head)
        backend = self.backend(receipt)
        self.assertEqual(backend["source_identity"], {"kind": "committed", "sha": self.head})
        record = backend["checks"]["command.actual"]
        self.assertEqual(record["exit_code"], 0)
        self.assertEqual(record["artifacts"]["proof.txt"]["sha256"], hashlib.sha256(b"committed\n").hexdigest())
        self.assertIn("actual command: committed", Path(record["log"]).read_text())
        self.assertEqual(len(backend["commands"]), 1)
        self.assertNotIn("ctest", backend["commands"][0])
        self.write("app/vector.txt", "staged\n")
        self.git("add", "app/vector.txt")
        self.write("app/vector.txt", "snapshot\n")
        index = Path(self.git("rev-parse", "--git-path", "index"))
        if not index.is_absolute():
            index = self.repo / index
        before = index.read_bytes()
        receipt, directory = self.gate("--base", self.head)
        backend = self.backend(receipt)
        self.assertEqual(backend["source_identity"]["kind"], "dirty")
        self.assertEqual(backend["checks"]["command.actual"]["artifacts"]["proof.txt"]["sha256"], hashlib.sha256(b"snapshot\n").hexdigest())
        self.assertEqual(index.read_bytes(), before)
        self.assertEqual((self.repo / "app/vector.txt").read_text(), "snapshot\n")
        aggregate = json.loads((directory / "0/aggregate.json").read_text())
        self.assertTrue(aggregate["passed"])
        self.assertFalse(receipt["complete"])

    def test_source_only_does_not_provision_unrelated_submodules(self):
        self.git("update-index", "--add", "--cacheinfo", "160000," + self.head + ",third_party/nsync")
        self.git("commit", "-qm", "unrelated uninitialized gitlink")
        head = self.git("rev-parse", "HEAD")
        receipt, directory = self.gate("--head", head, "--base", self.head)
        self.assertEqual(self.backend(receipt)["outcome"], "passed")
        self.assertFalse(any(directory.rglob("submodules.log")))

    def test_source_command_phase_maps_remain_materially_unsupported(self):
        # Core source-only realization deliberately cannot attest any authored
        # phase map, even a test-only one. Do not drop it to manufacture support.
        self.policy["profiles"]["local.command"]["phase_environments"] = {
            "configure": {}, "build": {}, "test": {"ADAPTER_TEST_ONLY": "test"}}
        self.save_policy()
        head = self.commit()
        _, directory = self.gate("--head", head, "--base", self.head, success=False)
        logs = "\n".join(p.read_text() for p in directory.rglob("resolve.log"))
        self.assertIn("source-only commands cannot attest configure/build phase overlays", logs)

    def test_missing_runtime_input_or_evidence_and_nonzero_never_pass(self):
        for case in ("input", "evidence", "nonzero"):
            with self.subTest(case=case):
                if case == "input":
                    self.policy["checks"]["command.actual"]["runtime_inputs"].append("{source}/app/missing.txt")
                elif case == "evidence":
                    self.policy["checks"]["command.actual"]["runtime_inputs"].pop()
                    self.policy["checks"]["command.actual"]["required_evidence"] = ["not-written.txt"]
                else:
                    self.write("app/check.py", "import os, sys\nos.write(1, b'raw \\xff error\\n')\nsys.exit(7)\n")
                self.save_policy()
                head = self.commit()
                receipt, directory = self.gate("--head", head, "--base", self.head, success=False)
                self.assertEqual(receipt["status"], "failed")
                self.assertNotEqual(receipt["executions"][0]["lanes"][0]["status"], "passed")
                if case == "nonzero":
                    logs = [p.read_bytes() for p in directory.rglob("command-*.log")]
                    self.assertTrue(any(b"raw \xff error" in log for log in logs), logs)

    def test_dangerous_or_unprovisioned_capabilities_are_not_bypassed(self):
        for capability in ("docker", "external_peer", "developer_keychain", "unknown"):
            with self.subTest(capability=capability):
                self.policy["profiles"]["local.command"]["capabilities"] = [capability]
                self.save_policy()
                head = self.commit()
                receipt, _ = self.gate("--head", head, "--base", self.head, success=False)
                self.assertIn("unimplemented guarded adapter", receipt["executions"][0]["lanes"][0]["error"])

    def test_native_build_contract_uses_real_configured_targets_and_audit(self):
        self.policy["profiles"]["local.command"].update(compiler="clang" if platform.system() == "Darwin" else "gcc",
            configure_args=["-G", "Ninja", "-DCMAKE_BUILD_TYPE=Debug"])
        if platform.system() == "Darwin":
            self.policy["profiles"]["local.command"]["configure_args"] += [
                "-DNIP55L_SECRET_BACKEND=libsecret", "-DGROUNDHOG_STORE_KEY_DEFAULT_BACKEND=secret-service"]
        self.policy["checks"]["command.actual"].update(kind="build_contract",
            command=["cmake", "--build", "{build}", "--target", "probe"], environment={},
            runtime_inputs=[], required_evidence=[])
        target = self.repo / "cmake/NostrcTestInventory.cmake"
        target.parent.mkdir(exist_ok=True)
        shutil.copy2(ROOT / "cmake/NostrcTestInventory.cmake", target)
        self.write("app/main.c", 'int main(void) { return 0; }\n')
        self.write("CMakeLists.txt", """cmake_minimum_required(VERSION 3.22)
project(adapter_contract C)
enable_testing()
include(cmake/NostrcTestInventory.cmake)
add_executable(probe app/main.c)
nostrc_finalize_test_inventory()
""")
        self.save_policy()
        head = self.commit()
        receipt, _ = self.gate("--head", head, "--base", self.head)
        backend = self.backend(receipt)
        self.assertTrue(backend["input_audit"]["after_build"])
        self.assertTrue(any(path.endswith("app/main.c") for path in backend["input_audit"]["input_hashes"]))
        self.assertEqual(backend["checks"]["command.actual"]["exit_code"], 0)

    def test_actual_command_cancellation_keeps_evidence_and_stops_owned_child(self):
        self.write("app/check.py", """import json, os, signal, socket
os.write(1, b'command ready \\xff\\n')
peer = socket.socket(socket.AF_UNIX)
peer.connect(os.environ['ADAPTER_READY_SOCKET'])
peer.sendall(json.dumps(os.getpid()).encode())
peer.close()
signal.pause()
""")
        head = self.commit()
        server = socket.socket(socket.AF_UNIX)
        self.addCleanup(server.close)
        # macOS Unix-domain path limit is 104 bytes.
        endpoint = '/tmp/nostrc-command-' + str(os.getpid()) + '.sock'
        server.bind(endpoint)
        self.addCleanup(os.unlink, endpoint)
        server.listen(1)
        server.settimeout(30)
        evidence = self.work / "cancel-evidence"
        process = subprocess.Popen(["bash", str(self.repo / "scripts/test-gate.sh"), "--repo", str(self.repo),
            "--head", head, "--broad", "--evidence-dir", str(evidence)],
            env=dict(os.environ, ADAPTER_READY_SOCKET=endpoint, PYTHONDONTWRITEBYTECODE="1"),
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        pid = None
        try:
            peer, _ = server.accept()
            with peer:
                pid = json.loads(peer.recv(4096))
            process.terminate()
            output = process.communicate(timeout=20)[0]
            self.assertEqual(process.returncode, 130, output)
            receipt = json.loads(next(evidence.glob("*/adapter-receipt.json")).read_text())
            self.assertEqual(receipt["status"], "cancelled")
            state = subprocess.run(["ps", "-o", "stat=", "-p", str(pid)], capture_output=True, text=True).stdout.strip()
            self.assertTrue(not state or state.startswith("Z"), state)
            self.assertTrue(any(b"command ready \xff\n" in p.read_bytes() for p in evidence.rglob("command-*.log")))
            self.assertEqual(self.git("worktree", "list", "--porcelain").count("worktree "), 1)
        finally:
            if process.poll() is None:
                process.terminate()
                process.communicate(timeout=20)
            if pid:
                try:
                    os.kill(pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass


if __name__ == "__main__":
    unittest.main()
