#!/usr/bin/env python3
"""Opt-in C<->Rust FIPS link over real onion services (nostrc-gfi8h.4).

Follows upstream docs/how-to/deploy-tor-onion.md on loopback: two local tor
daemons (one per FIPS daemon, as on two hosts), each publishing a v3 onion
service through HiddenServiceDir/HiddenServicePort and exposing SocksPort and
a cookie-authenticated ControlPort. fipsd and the pinned Rust fips both run
transports.tor in directory mode with the control-port monitor. The link is
established over the onions in both directions: C dials Rust's onion, then
Rust dials C's. Waits are event-driven: tor's own STATUS_CLIENT BOOTSTRAP and
HS_DESC UPLOADED control events, and the daemons' log lines (drained by
reader threads). Timings are printed as JSON. SKIP (77) without a tor binary
(--tor / FIPSD_TOR_BINARY) or the Rust daemon (--rust /
FIPSD_UDP_INTEROP_RUST_BINARY). No build or checkout mutation; every tor data
directory lives under FIPSD_TEST_TMPDIR.
"""
import argparse
import json
import os
from pathlib import Path
import selectors
import signal
import socket
import subprocess
import sys
import threading
import time

sys.dont_write_bytecode = True  # never leave __pycache__ in the source tree
sys.path.insert(0, str(Path(__file__).resolve().parent))
from udp_interop import POLL, ControlError, NotYet, control  # noqa: E402 - deadline-bounded wait helpers

BOOTSTRAP_SECS = 300
DESCRIPTOR_SECS = 300
LINK_SECS = 420


class LogTap:
    """Drains a child's stdout on a thread; waiters block on a condition."""

    def __init__(self, name, proc):
        self.name, self.proc, self.data = name, proc, bytearray()
        self.cond = threading.Condition()
        self.done = False
        self.thread = threading.Thread(target=self._run, daemon=True)
        self.thread.start()

    def _run(self):
        while True:
            chunk = os.read(self.proc.stdout.fileno(), 65536)
            with self.cond:
                if not chunk:
                    self.done = True
                    self.cond.notify_all()
                    return
                self.data.extend(chunk)
                self.cond.notify_all()

    def wait(self, needle, seconds, start=0):
        deadline = time.monotonic() + seconds
        needle = needle.lower().encode()
        with self.cond:
            while True:
                at = self.data.lower().find(needle, start)
                if at >= 0:
                    return at + len(needle)
                remaining = deadline - time.monotonic()
                if self.done or remaining <= 0:
                    raise AssertionError(f'{self.name}: no "{needle.decode()}" within {seconds}s '
                                         f'(exited={self.done}); log tail:\n{self.tail()}')
                self.cond.wait(remaining)

    def mark(self):
        with self.cond:
            return len(self.data)

    def tail(self, n=4000):
        return bytes(self.data[-n:]).decode(errors='replace')


class TorControl:
    """Minimal control-spec client: cookie AUTHENTICATE, GETINFO, async 650s."""

    def __init__(self, port, password):
        self.sock = socket.create_connection(('127.0.0.1', port), timeout=30)
        self.sock.setblocking(False)
        self.sel = selectors.DefaultSelector()
        self.sel.register(self.sock, selectors.EVENT_READ)
        self.buf, self.events = b'', []
        reply = self.command(f'AUTHENTICATE "{password}"')
        assert reply[-1].startswith('250'), reply

    def _line(self, deadline):
        while b'\r\n' not in self.buf:
            remaining = deadline - time.monotonic()
            if remaining <= 0 or not self.sel.select(remaining):
                return None
            chunk = self.sock.recv(65536)
            assert chunk, 'tor control connection closed'
            self.buf += chunk
        line, self.buf = self.buf.split(b'\r\n', 1)
        return line.decode(errors='replace')

    def command(self, text):
        self.sock.sendall(text.encode() + b'\r\n')
        lines, deadline = [], time.monotonic() + 30
        while True:
            line = self._line(deadline)
            assert line is not None, f'no reply to {text.split()[0]}'
            if line.startswith('650'):
                self.events.append(line)
                continue
            lines.append(line)
            if len(line) > 3 and line[3] == ' ':
                return lines

    def wait_event(self, predicate, seconds):
        deadline = time.monotonic() + seconds
        while True:
            for i, line in enumerate(self.events):
                if predicate(line):
                    del self.events[:i + 1]
                    return line
            self.events.clear()
            line = self._line(deadline)
            if line is None:
                return None
            if line.startswith('650'):
                self.events.append(line)

    def close(self):
        self.sel.close()
        self.sock.close()


