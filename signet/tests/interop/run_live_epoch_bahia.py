#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Opt-in disposable Signet/Bahia NIP-46 custody interop on a loopback relay.

Never accepts an existing identity, relay URL, or database. All keys are
synthetic and short-lived; no secret or bunker URI is written to stdout.
"""
import argparse
import datetime as dt
import json
import os
from pathlib import Path
import re
import secrets
import socket
import subprocess
import sys
import tempfile
import time


def run(argv, *, env=None, cwd=None):
    result = subprocess.run(argv, env=env, cwd=cwd, text=True, capture_output=True)
    if result.returncode:
        raise RuntimeError(f"{Path(argv[0]).name} failed (exit {result.returncode}); private output withheld")
    return result.stdout


def private_file(path, value):
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    with os.fdopen(fd, "w") as stream:
        stream.write(value)
        stream.flush()
        os.fsync(stream.fileno())


def pubkey(secret):
    value = run(["nak", "key", "public", secret]).strip()
    if len(value) != 64 or any(ch not in "0123456789abcdef" for ch in value):
        raise RuntimeError("nak did not return a hex public key")
    return value


def free_loopback_port():
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def wait_tcp(port, process, label):
    for _ in range(100):
        if process.poll() is not None:
            raise RuntimeError(f"{label} exited during startup; private log withheld")
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=0.2):
                return
        except OSError:
            time.sleep(0.1)
    raise RuntimeError(f"{label} did not listen on loopback")


def management(ctl, config, env, *args):
    output = run([str(ctl), "-c", str(config), *args], env=env)
    marker = "Reply received:\n"
    if marker not in output:
        raise RuntimeError("management acknowledgement missing")
    reply = json.loads(output.split(marker, 1)[1])
    if not isinstance(reply, dict) or not isinstance(reply.get("result"), dict):
        raise RuntimeError("management acknowledgement has no result")
    result = reply["result"]
    # Signet wraps command result under a second result object.
    if isinstance(result.get("result"), dict):
        result = result["result"]
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, required=True,
                        help="prebuilt nostrc CMake tree for this Signet worktree")
    parser.add_argument("--bahia", type=Path, required=True,
                        help="Bahia checkout containing the opt-in signetinterop test")
    args = parser.parse_args()
    repo = Path(__file__).resolve().parents[3]
    build = args.build_dir.resolve()
    bahia = args.bahia.resolve()
    if not (bahia / "internal/adapters/signet/epoch_interop_integration_test.go").is_file():
        raise RuntimeError("Bahia checkout lacks the opt-in Signet interop test")
    for executable in ("nak", "go", "cmake"):
        if subprocess.run(["which", executable], capture_output=True).returncode:
            raise RuntimeError(f"{executable} is required")
    daemon = build / "signet/signetd"
    ctl = build / "signet/signetctl"
    cache = build / "CMakeCache.txt"
    if not cache.is_file() or f"CMAKE_HOME_DIRECTORY:INTERNAL={repo}" not in cache.read_text():
        raise RuntimeError("build directory must be configured from this exact Signet worktree")
    run(["cmake", "--build", str(build), "--target", "signetd", "signetctl"])
    if not daemon.is_file() or not ctl.is_file():
        raise RuntimeError("Signet daemon or management CLI was not built")
    commit = run(["git", "-C", str(repo), "rev-parse", "HEAD"]).strip()
    if run(["git", "-C", str(repo), "status", "--porcelain"]).strip():
        raise RuntimeError("commit Signet source before interop so the fixture identifies the exact build")
    # No operator-provided keys/endpoints: this script cannot target production.
    with tempfile.TemporaryDirectory(prefix="signet-bahia-epoch-") as temp:
        root = Path(temp)
        os.chmod(root, 0o700)
        relay_port = free_loopback_port()
        relay_url = f"ws://127.0.0.1:{relay_port}"
        bunker_sk = secrets.token_hex(32)
        provisioner_sk = secrets.token_hex(32)
        owner_sk = secrets.token_hex(32)
        bunker_pk, provisioner_pk, owner_pk = map(pubkey,
                                                   (bunker_sk, provisioner_sk, owner_sk))
        if len({bunker_pk, provisioner_pk, owner_pk}) != 3:
            raise RuntimeError("test roles unexpectedly share a key")
        private_file(root / "bunker.key", bunker_sk + "\n")
        private_file(root / "provisioner.key", provisioner_sk + "\n")
        policy = root / "policies.toml"
        private_file(policy,
            '[identity.interop]\n'
            'allow_clients = "*"\n'
            'allow_methods = "connect, get_public_key, nip44_encrypt, nip44_decrypt, '
            'nip44_encrypt_b64, nip44_decrypt_b64, sign_bahia_sbom_dsse"\n'
            'allow_kinds = "*"\n'
            'default = "allow"\n')
        config = root / "signet.conf"
        private_file(config,
            '[server]\nlog_level = info\nhealth_port = 0\n'
            f'[store]\ndb_path = {root / "store.db"}\n'
            f'[nostr]\nrelays = {relay_url}\nidentity = epoch-interop\n'
            f'bunker_pubkey = {bunker_pk}\n'
            f'provisioner_pubkeys = {provisioner_pk}\n'
            f'[policy_defaults]\ndefault_decision = deny\npolicy_file = {policy}\n'
            f'[audit]\npath = {root / "audit.log"}\nstdout = false\n'
            '[bootstrap]\nport = 0\n'
            '[dbus]\nunix_enabled = false\ntcp_enabled = false\n'
            '[nip5l]\nenabled = false\n[ssh_agent]\nenabled = false\n')
        env = os.environ.copy()
        env.update(SIGNET_DB_KEY=secrets.token_hex(32),
                   SIGNET_BUNKER_NSEC_FILE=str(root / "bunker.key"),
                   SIGNET_PROVISIONER_NSEC_FILE=str(root / "provisioner.key"))
        processes = []
        try:
            with open(root / "relay.log", "w") as relay_log, open(root / "daemon.log", "w") as daemon_log:
                relay = subprocess.Popen(["nak", "serve", "--hostname", "127.0.0.1",
                                          "--port", str(relay_port)], stdout=relay_log,
                                         stderr=subprocess.STDOUT, env=env)
                processes.append(relay)
                wait_tcp(relay_port, relay, "private relay")
                signet = subprocess.Popen([str(daemon), "-c", str(config)],
                                          stdout=daemon_log, stderr=subprocess.STDOUT, env=env)
                processes.append(signet)
                # Health is an authenticated management roundtrip, not a sleep-based guess.
                for _ in range(30):
                    if signet.poll() is not None:
                        raise RuntimeError("signetd exited during startup; private log withheld")
                    try:
                        management(ctl, config, env, "status")
                        break
                    except RuntimeError:
                        time.sleep(0.2)
                else:
                    raise RuntimeError("signetd did not answer authenticated management status")
                provision = management(ctl, config, env, "provision", "interop")
                service_pk = provision.get("pubkey")
                bunker_uri = provision.get("bunker_uri")
                if not isinstance(service_pk, str) or len(service_pk) != 64 or not isinstance(bunker_uri, str):
                    raise RuntimeError("provision reply lacked public identity or pairing URI")
                if relay_url not in bunker_uri and "127.0.0.1" not in bunker_uri:
                    raise RuntimeError("pairing URI does not point at private loopback relay")
                lease = None
                for _ in range(2):
                    lease = management(ctl, config, env, "writer-acquire", "interop", owner_pk,
                                       "--ttl", "600")
                if not isinstance(lease, dict) or lease.get("epoch") != 2:
                    raise RuntimeError("second acquisition did not return epoch 2")
                expiry = dt.datetime.fromtimestamp(int(lease["expires_at"]), dt.timezone.utc)
                fixture = root / "bahia-fixture.json"
                private_file(fixture, json.dumps(dict(
                    disposable=True, signet_commit=commit, bunker_uri=bunker_uri,
                    owner_secret_key_hex=owner_sk, expected_service_pubkey=service_pk,
                    epoch=lease["epoch"], expires_at=expiry.isoformat().replace("+00:00", "Z"))) + "\n")
                test_env = env.copy()
                test_env["BAHIA_SIGNET_INTEROP_CONFIG"] = str(fixture)
                result = subprocess.run(["go", "test", "-tags", "signetinterop",
                    "./internal/adapters/signet", "-run", "^TestLiveSignetEpochNIP44AndSBOMDSSE$",
                    "-count=1", "-v"], cwd=bahia, env=test_env, text=True, capture_output=True)
                # The Go test may print errors containing its fixture URI. Do not echo raw logs.
                if result.returncode:
                    locations = re.findall(r"epoch_interop_integration_test\.go:(\d+)",
                                           result.stdout + result.stderr)
                    location = f" at test line {locations[0]}" if locations else ""
                    raise RuntimeError(f"Bahia live NIP-46 interop test failed{location}; private output withheld")
                print("PASS: Bahia live authenticated NIP-46 epoch NIP-44 and DSSE interop on disposable loopback Signet")
        finally:
            for process in reversed(processes):
                process.terminate()
            for process in reversed(processes):
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()


if __name__ == "__main__":
    try:
        main()
    except (OSError, RuntimeError, ValueError, json.JSONDecodeError) as exc:
        print(f"interop fixture: {exc}", file=sys.stderr)
        sys.exit(1)
