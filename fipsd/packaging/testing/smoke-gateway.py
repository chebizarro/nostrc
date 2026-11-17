#!/usr/bin/env python3
"""Root-only installed gateway smoke; --direct needs NET_ADMIN and /dev/net/tun.

Uses the deployment example with loopback as the LAN, an isolated pool/TUN and
high DNS ports. No mesh peer is needed to prove startup and control readiness.
Stops both processes and restores the config and sysctls, including on failure.
Run only on disposable packaging runners, not an operator's running gateway.
"""
import argparse
import os
from contextlib import ExitStack
from pathlib import Path
import subprocess
import sys
import tempfile

HERE = Path(__file__).resolve().parent
POOL = "fd01:ffff::/112"
SOCKET = Path("/run/fips/gateway.sock")
CONFIG = Path("/etc/fips/fips.yaml")
SYSCTLS = ("net.ipv6.conf.all.forwarding", "net.ipv6.conf.all.proxy_ndp")


def run(*args, check=True):
    return subprocess.run(args, check=check, text=True, stdout=subprocess.PIPE,
                          stderr=subprocess.STDOUT, timeout=40)


def query(command, socket):
    result = run(sys.executable, str(HERE / "control-query.py"),
                 "--socket", str(socket), "--retry", "30", command)
    print(result.stdout, flush=True)


def assert_clean():
    assert not SOCKET.exists(), "gateway control socket survived stop"
    result = run("nft", "list", "table", "inet", "fips_gateway", check=False)
    assert result.returncode != 0 and "No such file" in result.stdout, result.stdout
    routes = run("ip", "-6", "route", "show", "table", "local").stdout
    assert POOL not in routes, routes
    print("gateway cleanup: nft table, pool route and control socket absent", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--direct", action="store_true")
    args = parser.parse_args()
    assert os.geteuid() == 0, "run as root on a disposable packaging runner"
    assert_clean()  # Never take over an existing gateway's network resources.
    old_config = CONFIG.read_bytes()
    old_sysctls = {key: run("sysctl", "-n", key).stdout.strip() for key in SYSCTLS}
    children = []
    with ExitStack() as restore, tempfile.TemporaryDirectory(prefix="fips-gateway-smoke-") as tmp:
        restore.callback(CONFIG.write_bytes, old_config)
        log = open(Path(tmp) / "process.log", "w+")
        try:
            if not args.direct:
                run("systemctl", "stop", "fipsd.service")
                assert run("systemctl", "is-enabled", "fips-gateway.service",
                           check=False).stdout.strip() == "disabled", "gateway must be opt-in"
            for key, value in old_sysctls.items():
                if value != "1":
                    run("sysctl", "-w", f"{key}=1")
                    restore.callback(run, "sysctl", "-w", f"{key}={value}")
            # Same required gateway block as docs/fips-gateway.md; only the
            # smoke's interface, pool and DNS ports differ from deployment.
            CONFIG.write_text(f'''node:
  identity:
    persistent: false
  control:
    socket_path: /run/fips/control.sock
tun:
  enabled: true
  name: fipsgwpkg0
dns:
  enabled: true
  bind_addr: "::1"
  port: 55354
transports:
  udp:
    bind_addr: "127.0.0.1:0"
peers: []
gateway:
  enabled: true
  pool: "{POOL}"
  lan_interface: "lo"
  dns:
    listen: "[::1]:55365"
    upstream: "[::1]:55354"
''')
            if args.direct:
                Path("/run/fips").mkdir(exist_ok=True)
                children.append(subprocess.Popen(["fipsd", "-c", str(CONFIG)],
                                                 stdout=log, stderr=log))
            else:
                run("systemctl", "start", "fipsd.service")
            query("show_status", "/run/fips/control.sock")
            if args.direct:
                children.append(subprocess.Popen(["fips-gateway", "-c", str(CONFIG)],
                                                 stdout=log, stderr=log))
            else:
                run("systemctl", "start", "fips-gateway.service")
            query("show_gateway", SOCKET)
            query("show_mappings", SOCKET)
            print(run("nft", "list", "table", "inet", "fips_gateway").stdout)
            routes = run("ip", "-6", "route", "show", "table", "local").stdout
            assert POOL in routes, routes
            if not args.direct:
                run("systemctl", "is-active", "fips-gateway.service")
                run("systemctl", "stop", "fips-gateway.service")
                assert run("systemctl", "show", "-p", "Result", "--value",
                           "fips-gateway.service").stdout.strip() == "success"
            else:
                gateway = children[-1]
                gateway.terminate()
                assert gateway.wait(timeout=20) == 0, "gateway did not stop cleanly"
                children.pop()
            assert_clean()
            print("smoke-gateway: start, control query and clean stop passed", flush=True)
        finally:
            # Stop gateway before daemon: the daemon owns /run/fips.
            if not args.direct:
                print(run("journalctl", "-u", "fips-gateway.service", "--no-pager",
                          "-n", "40", check=False).stdout)
                run("systemctl", "stop", "fips-gateway.service", "fipsd.service")
            failures = []
            for child in reversed(children):
                child.terminate()
                try:
                    code = child.wait(timeout=20)
                    if code != 0:
                        failures.append(f"process exited {code}")
                except subprocess.TimeoutExpired:
                    child.kill()
                    child.wait(timeout=5)
                    failures.append("process did not stop within 20 seconds")
            log.seek(0)
            print(log.read())
            log.close()
            assert not failures, failures


if __name__ == "__main__":
    main()