def free_port():
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.bind(('127.0.0.1', 0))
        return s.getsockname()[1]


def query(path, command, timeout=10):
    return control(path, command, timeout=timeout)


def observe(what, seconds, predicate, tap):
    """udp_interop's Scenario.wait shape: one absolute deadline bounds every
    control round trip (`predicate(timeout)`) and each pause between them; a
    pause ends early on new output from `tap`'s daemon. A Rust control view is
    republished per tick, so a reply alone is no reason to ask again."""
    deadline, last = time.monotonic() + seconds, None
    while True:
        remaining = deadline - time.monotonic()
        if tap.done or remaining <= 0:
            raise AssertionError(f'timed out waiting for {what} (exited={tap.done}); last observation: {last}; '
                                 f'{tap.name} log tail:\n{tap.tail()}')
        try:
            return predicate(min(10, max(0.001, remaining)))
        except NotYet as e:
            last = e.args[0] if e.args else None
        except (ControlError, OSError) as e:
            last = str(e)
        with tap.cond:
            tap.cond.wait(min(POLL, max(0, deadline - time.monotonic())))


def peer_listed(path, npub):
    """An observe() predicate: `npub` is in this daemon's show_peers."""
    def listed(timeout):
        peers = query(path, 'show_peers', timeout)['peers']
        if not any(p.get('npub') == npub for p in peers):
            raise NotYet(f'show_peers npubs: {[p.get("npub") for p in peers]}')
        return peers
    return listed


