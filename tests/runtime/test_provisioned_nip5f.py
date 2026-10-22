#!/usr/bin/env python3
"""Real, Linux-only NIP-5F TCP provision/approval diagnostics (no selector admission).

Run inside a network-disabled Linux environment with PyGObject/libsecret/GIO,
C compiler, dbus-daemon, gdbus, and an explicitly TCP-enabled root build:
  python3 tests/runtime/test_provisioned_nip5f.py --source-root . --build BUILD \
    --evidence-dir NEW_DIR --approve-unidentified-get-public-key
No tests are skipped. Missing prerequisites refuse before provisioning. The
private Secret Service is the existing Groundhog test service, not an ambient
keyring; StoreKey exercises the real libsecret backend through the public API.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import queue
import resource
import shlex
import shutil
import socket
import stat
import subprocess
import sys
import tempfile
import threading
import time
import unittest

ARGS = None


def identity(path):
    path = Path(path).resolve(strict=True)
    return {"path": str(path), "sha256": hashlib.sha256(path.read_bytes()).hexdigest()}


class Process:
    def __init__(self, argv, env, log):
        self.log = open(log, "wb")
        self.lines = queue.Queue()
        self.process = subprocess.Popen(argv, env=env, stdin=subprocess.DEVNULL,
                                        stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        def drain():
            for line in self.process.stdout:
                self.log.write(line); self.log.flush()
                self.lines.put(line.decode("utf-8", "replace").strip())
            self.lines.put(None)
        self.thread = threading.Thread(target=drain)
        self.thread.start()

    def until(self, prefix, timeout=30):
        end = time.monotonic() + timeout
        while True:
            line = self.lines.get(timeout=max(0, end - time.monotonic()))
            if line is None:
                raise RuntimeError("process exited before readiness: " + prefix)
            if line.startswith(prefix) or prefix in line:
                return line

    def stop(self):
        running = self.process.poll() is None
        if running:
            self.process.terminate()
        forced = False
        try:
            self.process.wait(timeout=10)
        except subprocess.TimeoutExpired:
            forced = True
            self.process.kill(); self.process.wait(timeout=10)
        self.thread.join(timeout=10)
        if self.thread.is_alive():
            raise RuntimeError("owned process output did not drain")
        self.process.stdout.close()
        self.log.close()
        return {"reaped": self.process.poll() is not None, "output_drained": True,
                "terminated_by_cleanup": running,
                "forced_kill": forced, "exit_code": self.process.returncode}


class Signer:
    def __init__(self, name, decision="allow"):
        self.out = ARGS.evidence_dir / name
        self.out.mkdir(mode=0o700)
        self.private = Path(tempfile.mkdtemp(prefix="nip5f-private-", dir=self.out))
        self.processes = []
        self.finished = False
        self.result = {"decision": decision, "cleanup": {}, "complete": False,
                       "complete_gate_eligible": False,
                       "scope": "owned fixture processes and private namespace only"}
        self.env = {k: v for k, v in os.environ.items()
                    if not k.startswith(("NOSTR_", "GNOSTR_", "DBUS_", "GSETTINGS_", "XDG_"))}
        for key, directory in (("HOME", "home"), ("XDG_CONFIG_HOME", "config"),
                               ("XDG_DATA_HOME", "data"), ("XDG_CACHE_HOME", "cache"),
                               ("XDG_RUNTIME_DIR", "runtime")):
            p = self.private / directory; p.mkdir(mode=0o700); self.env[key] = str(p)
        self.env.update(XPC_SERVICE_NAME="0", PYTHONDONTWRITEBYTECODE="1", GSETTINGS_BACKEND="memory",
                        NOSTR_SIGNER_TEST_APPROVERS=str(ARGS.approver),
                        GSETTINGS_SCHEMA_DIR=str(ARGS.schemas))
        # Reserve a candidate loopback port. A subsequent bind conflict is a
        # retained setup failure, never a blind retry or another signer's token.
        with socket.socket() as reservation:
            reservation.bind(("127.0.0.1", 0)); self.port = reservation.getsockname()[1]
        self.env["NOSTR_SIGNER_ENDPOINT"] = f"tcp:127.0.0.1:{self.port}"
        self.decision = decision

    def start_process(self, name, argv):
        p = Process(argv, self.env, self.out / (name + ".log"))
        self.processes.append((name, p)); return p

    def start(self):
        address = "unix:path=" + str(self.private / "bus")
        config = self.private / "bus.conf"
        config.write_text('<busconfig><type>session</type><listen>' + address +
                          '</listen><auth>EXTERNAL</auth><policy context="default">'
                          '<allow send_destination="*"/><allow receive_sender="*"/>'
                          '<allow own="*"/></policy></busconfig>')
        self.env["DBUS_SESSION_BUS_ADDRESS"] = address
        bus = self.start_process("bus", ["dbus-daemon", "--nofork", "--config-file=" + str(config), "--print-address=1"])
        bus.until(address)
        self.start_process("secrets", [sys.executable, str(ARGS.source_root / "gnome/groundhog/tests/app/test_secret_service.py"), str(self.private / "secrets.json")])
        subprocess.run(["gdbus", "wait", "--session", "--timeout", "30", "org.freedesktop.secrets"], env=self.env, check=True, timeout=35)
        self.daemon = self.start_process("daemon", [str(ARGS.daemon)])
        self.daemon.until("tcp ipc server started on 127.0.0.1:")
        subprocess.run(["gdbus", "wait", "--session", "--timeout", "30", "org.nostr.Signer"], env=self.env, check=True, timeout=35)
        token = Path(self.env["XDG_RUNTIME_DIR"]) / "gnostr/token"
        if token.is_symlink() or not stat.S_ISREG(token.stat().st_mode) or stat.S_IMODE(token.stat().st_mode) != 0o600:
            raise RuntimeError("unsafe runtime token")
        self.token = token.read_text()
        if len(self.token) != 64 or any(c not in "0123456789abcdef" for c in self.token):
            raise RuntimeError("malformed runtime token")
        self.env["NOSTR_SIGNER_TOKEN"] = self.token
        key = self.private / "key"
        with key.open("x") as f:
            os.chmod(key, 0o600); f.write("0" * 63 + "1")
        self.approver = self.start_process("approver", [str(ARGS.approver), "--approve-unidentified-get-public-key", self.decision, str(key)])
        ready = self.approver.until("ready:")
        key.unlink()
        if ready != "ready:npub10xlxvlhemja6c4dqv22uapctqupfhlxm9h8z3k2e72q4k9hcz7vqpkge6d":
            raise RuntimeError("stored identity differs from the public fixture vector")
        self.result.update(endpoint=self.env["NOSTR_SIGNER_ENDPOINT"], identity=ready.split("ready:", 1)[1], token_mode="0600", stored_via="StoreKey/libsecret", approval_api="GetApprovalInfo/ApproveRequest", remember=False)
        # This must be real persisted Secret Service custody, not an env key.
        if not (self.private / "secrets.json").is_file():
            raise RuntimeError("StoreKey did not persist to the isolated Secret Service")
        return self

    def client(self, token=None):
        env = dict(self.env)
        if token is not None: env["NOSTR_SIGNER_TOKEN"] = token
        p = Process([str(ARGS.client)], env, self.out / "client.log")
        self.processes.append(("client", p)); return p

    def snapshot(self):
        if self.approver.process.poll() is not None:
            raise RuntimeError("approver exited before health snapshot")
        reply = subprocess.run(["gdbus", "call", "--session", "--dest", "org.nostr.Grotto",
                                "--object-path", "/org/nostr/test/nip5f_approver", "--method",
                                "org.nostr.Test.Nip5fApprover1.Snapshot"],
                               env=self.env, check=True, capture_output=True, text=True, timeout=15)
        if self.approver.process.poll() is not None:
            raise RuntimeError("approver exited during health snapshot")
        return reply.stdout.strip()

    def finish(self):
        if self.finished:
            return
        self.finished = True
        errors = []
        for name, p in reversed(self.processes):
            try:
                stopped = p.stop()
                self.result["cleanup"][name] = stopped
                if name != "client":
                    if not stopped["terminated_by_cleanup"]:
                        errors.append(name + " exited before fixture cleanup")
                    if stopped["forced_kill"] or stopped["exit_code"] not in (0, -15):
                        errors.append(name + " did not complete expected fixture shutdown")
                    if name == "daemon" and stopped["exit_code"] != 0:
                        errors.append("daemon did not complete graceful fixture shutdown")
            except Exception as exc: errors.append(f"{name}: {type(exc).__name__}")
        shutil.rmtree(self.private)
        self.result["private_namespace_absent"] = not self.private.exists()
        self.result["cleanup_errors"] = errors
        (self.out / "receipt.json").write_text(json.dumps(self.result, indent=2) + "\n")
        if errors: raise RuntimeError("fixture cleanup failed: " + ", ".join(errors))


class ProvisionedTests(unittest.TestCase):
    def fixture(self, name, decision="allow"):
        f = Signer(name, decision); self.addCleanup(f.finish); return f.start()

    def test_real_tcp_explicit_approval(self):
        f = self.fixture("allow")
        p = f.client()
        self.assertEqual(f.approver.until("pending:"), "pending:unidentified:NIP5F:get_public_key:nonrememberable")
        self.assertEqual(f.approver.until("decision:"), "decision:allow:remember=false")
        self.assertEqual(p.process.wait(timeout=15), 0)
        self.assertEqual(f.snapshot(), "(uint32 1, uint32 1, uint32 0)")
        f.result["test_nip5f_tcp_exit"] = 0

    def test_explicit_denial_is_not_success(self):
        f = self.fixture("deny", "deny"); p = f.client()
        f.approver.until("pending:"); f.approver.until("decision:deny:")
        code = p.process.wait(timeout=15)
        self.assertEqual(code, 1); f.result["denied_client_exit"] = code
        self.assertEqual(f.snapshot(), "(uint32 1, uint32 1, uint32 0)")

    def test_wrong_token_cannot_reach_approval(self):
        f = self.fixture("bad-token"); p = f.client("f" * 64 if f.token != "f" * 64 else "e" * 64)
        code = p.process.wait(timeout=15)
        self.assertEqual(code, 1); f.result["wrong_token_client_exit"] = code
        f.daemon.until("tcp: rejected unauthorized connection")
        self.assertEqual(f.snapshot(), "(uint32 0, uint32 0, uint32 0)")
        f.result["healthy_approver_requests_decisions_pending"] = [0, 0, 0]

    def test_unexpected_approver_exit_is_cleanup_failure(self):
        f = self.fixture("dead-approver")
        f.approver.process.kill(); f.approver.process.wait(timeout=10)
        f.result["expected_cleanup_refusal"] = "approver exited before fixture cleanup"
        with self.assertRaisesRegex(RuntimeError, "approver exited before fixture cleanup"):
            f.finish()

    def test_untrusted_management_caller_is_refused(self):
        f = self.fixture("untrusted")
        r = subprocess.run(["gdbus", "call", "--session", "--dest", "org.nostr.Signer", "--object-path", "/org/nostr/signer", "--method", "org.nostr.Signer.ListPendingRequests"], env=f.env, capture_output=True, timeout=15)
        self.assertNotEqual(r.returncode, 0)
        self.assertIn(b"org.nostr.Signer.Error.PermissionDenied", r.stderr)
        f.result["untrusted_approver_refused"] = True

    def test_pending_client_cancel_preserves_sibling(self):
        sibling = self.fixture("sibling")
        owned = self.fixture("cancel", "hold"); p = owned.client()
        self.assertNotEqual(owned.token, sibling.token)
        self.assertNotEqual(owned.env["DBUS_SESSION_BUS_ADDRESS"], sibling.env["DBUS_SESSION_BUS_ADDRESS"])
        owned.approver.until("pending:")
        self.assertIsNone(p.process.poll())
        self.assertEqual(owned.snapshot(), "(uint32 1, uint32 0, uint32 1)")
        p.process.terminate(); self.assertNotEqual(p.process.wait(timeout=15), 0)
        owned.result["cancelled_while_pending_without_approval"] = True
        owned.finish()
        self.assertIsNone(sibling.daemon.process.poll())
        live = sibling.client(); sibling.approver.until("decision:allow:")
        self.assertEqual(live.process.wait(timeout=15), 0)
        sibling.result["sibling_survived_owned_cancel"] = True


def main():
    global ARGS
    parser = argparse.ArgumentParser(description=__doc__)
    for flag in ("source-root", "build", "evidence-dir"):
        parser.add_argument("--" + flag, type=Path, required=True)
    parser.add_argument("--approve-unidentified-get-public-key", action="store_true", required=True)
    ARGS = parser.parse_args()
    if platform.system() != "Linux": parser.error("Linux-only fixture: no developer keychain access")
    ARGS.source_root = ARGS.source_root.resolve(strict=True); ARGS.build = ARGS.build.resolve(strict=True)
    cache = (ARGS.build / "CMakeCache.txt").read_text()
    for expected in ("ENABLE_TCP_IPC:BOOL=ON", "NIP55L_SECRET_BACKEND:STRING=libsecret", "NIP55L_TEST_TRUST_ENV:BOOL=ON"):
        if expected not in cache: parser.error("required explicit build contract absent: " + expected)
    if "CMAKE_HOME_DIRECTORY:INTERNAL=" + str(ARGS.source_root) + "\n" not in cache:
        parser.error("build source root mismatch")
    ARGS.daemon = ARGS.build / "apps/grotto/grotto-daemon"
    ARGS.client = ARGS.build / "nips/nip5f/test_nip5f_tcp"
    for tool in ("cc", "pkg-config", "dbus-daemon", "gdbus", "glib-compile-schemas"):
        if not shutil.which(tool): parser.error("missing required tool: " + tool)
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    os.umask(0o077); ARGS.evidence_dir = ARGS.evidence_dir.resolve(); ARGS.evidence_dir.mkdir(mode=0o700)
    ARGS.schemas = ARGS.evidence_dir / "schemas"
    ARGS.schemas.mkdir(mode=0o700)
    subprocess.run(["glib-compile-schemas", "--strict", "--targetdir=" + str(ARGS.schemas),
                    str(ARGS.source_root / "apps/grotto/data")], check=True)
    ARGS.approver = ARGS.evidence_dir / "nip5f-approver"
    source = ARGS.source_root / "tests/runtime/nip5f_approver.c"
    flags = shlex.split(subprocess.check_output(["pkg-config", "--cflags", "--libs", "gio-2.0"], text=True))
    subprocess.run(["cc", "-Wall", "-Wextra", "-Werror", str(source), "-o", str(ARGS.approver), *flags], check=True)
    before = {k: identity(p) for k, p in (("daemon", ARGS.daemon), ("client", ARGS.client), ("approver", ARGS.approver), ("approver_source", source), ("runner", __file__), ("cache", ARGS.build / "CMakeCache.txt"), ("schema_source", ARGS.source_root / "apps/grotto/data/org.nostr.Grotto.gschema.xml"), ("schema_compiled", ARGS.schemas / "gschemas.compiled"), ("secret_service_fixture", ARGS.source_root / "gnome/groundhog/tests/app/test_secret_service.py"))}
    result = unittest.TextTestRunner(verbosity=2).run(unittest.defaultTestLoader.loadTestsFromTestCase(ProvisionedTests))
    unchanged = all(identity(v["path"]) == v for v in before.values())
    receipt = {"inputs": before, "platform": platform.platform(), "machine": platform.machine(), "tests_run": result.testsRun, "failures": len(result.failures), "errors": len(result.errors), "skipped": len(result.skipped), "inputs_unchanged": unchanged, "complete": False, "complete_gate_eligible": False, "native_arbitrary_tree_custody": False}
    (ARGS.evidence_dir / "summary.json").write_text(json.dumps(receipt, indent=2) + "\n")
    return 0 if result.wasSuccessful() and unchanged and not result.skipped else 1


if __name__ == "__main__":
    sys.exit(main())