def start(cmd, name, root, procs):
    env = dict(os.environ, FIPSD_WORKERS='2', TOKIO_WORKER_THREADS='2', NO_COLOR='1')
    proc = subprocess.Popen(cmd, cwd=root, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    tap = LogTap(name, proc)
    procs.append((name, proc, tap))
    return tap


def stop(entry, failures):
    name, proc, tap = entry
    if proc.poll() is None:
        proc.send_signal(signal.SIGTERM)
    try:
        code = proc.wait(timeout=30)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait()
        failures.append(f'{name} did not exit on SIGTERM')
        code = proc.returncode
    tap.thread.join(timeout=5)
    proc.stdout.close()
    if code not in (0, -signal.SIGTERM):
        failures.append(f'{name} exited {code}')


def launch_tor(tor, root, name, listen_port, procs):
    d = root / name
    d.mkdir(mode=0o700)
    socks, ctl = free_port(), free_port()
    # The script authenticates with a password (tor writes its cookie file
    # after opening the listener); fipsd's monitor uses the cookie, which
    # exists long before fipsd starts.
    password = os.urandom(12).hex()
    hashed = subprocess.run([tor, '--quiet', '--hash-password', password], check=True,
                            capture_output=True, text=True).stdout.strip().splitlines()[-1]
    torrc = d / 'torrc'
    torrc.write_text(f'''DataDirectory {d}/data
SocksPort 127.0.0.1:{socks}
ControlPort 127.0.0.1:{ctl}
HashedControlPassword {hashed}
CookieAuthentication 1
CookieAuthFile {d}/control.authcookie
HiddenServiceDir {d}/hs
HiddenServicePort 443 127.0.0.1:{listen_port}
Log notice stdout
''')
    tap = start([tor, '-f', str(torrc)], name, root, procs)
    tap.wait('opened control listener', 60)
    control = TorControl(ctl, password)
    assert control.command('SETEVENTS STATUS_CLIENT HS_DESC')[-1].startswith('250')
    return {'dir': d, 'socks': socks, 'ctl': ctl, 'listen': listen_port, 'control': control, 'tap': tap,
            'cookie': d / 'control.authcookie'}


def bootstrapped(t, started, timings, name):
    phase = t['control'].command('GETINFO status/bootstrap-phase')
    if not any('PROGRESS=100' in line for line in phase):
        line = t['control'].wait_event(lambda l: 'BOOTSTRAP PROGRESS=100' in l, BOOTSTRAP_SECS)
        if line is None:
            phase = t['control'].command('GETINFO status/bootstrap-phase')
            raise AssertionError(f'{name}: tor not bootstrapped within {BOOTSTRAP_SECS}s: {phase}')
    timings[f'{name}_bootstrap_s'] = round(time.monotonic() - started, 1)
    onion = (t['dir'] / 'hs' / 'hostname').read_text().strip()
    t['onion'] = onion
    line = t['control'].wait_event(
        lambda l: l.startswith('650 HS_DESC UPLOADED ' + onion[:-len('.onion')] + ' '), DESCRIPTOR_SECS)
    if line is None:
        raise AssertionError(f'{name}: no HS_DESC UPLOADED for {onion} within {DESCRIPTOR_SECS}s')
    timings[f'{name}_descriptor_uploaded_s'] = round(time.monotonic() - started, 1)


def tor_section(t):
    return f'''  tor:
    mode: directory
    socks5_addr: '127.0.0.1:{t['socks']}'
    control_addr: '127.0.0.1:{t['ctl']}'
    control_auth: 'cookie:{t['cookie']}'
    directory_service: {{hostname_file: '{t['dir'] / 'hs' / 'hostname'}', bind_addr: '127.0.0.1:{t['listen']}'}}
'''


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--fipsd', required=True)
    ap.add_argument('--rust', default='')
    ap.add_argument('--tor', default='')
    ap.add_argument('--c-log-level', default='info')
    args = ap.parse_args()
    args.rust = args.rust or os.environ.get('FIPSD_UDP_INTEROP_RUST_BINARY', '')
    args.tor = args.tor or os.environ.get('FIPSD_TOR_BINARY', '')
    if not args.tor or not Path(args.tor).is_file() or not args.rust or not Path(args.rust).is_file():
        print('SKIP: set FIPSD_TOR_BINARY to a tor daemon and FIPSD_UDP_INTEROP_RUST_BINARY to a pinned Rust fips')
        return 77
    root = Path(os.environ['FIPSD_TEST_TMPDIR'])
    procs, failures, timings, rc = [], [], {}, 1
    began = time.monotonic()
    try:
        c_tor = launch_tor(args.tor, root, 'tor-c', free_port(), procs)
        r_tor = launch_tor(args.tor, root, 'tor-rust', free_port(), procs)
        bootstrapped(c_tor, began, timings, 'tor_c')
        bootstrapped(r_tor, began, timings, 'tor_rust')
        print(f'onions: C {c_tor["onion"]}, Rust {r_tor["onion"]}')
        c_sock, r_sock = root / 'c.sock', root / 'r.sock'

        def rust_config(peer=None):
            peers = f"peers:\n  - npub: '{peer[0]}'\n    addresses: [{{transport: tor, addr: '{peer[1]}:443'}}]\n" if peer else ''
            path = root / 'rust.yaml'
            path.write_text(f'''node:
  identity: {{nsec: '{'2' * 64}', persistent: false}}
  control: {{enabled: true, socket_path: '{r_sock}'}}
  native_api: {{enabled: false}}
tun: {{enabled: false}}
dns: {{enabled: false}}
transports:
{tor_section(r_tor)}{peers}''')
            return path

        def c_config(peer=None):
            peers = f"peers:\n  - npub: '{peer[0]}'\n    addresses: [{{transport: tor, addr: '{peer[1]}:443'}}]\n" if peer else ''
            path = root / 'c.yaml'
            path.write_text(f'''node:
  identity: {{nsec: '{'1' * 64}', persistent: false}}
  control: {{enabled: true, socket_path: '{c_sock}'}}
  log_level: {args.c_log_level}
transports:
{tor_section(c_tor)}{peers}''')
            return path

        # Direction 1: fipsd dials the Rust daemon's onion.
        rust = start([args.rust, '-c', str(rust_config())], 'rust', root, procs)
        rust.wait('control socket listening', 60)
        rust_npub = query(r_sock, 'show_status')['npub']
        t0 = time.monotonic()
        c = start([args.fipsd, '-c', str(c_config((rust_npub, r_tor['onion'])))], 'fipsd', root, procs)
        c.wait('link established with', LINK_SECS)
        timings['c_to_rust_link_s'] = round(time.monotonic() - t0, 1)
        # The responder logs its promotion at debug only; its control view is
        # tick-published, so observe its snapshots under one deadline.
        c_npub = query(c_sock, 'show_status')['npub']
        observe('Rust show_peers lists the C peer', 30, peer_listed(r_sock, c_npub), rust)
        row = next(t for t in query(c_sock, 'show_transports')['transports'] if t['type'] == 'tor')
        assert row['tor_mode'] == 'directory' and row['onion_address'] == c_tor['onion'], row
        assert row['tor_monitoring']['bootstrap'] == 100, row
        assert row['stats']['connections_established'] >= 1 and row['stats']['pool_outbound'] >= 1, row
        print('C show_transports tor row:', json.dumps(row, sort_keys=True))
        timings['c_dial_socks5_errors'] = row['stats']['socks5_errors']
        timings['c_dial_connect_timeouts'] = row['stats']['connect_timeouts']
        for entry in [p for p in procs if p[0] in ('fipsd', 'rust')]:
            stop(entry, failures)
        procs[:] = [p for p in procs if p[0] not in ('fipsd', 'rust')]
        assert not failures, '; '.join(failures)
        # Direction 2: the Rust daemon dials fipsd's onion.
        c = start([args.fipsd, '-c', str(c_config())], 'fipsd', root, procs)
        c.wait('control socket listening', 60)
        t0 = time.monotonic()
        rust = start([args.rust, '-c', str(rust_config((c_npub, c_tor['onion'])))], 'rust', root, procs)
        rust.wait('peer promoted to active', LINK_SECS)
        c.wait('link established with', 60)
        timings['rust_to_c_link_s'] = round(time.monotonic() - t0, 1)
        row = next(t for t in query(c_sock, 'show_transports')['transports'] if t['type'] == 'tor')
        assert row['stats']['connections_accepted'] >= 1 and row['stats']['pool_inbound'] >= 1, row
        rrow = next(t for t in query(r_sock, 'show_transports')['transports'] if t['type'] == 'tor')
        timings['rust_dial_socks5_errors'] = rrow['stats']['socks5_errors']
        timings['rust_dial_connect_timeouts'] = rrow['stats']['connect_timeouts']
        timings['total_s'] = round(time.monotonic() - began, 1)
        print('timings:', json.dumps(timings, sort_keys=True))
        rc = 0
    except Exception as e:  # noqa: BLE001 - report what was observed, then fail
        print('FAIL:', e)
        print('observed timings:', json.dumps(timings, sort_keys=True))
        for name, _, tap in procs:
            print(f'--- {name} log tail ---\n{tap.tail(2500)}')
    finally:
        for entry in reversed(procs):
            if entry[0].startswith('tor-'):
                continue
            stop(entry, failures)
        for t in [x for x in (locals().get('c_tor'), locals().get('r_tor')) if x]:
            t['control'].close()
        for entry in reversed([p for p in procs if p[0].startswith('tor-')]):
            stop(entry, failures)
    if failures:
        print('teardown:', '; '.join(failures))
    return 1 if failures else rc


if __name__ == '__main__':
    raise SystemExit(main())
