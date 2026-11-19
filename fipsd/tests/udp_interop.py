#!/usr/bin/env python3
"""Two-node C<->Rust UDP interoperability gate (nostrc-f66sr.1).

One fipsd and one `fips` daemon built from the pinned upstream Rust source
(libfips-core/UPSTREAM.md) share loopback UDP, once per role assignment:
C initiates the IK link and Rust responds, then Rust initiates and C
responds. Every assertion is an observation through one of the two control
sockets or a daemon's own log, waited for under a deadline. No sleep stands
in for an event: the only pause is the bounded poll interval between control
snapshots (Rust publishes its off-loop control view once per tick).

Per role assignment the harness proves, in order:

1. IK link: `connect` on the initiator; both `show_links` and `show_peers`
   report the peer, npubs cross-match and directions are consistent.
2. Spanning tree and filters: both `show_tree` agree on the root, one side is
   root and the other hangs at depth 1 under it; both `show_bloom` hold the
   peer's filter.
3. XK session with the initiator role matching the link's: C-initiated via a
   C diagnostic probe (`probe_start`, whose session the Rust->C datagram
   below adopts, so neither side tears it down) when C initiated the link;
   Rust-initiated via a Rust native-API write when Rust initiated. Both
   `show_sessions` report it established with a proven identity and
   complementary `is_initiator` flags.
4. Datagrams: Rust->C application datagrams through the Rust native datagram
   API, receipt verified by C's `native` counters (datagram and byte counts
   equal to what was written) and by a C client over libfipsnative holding
   the port on fipsd's native API, which prints the bytes it accepted. C->Rust
   application datagrams: that C client answers each flow with a datagram the
   harness reads back off the Rust flow's descriptor, and Rust's
   `show_native_flows` `received_datagrams`/`received_bytes` confirm receipt
   (the direction the nostrc-f66sr.1 gate could not verify). Optionally
   (`--rust-echo`), upstream's own Rust client (the `native-echo` example
   built from the pinned tree) holds a port on fipsd's native API and echoes
   a datagram the Rust daemon's client sent through fipsd, closing the loop in
   the other direction. Session traffic both ways through a C probe
   (CoordsWarmup and MMP reports over the session; Rust's session
   `packets_recv` rises and C records RTT samples) and a Rust probe.
5. One rekey cycle each for the FSP session and the FMP link, driven by
   `node.rekey.after_secs` (25 s, so with the 15 s jitter the first session
   rekey falls 10-40 s after establishment; links apply the 45 s floor and the
   30 s link-age hold on both daemons): the K bit flips on both sides from
   the value first observed (`show_sessions` and `show_peers`
   `current_k_bit`), the session count stays one, then traffic flows under
   the new keys in both directions.
6. Clean shutdown: SIGTERM -> exit 0 for both, no sanitizer report in either
   log or in the C client's, both daemons' control sockets and both native
   API sockets unlinked.

Exit 0 on success, 77 (CTest SKIP) when no Rust daemon is supplied, 1 on
failure with both daemons' complete logs and the last observations printed.
"""
import argparse
import array
import atexit
import json
import os
from pathlib import Path
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import threading
import time
import traceback

POLL = 0.1
KILL_GRACE = 10  # seconds to reap a process group after SIGKILL
SANITIZER_MARKERS = (b'AddressSanitizer', b'LeakSanitizer', b'ThreadSanitizer',
                     b'UndefinedBehaviorSanitizer', b'runtime error:')
# Every daemon in these scenarios binds only these (reserve_port, Scenario.host),
# which is what lets quantized_echo() read a quantized RTT as a sub-ms path.
LOOPBACK = {4: '127.0.0.1', 6: '::1'}


def quantized_echo(rtt):
    """An RTT stage that saw authenticated MMP echoes but only sub-ms samples.

    libfips-core/UPSTREAM.md I-5: MMP timestamps are whole milliseconds, so a
    path well under 1 ms (loopback) yields samples of -1, 0, 1 or 2 ms. Upstream
    counts 0 as `zero_samples` (reason `sub_millisecond`) and -1 as an
    `arith_failures` that it ranks above it (reason `bad_timestamp_echo`);
    neither is a positive sample. Either reason qualifies only with reports seen,
    no positive sample and the counter that produced that reason. A positive
    sample, no report, no echo or any other reason is not this case.
    """
    if rtt.get('verdict') != 'failed' or rtt['reports_seen'] < 1 or rtt['samples'] != 0:
        return False
    if rtt.get('reason') == 'sub_millisecond':
        return rtt['zero_samples'] > 0 and rtt['arith_failures'] == 0
    return rtt.get('reason') == 'bad_timestamp_echo' and rtt['arith_failures'] > 0


class Failure(AssertionError):
    pass


class NotYet(Exception):
    """Raised by a wait predicate: the state is not there yet; `args[0]` says what was seen."""


class ControlError(Exception):
    pass


def control(path, command, params=None, timeout=10):
    with socket.socket(socket.AF_UNIX) as conn:
        conn.settimeout(timeout)
        conn.connect(str(path))
        request = {'command': command}
        if params is not None:
            request['params'] = params
        conn.sendall(json.dumps(request).encode() + b'\n')
        line = conn.makefile('rb').readline()
    if not line:
        raise ControlError(f'{command}: connection closed without a reply')
    response = json.loads(line)
    if response.get('status') != 'ok':
        raise ControlError(f'{command}: {response.get("message")}')
    return response['data']


class Daemon:
    """A child whose merged stdout/stderr is captured continuously.

    Each child leads its own session, so teardown signals its whole process
    group (a sanitizer symbolizer or helper it started goes with it), and its
    output never reaches the harness's own stdout, which is CTest's pipe.
    """

    live = set()  # started and not yet reaped: SIGKILLed by kill_children()

    def __init__(self, name, argv, env, cwd):
        self.name = name
        self.proc = subprocess.Popen(argv, cwd=cwd, env=env, stdin=subprocess.DEVNULL,
                                     stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                     start_new_session=True)
        Daemon.live.add(self)
        self.log = bytearray()
        self.cond = threading.Condition()
        self.eof = False
        self.returncode = None
        self.forced = False
        threading.Thread(target=self._pump, daemon=True).start()

    def _pump(self):
        while True:
            chunk = self.proc.stdout.read1(65536)
            with self.cond:
                if not chunk:
                    self.eof = True
                    self.cond.notify_all()
                    return
                self.log.extend(chunk)
                self.cond.notify_all()

    def await_log(self, needle, deadline):
        with self.cond:
            while needle.encode() not in self.log.lower():
                if self.eof:
                    raise Failure(f'{self.name} exited before logging {needle!r}')
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    raise Failure(f'{self.name} did not log {needle!r} in time')
                self.cond.wait(remaining)

    def alive(self):
        return self.proc.poll() is None

    def text(self):
        with self.cond:
            return self.log.decode(errors='replace')

    def sanitizer_report(self):
        with self.cond:
            return any(marker in self.log for marker in SANITIZER_MARKERS)

    def signal_group(self, sig):
        if not hasattr(os, 'killpg'):  # no process groups: the child alone
            if self.proc.poll() is None:
                self.proc.kill() if sig == getattr(signal, 'SIGKILL', None) else self.proc.terminate()
            return
        try:
            os.killpg(self.proc.pid, sig)
        except ProcessLookupError:
            pass  # the whole group is gone

    def await_eof(self, timeout):
        deadline = time.monotonic() + timeout
        with self.cond:
            while not self.eof and time.monotonic() < deadline:
                self.cond.wait(deadline - time.monotonic())
            return self.eof

    def stop(self, timeout):
        """SIGTERM the group, SIGKILL it after `timeout`, reap: bounded in every case.

        Returns the exit status, or None if even SIGKILL left it unreaped."""
        if self.proc.poll() is None:
            self.signal_group(signal.SIGTERM)
        try:
            self.returncode = self.proc.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            self.forced = True
            self.signal_group(signal.SIGKILL)
            try:
                self.returncode = self.proc.wait(timeout=KILL_GRACE)
            except subprocess.TimeoutExpired:
                return None  # still in Daemon.live for kill_children()
        Daemon.live.discard(self)
        if not self.await_eof(5):
            self.signal_group(signal.SIGKILL)  # something it started still holds its output
            self.await_eof(5)
        if self.eof:  # closing under a reader still blocked in read1() would wait for it
            self.proc.stdout.close()
        return self.returncode


def kill_children():
    """Last resort at exit or when the test wrapper is gone: SIGKILL every live child's group."""
    for daemon in list(Daemon.live):
        daemon.signal_group(signal.SIGKILL)
    for daemon in list(Daemon.live):
        try:
            daemon.proc.wait(timeout=KILL_GRACE)
        except subprocess.TimeoutExpired:
            continue
        Daemon.live.discard(daemon)


atexit.register(kill_children)


def watch_lifeline():
    """Exit with the test wrapper. CTest's TIMEOUT kills only its direct child,
    fipsd_test_tmpdir_run, which holds the write end of FIPSD_TEST_LIFELINE_FD:
    EOF there means it is gone, and the harness and its children must not
    outlive it holding CTest's pipes."""
    value = os.environ.pop('FIPSD_TEST_LIFELINE_FD', '')
    try:
        fd = int(value)
        os.fstat(fd)
    except (ValueError, OSError):
        return
    os.set_inheritable(fd, False)

    def watch():
        try:
            while os.read(fd, 1):
                pass
        except OSError:
            pass
        print('udp_interop: the test wrapper is gone (CTest timeout or kill); killing children', file=sys.stderr, flush=True)
        kill_children()
        os._exit(124)
    threading.Thread(target=watch, name='lifeline', daemon=True).start()


def exit_on_signal(signum, frame):
    raise SystemExit(128 + signum)  # unwinds through each scenario's shutdown


class NativeFlow:
    """One flow on either daemon's native datagram API (docs/reference/native-api.md).

    `connect` replies with one JSON line and the flow's SOCK_SEQPACKET half over
    SCM_RIGHTS in the same sendmsg. The command connection owns nothing and is
    closed at once, as the shipped Rust client does; the descriptor is the flow.
    """

    def __init__(self, api_path, peer_npub, remote_port, timeout=10):
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as conn:
            conn.settimeout(timeout)
            conn.connect(str(api_path))
            conn.sendall(json.dumps({'command': 'connect', 'params': {
                'peer': peer_npub, 'remote_port': remote_port}}).encode() + b'\n')
            buf, fds = b'', []
            while b'\n' not in buf:
                fdbuf = array.array('i')
                data, ancillary, _flags, _addr = conn.recvmsg(
                    65536, socket.CMSG_LEN(4 * fdbuf.itemsize), getattr(socket, "MSG_CMSG_CLOEXEC", 0))
                if not data:
                    raise Failure('native API closed the connection before replying')
                buf += data
                for level, kind, payload in ancillary:
                    if level == socket.SOL_SOCKET and kind == socket.SCM_RIGHTS:
                        fdbuf.frombytes(payload[:len(payload) - len(payload) % fdbuf.itemsize])
                        fds.extend(fdbuf)
        reply = json.loads(buf.split(b'\n', 1)[0])
        if reply.get('status') != 'ok':
            for fd in fds:
                os.close(fd)
            raise Failure(f'native API connect refused: {reply}')
        if len(fds) != 1:
            for fd in fds:
                os.close(fd)
            raise Failure(f'native API connect reply carried {len(fds)} descriptors, expected 1: {reply}')
        self.data = reply['data']
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET, fileno=fds[0])
        self.sock.settimeout(timeout)

    def send(self, payload):
        if len(payload) > self.data['max_payload']:
            raise Failure(f'payload {len(payload)} exceeds max_payload {self.data["max_payload"]}')
        sent = self.sock.send(payload)
        if sent != len(payload):
            raise Failure(f'short write on the native flow: {sent} of {len(payload)}')

    def recv(self):
        """One datagram off the flow's descriptor, under the socket's deadline."""
        try:
            return self.sock.recv(65536)
        except socket.timeout:
            raise Failure('no datagram arrived on the Rust native flow in time')

    def close(self):
        self.sock.close()


class Node:
    def __init__(self, name, daemon, control_path):
        self.name = name
        self.daemon = daemon
        self.control = control_path
        self.npub = None
        self.node_addr = None
        self.deadline = None

    def query(self, command, params=None):
        timeout = 10 if self.deadline is None else min(10, max(0.001, self.deadline - time.monotonic()))
        return control(self.control, command, params, timeout=timeout)


def reserve_port(family):
    """Pick a free loopback port. The daemon's own bind is authoritative if another process races it."""
    with socket.socket(family, socket.SOCK_DGRAM) as reservation:
        reservation.bind((LOOPBACK[6 if family == socket.AF_INET6 else 4], 0))
        return reservation.getsockname()[1]


class Scenario:
    def __init__(self, args, family, initiator, root, env):
        self.args = args
        self.family = family
        self.initiator = initiator  # 'c' or 'rust'
        self.root = root
        self.env = env
        self.tag = f'{initiator}{family}'
        self.nodes = {}
        self.lines = []
        self.flow = None
        self.sent_payloads = []
        self.rust_received = []  # every datagram the Rust daemon's flows received through fipsd
        self.reply_payload = b'nostrc fipsd -> fips (rust) via libfipsnative'
        self.tool = None
        self.echo = None

    # -- infrastructure -----------------------------------------------------
    def note(self, line):
        stamp = time.strftime('%H:%M:%S')
        text = f'[{stamp} {self.tag}] {line}'
        self.lines.append(text)
        print(text, flush=True)

    def host(self, port):
        return f'[{LOOPBACK[6]}]:{port}' if self.family == 6 else f'{LOOPBACK[4]}:{port}'

    def check_alive(self):
        for node in self.nodes.values():
            if not node.daemon.alive():
                raise Failure(f'{node.name} exited with {node.daemon.proc.returncode} during the scenario')

    def wait(self, what, deadline, predicate):
        """Observe until success, within one absolute deadline including socket IO."""
        last = None
        while True:
            self.check_alive()
            if time.monotonic() >= deadline:
                raise Failure(f'timed out waiting for {what}; last observation: {last}')
            for node in self.nodes.values():
                node.deadline = deadline
            try:
                value = predicate()
                if time.monotonic() > deadline:
                    raise Failure(f'{what} completed after its deadline')
                self.note(f'ok: {what}')
                return value
            except NotYet as e:
                last = e.args[0] if e.args else None
            except (ControlError, socket.timeout, ConnectionError, FileNotFoundError) as e:
                last = f'{type(e).__name__}: {e}'
            finally:
                for node in self.nodes.values():
                    node.deadline = None
            if time.monotonic() >= deadline:
                raise Failure(f'timed out waiting for {what}; last observation: {last}')
            with next(iter(self.nodes.values())).daemon.cond:
                next(iter(self.nodes.values())).daemon.cond.wait(min(POLL, max(0, deadline - time.monotonic())))

    def start(self):
        af = socket.AF_INET6 if self.family == 6 else socket.AF_INET
        c_port, rust_port = reserve_port(af), reserve_port(af)
        r_sock, r_api, c_sock, c_api = (self.root / f'r{self.tag}.sock', self.root / f'r{self.tag}a.sock',
                                        self.root / f'c{self.tag}.sock', self.root / f'c{self.tag}a.sock')
        rust_config = self.root / f'rust-{self.tag}.yaml'
        rust_config.write_text(f'''
node:
  identity: {{nsec: '{'2' * 64}', persistent: false}}
  control: {{enabled: true, socket_path: '{r_sock}'}}
  native_api: {{enabled: true, socket_path: '{r_api}'}}
  rekey: {{after_secs: {self.args.rekey_after_secs}}}
tun: {{enabled: false}}
dns: {{enabled: false}}
transports:
  udp: {{bind_addr: '{self.host(rust_port)}'}}
''')
        c_config = self.root / f'c-{self.tag}.yaml'
        c_config.write_text(f'''
node:
  identity: {{nsec: '{'1' * 64}', persistent: false}}
  control: {{enabled: true, socket_path: '{c_sock}'}}
  native_api: {{enabled: true, socket_path: '{c_api}'}}
  rekey: {{after_secs: {self.args.rekey_after_secs}}}
tun: {{enabled: false}}
dns: {{enabled: false}}
transports:
  udp: {{bind_addr: '{self.host(c_port)}'}}
''')
        rust = Daemon('rust', [self.args.rust, '-c', str(rust_config)], self.env, self.root)
        self.nodes['rust'] = Node('rust', rust, r_sock)
        c = Daemon('c', [self.args.fipsd, '-c', str(c_config)], self.env, self.root)
        self.nodes['c'] = Node('c', c, c_sock)
        self.api_path = r_api
        self.c_api_path = c_api
        self.ports = {'rust': rust_port, 'c': c_port}
        deadline = time.monotonic() + self.args.startup_timeout
        rust.await_log('control socket listening', deadline)
        c.await_log('fipsd running', deadline)
        c.await_log('native api socket listening', deadline)
        for node in self.nodes.values():
            status = self.wait(f'{node.name} show_status', deadline, lambda n=node: n.query('show_status'))
            node.npub, node.node_addr = status['npub'], status['node_addr']
            self.note(f'{node.name}: npub={node.npub} node_addr={node.node_addr} udp={self.host(self.ports[node.name])}')
        self.other = {'c': self.nodes['rust'], 'rust': self.nodes['c']}
        # The C client over libfipsnative holds the port the Rust flow targets on
        # fipsd's native API: it prints each datagram it accepts and answers it.
        self.tool = Daemon('c-client', [self.args.native_tool, 'serve', str(c_api), str(self.args.remote_port),
                                        self.reply_payload.hex()], self.env, self.root)
        self.tool.await_log(f'holding port {self.args.remote_port}', deadline)
        self.note(f'c-client (libfipsnative) holding port {self.args.remote_port} on {c_api}')
        if self.args.rust_echo:
            # Upstream's Rust client, unmodified, on fipsd's native API.
            self.echo = Daemon('rust-echo', [self.args.rust_echo, str(c_api), str(self.args.echo_port)], self.env, self.root)
            self.echo.await_log(f'holding port {self.args.echo_port}', deadline)
            self.note(f'rust-echo (pinned fips::native::client) holding port {self.args.echo_port} on {c_api}')

    # -- 1. IK link -----------------------------------------------------------
    def link(self):
        init, resp = self.nodes[self.initiator], self.other[self.initiator]
        result = init.query('connect', {'npub': resp.npub, 'address': self.host(self.ports[resp.name]),
                                        'transport': 'udp'})
        self.note(f'{init.name} connect -> {result}')
        link_state = {'c': 'active', 'rust': 'connected'}

        def both_linked():
            seen = {}
            for node in self.nodes.values():
                links = node.query('show_links')['links']
                peers = node.query('show_peers')['peers']
                seen[node.name] = (links, [p.get('npub') for p in peers])
                if len(links) != 1 or len(peers) != 1:
                    raise NotYet(seen)
                if links[0]['state'] != link_state[node.name]:
                    raise NotYet(seen)
                expected_direction = 'outbound' if node is init else 'inbound'
                if links[0]['direction'] != expected_direction:
                    raise Failure(f'{node.name} link direction {links[0]["direction"]}, expected {expected_direction}: {links}')
                if peers[0]['npub'] != self.other[node.name].npub:
                    raise Failure(f'{node.name} peer npub {peers[0]["npub"]} is not {self.other[node.name].npub}')
            return seen

        seen = self.wait('IK link established on both sides', time.monotonic() + self.args.link_timeout, both_linked)
        for name, (links, _) in seen.items():
            self.note(f'{name} show_links: {json.dumps(links[0])}')
        self.link_up_at = time.monotonic()
        for node in self.nodes.values():
            status = node.query('show_status')
            if status['peer_count'] != 1 or status['link_count'] != 1:
                raise Failure(f'{node.name} show_status counts after link: {status}')

    # -- 2. tree and filters --------------------------------------------------
    def tree_and_filters(self):
        def converged():
            trees = {n.name: n.query('show_tree') for n in self.nodes.values()}
            roots = {t['root'] for t in trees.values()}
            if len(roots) != 1 or any(t['peer_tree_count'] != 1 for t in trees.values()):
                raise NotYet({k: (t['root'], t['is_root'], t['depth'], t['peer_tree_count']) for k, t in trees.items()})
            root_side = [n for n, t in trees.items() if t['is_root']]
            if len(root_side) != 1:
                raise NotYet({k: t['is_root'] for k, t in trees.items()})
            leaf = [n for n in trees if n != root_side[0]][0]
            if trees[leaf]['depth'] != 1 or trees[root_side[0]]['depth'] != 0:
                raise NotYet({k: t['depth'] for k, t in trees.items()})
            if trees[leaf]['parent'] != trees[root_side[0]]['my_node_addr']:
                raise Failure(f'{leaf} parent {trees[leaf]["parent"]} is not the root node {trees[root_side[0]]["my_node_addr"]}')
            blooms = {n.name: n.query('show_bloom')['peer_filters'] for n in self.nodes.values()}
            for name, filters in blooms.items():
                if len(filters) != 1 or not filters[0].get('has_filter'):
                    raise NotYet({'bloom': blooms})
            peer_rows = {n.name: n.query('show_peers')['peers'] for n in self.nodes.values()}
            if any(len(rows) != 1 for rows in peer_rows.values()):
                raise NotYet({'peers': peer_rows})
            peers = {name: rows[0] for name, rows in peer_rows.items()}
            for name, row in peers.items():
                want = 'is_child' if name == root_side[0] else 'is_parent'
                if not row.get(want):
                    raise NotYet({name: {k: row.get(k) for k in ('is_parent', 'is_child')}})
            return trees, blooms, root_side[0]

        trees, blooms, root = self.wait('spanning tree converged and bloom filters exchanged',
                                        time.monotonic() + self.args.link_timeout, converged)
        self.note(f'tree root: {root} ({trees[root]["root"]}); '
                  + '; '.join(f'{n} depth={t["depth"]} coords={t["my_coords"]}' for n, t in trees.items()))
        for name, filters in blooms.items():
            self.note(f'{name} show_bloom.peer_filters[0]: {json.dumps(filters[0])}')

    # -- helpers for sessions and counters -----------------------------------
    def sessions(self, node):
        return node.query('show_sessions')['sessions']

    def native_counters(self, node):
        return node.query('show_metrics')['native']

    def session_established(self, expect_initiator):
        """Both sides hold exactly one established, proven session with complementary roles."""
        rows = {}
        for node in self.nodes.values():
            s = self.sessions(node)
            rows[node.name] = s
            if len(s) != 1 or s[0]['state'] != 'established' or s[0].get('identity') != 'proven':
                raise NotYet(rows)
            if s[0].get('npub') != self.other[node.name].npub:
                raise Failure(f'{node.name} session peer {s[0].get("npub")} is not {self.other[node.name].npub}: {s}')
            if s[0]['is_initiator'] != (node.name == expect_initiator):
                raise Failure(f'{node.name} is_initiator={s[0]["is_initiator"]} but {expect_initiator} opened the session: {s}')
        return rows

    def send_native(self, payload, what):
        before = self.native_counters(self.nodes['c'])
        self.flow.send(payload)
        self.sent_payloads.append(payload)
        want_dgrams, want_bytes = len(self.sent_payloads), sum(map(len, self.sent_payloads))

        def delivered():
            now = self.native_counters(self.nodes['c'])
            if now['received_datagrams'] < want_dgrams:
                raise NotYet(now)
            if now['received_datagrams'] != want_dgrams or now['received_bytes'] != want_bytes:
                raise Failure(f'C native counters {now} do not match {want_dgrams} datagram(s)/{want_bytes} byte(s) sent')
            return now

        after = self.wait(f'{what}: C received {len(payload)} bytes (native counters)',
                          time.monotonic() + self.args.session_timeout, delivered)
        self.note(f'C native counters before={before["received_datagrams"]}/{before["received_bytes"]} '
                  f'after={after["received_datagrams"]}/{after["received_bytes"]}')
        # The C client accepted the flow fipsd announced and read the bytes.
        self.tool.await_log(f'received {payload.hex()}', time.monotonic() + self.args.session_timeout)
        self.note(f'{what}: c-client accepted the flow and read {len(payload)} bytes')
        # C->Rust: the C client's reply crosses fipsd, the wire and the Rust
        # daemon to the Rust flow's descriptor, and Rust counts it.
        got = self.flow.recv()
        if got != self.reply_payload:
            raise Failure(f'the C client\'s reply arrived as {got!r}, expected {self.reply_payload!r}')
        self.note(f'C->Rust datagram: {len(got)} bytes from the C client read off the Rust flow')
        self.rust_received.append(got)
        want_in, want_in_bytes = len(self.rust_received), sum(map(len, self.rust_received))

        def rust_received():
            stats = self.nodes['rust'].query('show_native_flows')['stats']
            if stats['received_datagrams'] < want_in:
                raise NotYet(stats)
            if stats['received_datagrams'] != want_in or stats['received_bytes'] != want_in_bytes:
                raise Failure(f'Rust native stats {stats} do not match {want_in} C->Rust datagram(s)/{want_in_bytes} byte(s)')
            return stats

        stats = self.wait(f'Rust show_native_flows counts {want_in} received datagram(s) from C',
                          time.monotonic() + self.args.session_timeout, rust_received)
        self.note(f'Rust native stats: received {stats["received_datagrams"]}/{stats["received_bytes"]}')
        # The C client closed the served flow; wait for fipsd to release it so
        # the next datagram announces a fresh flow rather than landing on a
        # half-closed one.
        self.tool.await_log('closed', time.monotonic() + self.args.session_timeout)

        def c_released():
            flows = self.nodes['c'].query('show_native_flows')['flows']
            held = [f for f in flows if f['local_port'] == self.args.remote_port]
            if held:
                raise NotYet(held)
            return flows

        self.wait('C released the served flow', time.monotonic() + self.args.session_timeout, c_released)

        def rust_flow_listed():
            # The flow list is Rust's tick-published view; its counters are live.
            flows = self.nodes['rust'].query('show_native_flows')
            if not any(f['flow_id'] == self.flow.data['flow_id'] for f in flows['flows']):
                raise NotYet(flows)
            if flows['stats']['sent_datagrams'] < want_dgrams or flows['stats']['sent_bytes'] < want_bytes:
                raise Failure(f'Rust native stats {flows["stats"]} below {want_dgrams} datagram(s)/{want_bytes} byte(s) written')
            return flows['stats']

        stats = self.wait(f'Rust show_native_flows lists flow {self.flow.data["flow_id"]}',
                          time.monotonic() + self.args.session_timeout, rust_flow_listed)
        self.note(f'Rust native stats: sent {stats["sent_datagrams"]}/{stats["sent_bytes"]}')

    def probe_reports(self, source, target, started=None, retry_quantized=False):
        """Collect at most two probes; retry only an otherwise healthy no_echo.

        With retry_quantized (callers that require a positive RTT sample), an
        otherwise healthy loopback quantized_echo() report gets the same single
        bounded retry: one RTT budget need not yield a >= 1 ms sample there.
        """
        deadline = time.monotonic() + self.args.probe_timeout
        started = started or source.query('probe_start', {'npub': target.npub})
        reports = []
        for attempt in (1, 2):
            probe_id = started['probe_id']
            self.note(f'{source.name}->{target.name} probe attempt {attempt}: {json.dumps(started)}')

            def done():
                poll = source.query('probe_poll', {'probe_id': probe_id})
                if poll['state'] != 'done':
                    raise NotYet(poll)
                return poll['report']

            report = self.wait(f'{source.name}->{target.name} probe {probe_id} finished', deadline, done)
            reports.append(report)
            self.note(f'{source.name}->{target.name} probe attempt {attempt} report: {json.dumps(report)}')
            rtt = report['rtt']
            healthy = (report['overall'] == 'partial' and
                       all(report[s]['verdict'] in ('ok', 'skipped') for s in ('bloom', 'discovery')) and
                       (report['path']['verdict'] == 'ok' or
                        (report['path']['verdict'] == 'skipped' and report['path']['reason'] == 'direct_peer')) and
                       report['session']['verdict'] in ('ok', 'skipped') and report['session']['established'])
            no_echo = (rtt['verdict'] == 'failed' and rtt['reason'] == 'no_echo' and
                       rtt['reports_seen'] > 0 and rtt['samples'] == 0 and
                       rtt['zero_samples'] == 0 and rtt['arith_failures'] == 0)
            if not healthy or not (no_echo or (retry_quantized and quantized_echo(rtt))):
                return reports
            if attempt == 2:
                reasons = '/'.join(r['rtt']['reason'] for r in reports)
                raise Failure(f'{source.name}->{target.name}: two consecutive {reasons} reports: {json.dumps(reports)}')
            # Upstream budgets RTT at max(3s, 4 ticks), not the session MMP
            # ceiling (10s), and exposes no budget override. One report need
            # not carry a fresh usable timestamp echo. Retry the diagnostic,
            # not the poll of its already-terminal job, without accepting it
            # as RTT evidence or extending the harness's absolute deadline.
            self.note(f'{source.name}->{target.name}: retrying RTT-only {rtt["reason"]} once')
            started = source.query('probe_start', {'npub': target.npub})

    def probe(self, prober, expect_preexisting):
        """Run a diagnostic probe from `prober` toward the other node and return its report."""
        target = self.other[prober.name]
        reports = self.probe_reports(prober, target, retry_quantized=True)
        report, measured = reports[0], reports[-1]
        if measured['overall'] != 'ok':
            raise Failure(f'{prober.name} probe overall={measured["overall"]}: {json.dumps(reports)}')
        # A direct peer needs no bloom lookup or discovery, and a session that
        # already exists is reported as a skipped (preexisting) session stage.
        for stage, accepted in (('bloom', ('ok', 'skipped')), ('discovery', ('ok', 'skipped')), ('path', ('ok',)),
                                ('session', ('skipped',) if expect_preexisting else ('ok',))):
            if report[stage]['verdict'] not in accepted:
                raise Failure(f'{prober.name} probe stage {stage} verdict {report[stage]["verdict"]}: {json.dumps(report[stage])}')
        if not report['session']['established']:
            raise Failure(f'{prober.name} probe ran without an established session: {json.dumps(report["session"])}')
        if report['session']['preexisting'] != expect_preexisting:
            raise Failure(f'{prober.name} probe session.preexisting={report["session"]["preexisting"]}, expected {expect_preexisting}')
        if measured['rtt']['verdict'] != 'ok' or measured['rtt']['samples'] < 1 or measured['rtt']['reports_seen'] < 1:
            raise Failure(f'{prober.name} probe measured no RTT: {json.dumps(reports)}')
        if any(r['cleanup']['session_created_and_torn_down'] for r in reports):
            raise Failure(f'{prober.name} probe tore the shared session down: {json.dumps(report["cleanup"])}')
        # An RTT sample exists only if the target received the prober's session
        # frames (CoordsWarmup) and its MMP report came back over the session:
        # traffic in both directions under the current keys. Both daemons expose
        # the session SRTT only once it has been measured.
        snapshots = {}
        for node in (prober, target):
            rows = self.sessions(node)
            snapshots[node.name] = rows
            if len(rows) != 1:
                raise Failure(f'{node.name} holds {len(rows)} sessions after the {prober.name} probe: {rows}')
        observed = snapshots[prober.name][0]
        srtt = observed.get('mmp', {}).get('srtt_ms')
        if srtt is None:
            raise Failure(f'{prober.name} session shows no SRTT after its probe: {json.dumps(observed)}')
        self.note(f'{prober.name} session SRTT {srtt} ms after probe {measured["probe_id"]}')
        return measured

    # -- 3/4. XK session and datagrams ----------------------------------------
    def session_and_datagrams(self):
        c, rust = self.nodes['c'], self.nodes['rust']
        # A native-API connect is local registration only: no peer is contacted
        # until the first write, so opening the flow first is free in both orders.
        self.flow = NativeFlow(self.api_path, c.npub, self.args.remote_port)
        self.note(f'rust native connect -> {json.dumps(self.flow.data)}')
        payload = bytes(range(64)) + b'nostrc fipsd <- fips (rust)'
        if self.initiator == 'c':
            # C opens the XK session through its probe; the Rust->C datagram
            # below rides that session and adopts it, so the probe leaves it
            # intact (`cleanup.session_left_intact`) rather than tearing it down.
            started = c.query('probe_start', {'npub': rust.npub})
            self.note(f'c probe_start (opens the XK session) -> {started}')
            def ready():
                rows = self.session_established('c')
                # The probe records its adoption baseline in the observe that
                # passes its session stage; the datagram must land after that.
                poll = c.query('probe_poll', {'probe_id': started['probe_id']})
                if poll['state'] != 'running':
                    raise Failure(f'c probe finished before the Rust->C datagram could adopt its session: {json.dumps(poll)}')
                verdict = poll['report']['session']['verdict']
                if verdict != 'ok':
                    raise NotYet({'probe_session_verdict': verdict})
                return rows

            rows = self.wait('XK session established on both sides (C initiator) and probe session stage passed',
                             time.monotonic() + self.args.session_timeout, ready)
            self.session_up_at = time.monotonic()
            for name, s in rows.items():
                self.note(f'{name} show_sessions: {json.dumps(s[0])}')
            self.send_native(payload, 'Rust->C datagram over the C-initiated session')

            reports = self.probe_reports(c, rust, started, retry_quantized=True)
            report, measured = reports[0], reports[-1]
            if measured['overall'] != 'ok' or report['session']['verdict'] != 'ok' or measured['rtt']['verdict'] != 'ok':
                raise Failure(f'c probe did not complete cleanly: {json.dumps(reports)}')
            if report['session']['preexisting'] or not report['session']['established']:
                raise Failure(f'c probe did not open the session itself: {json.dumps(report["session"])}')
            if measured['rtt']['samples'] < 1:
                raise Failure(f'c probe measured no RTT: {json.dumps(reports)}')
            if report['cleanup']['session_created_and_torn_down'] or not report['cleanup']['session_left_intact']:
                raise Failure('c probe tore its session down before the Rust->C datagram adopted it: '
                              + json.dumps(report['cleanup']))
            if report['cleanup']['left_intact_reason'] != 'adopted_by_traffic':
                raise Failure(f'c probe left the session for an unexpected reason: {json.dumps(report["cleanup"])}')
            self.wait('session survives the C probe on both sides', time.monotonic() + self.args.session_timeout,
                      lambda: self.session_established('c'))
        else:
            # Rust opens the XK session for its first native datagram.
            self.send_native(payload, 'Rust->C datagram opening a Rust-initiated session')
            rows = self.wait('XK session established on both sides (Rust initiator)',
                             time.monotonic() + self.args.session_timeout,
                             lambda: self.session_established('rust'))
            self.session_up_at = time.monotonic()
            for name, s in rows.items():
                self.note(f'{name} show_sessions: {json.dumps(s[0])}')
        # K bits as first observed on each side; a flip from here is one rekey.
        self.initial_session_k = {name: s[0]['current_k_bit'] for name, s in rows.items()}
        if len(set(self.initial_session_k.values())) != 1:
            raise Failure(f'session K bits disagree right after establishment: {self.initial_session_k}')
        def peers_ready():
            rows = {n.name: n.query('show_peers')['peers'] for n in self.nodes.values()}
            if any(len(peers) != 1 for peers in rows.values()):
                raise NotYet(rows)
            return {name: peers[0] for name, peers in rows.items()}

        peers = self.wait('both peer snapshots before recording link K bits',
                          time.monotonic() + self.args.link_timeout, peers_ready)
        self.initial_link_k = {name: row['current_k_bit'] for name, row in peers.items()}
        if len(set(self.initial_link_k.values())) != 1:
            raise Failure(f'link K bits disagree: {self.initial_link_k}')
        self.note(f'initial K bits: session={self.initial_session_k} link={self.initial_link_k}')
        # Session traffic in both directions over the live session.
        self.probe(c, expect_preexisting=True)
        self.probe(rust, expect_preexisting=True)
        if self.echo:
            # Rust daemon's client -> fipsd -> upstream's Rust client (echo) -> fipsd -> Rust daemon.
            echo_flow = NativeFlow(self.api_path, c.npub, self.args.echo_port)
            payload = b'echo me through the pinned rust client on fipsd'
            echo_flow.send(payload)
            got = echo_flow.recv()
            echo_flow.close()
            if got != payload:
                raise Failure(f'the Rust echo client on fipsd returned {got!r}, expected {payload!r}')
            self.echo.await_log(f'returned {len(payload)} bytes', time.monotonic() + self.args.session_timeout)
            self.note(f'rust-echo on fipsd returned {len(got)} bytes to the Rust daemon\'s flow')
            # C's native counters saw this datagram too (it was accepted by
            # the echo client) and Rust's received the echo, so the
            # exact-count checks must expect both.
            self.sent_payloads.append(payload)
            self.rust_received.append(got)

    # -- 5. rekey ---------------------------------------------------------------
    def rekey(self):
        def session_k_flipped():
            rows = {n.name: self.sessions(n) for n in self.nodes.values()}
            for name, s in rows.items():
                if len(s) != 1:
                    raise Failure(f'{name} holds {len(s)} sessions during rekey: {s}')
            for name, s in rows.items():
                if s[0]['state'] != 'established' or s[0]['current_k_bit'] == self.initial_session_k[name]:
                    raise NotYet({k: (v[0]['state'], v[0]['current_k_bit']) for k, v in rows.items()})
            return rows

        rows = self.wait('FSP session rekey: K bit flipped on both sides',
                         self.session_up_at + self.args.rekey_timeout, session_k_flipped)
        self.note('session rekey observed after %.1fs: %s' % (
            time.monotonic() - self.session_up_at,
            {k: {'current_k_bit': v[0]['current_k_bit'], 'state': v[0]['state'], 'is_draining': v[0].get('is_draining')} for k, v in rows.items()}))

        def link_k_flipped():
            peers = {n.name: n.query('show_peers')['peers'] for n in self.nodes.values()}
            for name, p in peers.items():
                if len(p) != 1:
                    raise Failure(f'{name} holds {len(p)} peers during link rekey: {p}')
            for name, p in peers.items():
                if p[0]['current_k_bit'] == self.initial_link_k[name]:
                    raise NotYet({k: (v[0]['current_k_bit'], v[0].get('rekey_in_progress')) for k, v in peers.items()})
            return peers

        peers = self.wait('FMP link rekey: K bit flipped on both sides',
                          self.link_up_at + self.args.link_rekey_timeout, link_k_flipped)
        self.note('link rekey observed after %.1fs: %s' % (
            time.monotonic() - self.link_up_at,
            {k: {'current_k_bit': v[0]['current_k_bit'], 'our_session_index': v[0].get('our_session_index')} for k, v in peers.items()}))
        # Traffic under the new keys, both directions.
        self.send_native(b'post-rekey datagram under the new session keys', 'post-rekey Rust->C datagram')
        self.probe(self.nodes['c'], expect_preexisting=True)
        self.probe(self.nodes['rust'], expect_preexisting=True)
        for node in self.nodes.values():
            metrics = node.query('show_metrics')
            session, handshake = metrics['session'], metrics['handshake']
            flat = {**{f'session.{k}': v for k, v in session.items()}, **{f'handshake.{k}': v for k, v in handshake.items()}}
            suspicious = {k: v for k, v in flat.items() if v and any(
                word in k for word in ('mismatch', 'unknown', 'bad_state', 'decrypt', 'replay', 'expired', 'unanswered', 'failed'))}
            self.note(f'{node.name} session counters: {json.dumps(session)}')
            self.note(f'{node.name} handshake counters: {json.dumps(handshake)}')
            if suspicious:
                raise Failure(f'{node.name} reports protocol failures: {suspicious}')
            status = node.query('show_status')
            if status['peer_count'] != 1 or status['session_count'] != 1 or status['link_count'] != 1:
                raise Failure(f'{node.name} show_status after rekey: {status}')

    # -- 6. shutdown ------------------------------------------------------------
    def shutdown(self):
        """Stop every helper and daemon, whatever fails: never raises a test failure."""
        failures = []
        if self.flow:
            try:
                self.flow.close()
            except OSError as e:
                failures.append(f'closing the native flow: {e}')
        for helper in (self.tool, self.echo):
            if not helper:
                continue
            try:
                helper.stop(self.args.shutdown_timeout)
            except Exception as e:  # noqa: BLE001 - keep stopping the rest
                failures.append(f'{helper.name} stop: {type(e).__name__}: {e}')
            if helper.sanitizer_report():
                failures.append(f'{helper.name} log carries a sanitizer report')
        for name in self.nodes:
            node = self.nodes.get(name)
            if not node:
                continue
            try:
                code = node.daemon.stop(self.args.shutdown_timeout)
            except Exception as e:  # noqa: BLE001 - keep stopping the rest
                failures.append(f'{name} stop: {type(e).__name__}: {e}')
                continue
            if code is None:
                failures.append(f'{name} was not reaped {KILL_GRACE}s after SIGKILL')
            elif node.daemon.forced:
                failures.append(f'{name} did not exit on SIGTERM within {self.args.shutdown_timeout}s (killed)')
            elif code != 0:
                failures.append(f'{name} exited {code}')
            if node.daemon.sanitizer_report():
                failures.append(f'{name} log carries a sanitizer report')
            if node.control.exists():
                failures.append(f'{name} left its control socket {node.control} behind')
        if hasattr(self, 'api_path') and self.api_path.exists():
            failures.append(f'rust left its native API socket {self.api_path} behind')
        if hasattr(self, 'c_api_path') and self.c_api_path.exists():
            failures.append(f'c left its native API socket {self.c_api_path} behind')
        return failures

    def run(self):
        try:
            self.start()
            self.link()
            self.tree_and_filters()
            self.session_and_datagrams()
            self.rekey()
        except Exception:
            self.snapshot_failure()
            raise
        finally:
            failures = self.shutdown()
        if failures:
            raise Failure('; '.join(failures))
        self.note('scenario passed')

    def snapshot_failure(self):
        """Capture live control state before shutdown removes the sessions."""
        for node in self.nodes.values():
            deadline = time.monotonic() + 5
            node.deadline = deadline
            try:
                for command in ('show_sessions', 'show_peers', 'show_metrics'):
                    if time.monotonic() >= deadline:
                        break
                    try:
                        self.note(f'{node.name} failure {command}: {json.dumps(node.query(command))}')
                    except Exception as error:
                        self.note(f'{node.name} failure {command}: {type(error).__name__}: {error}')
            finally:
                node.deadline = None

    def dump_logs(self):
        for node in self.nodes.values():
            print(f'----- {self.tag}: {node.name} log -----', flush=True)
            for line in node.daemon.text().splitlines():
                print(f'[{node.name}] {line}')
        print(f'----- {self.tag}: harness trace -----', flush=True)
        print('\n'.join(self.lines), flush=True)


class MultiScenario(Scenario):
    """Small UDP graphs, reusing the two-node lifecycle/control/diagnostic API.

    Control sockets expose snapshots, not subscriptions. As in Scenario.wait,
    bounded observation polls state; it never treats elapsed time as completion.
    """

    TREE_BOUND = 30.0  # upstream STP fallback, not an unconditional protocol guarantee

    def __init__(self, args, topology, root, env):
        super().__init__(args, args.family, 'multi', root, env)
        self.topology = topology
        self.tag = topology
        self.count = 5 if topology in ('ring', 'full-mesh') else 3
        self.edges = {(i, j) for i in range(self.count) for j in range(i + 1, self.count)
                      if topology == 'full-mesh' or
                      (i == 0 if topology == 'star' else j == i + 1) or
                      (topology == 'ring' and i == 0 and j == self.count - 1)}
        self.api_paths = {}
        self.receivers = {}
        self.receivers_stopping = False
        self.reply_payload = b'multi-node UDP reply'
        self.ports = {}

    def start(self):
        af = socket.AF_INET6 if self.family == 6 else socket.AF_INET
        # Interleave roles: 2 C + 1 Rust for line/star, 3 C + 2 Rust for ring/mesh.
        for i in range(self.count):
            rust_indices = (2,) if self.topology == 'line' else (1, 3)
            role = 'rust' if i in rust_indices else 'c'
            name = f'{role}{i}'
            port = reserve_port(af)
            sock, api = self.root / f'm{i}.sock', self.root / f'a{i}.sock'
            config = self.root / f'm{i}.yaml'
            config.write_text(f"""
node:
  identity: {{nsec: '{str(i + 1) * 64}', persistent: false}}
  control: {{enabled: true, socket_path: '{sock}'}}
  native_api: {{enabled: true, socket_path: '{api}'}}
tun: {{enabled: false}}
dns: {{enabled: false}}
transports:
  udp: {{bind_addr: '{self.host(port)}'}}
""")
            binary = self.args.rust if role == 'rust' else self.args.fipsd
            daemon = Daemon(name, [binary, '-c', str(config)], self.env, self.root)
            self.nodes[name] = Node(name, daemon, sock)
            self.ports[name] = port
            self.api_paths[name] = api
        deadline = time.monotonic() + self.args.startup_timeout
        for node in self.nodes.values():
            node.daemon.await_log('control socket listening' if node.name.startswith('rust') else 'fipsd running', deadline)
            status = self.wait(f'{node.name} ready', deadline, lambda n=node: n.query('show_status'))
            node.npub, node.node_addr = status['npub'], status['node_addr']
        for name in self.nodes:
            receiver = Daemon(f'{name}-receiver', [self.args.native_tool, 'serve',
                str(self.api_paths[name]), str(self.args.remote_port), self.reply_payload.hex()], self.env, self.root)
            self.receivers[name] = receiver
            receiver.await_log('holding port', deadline)
        self.ordered = list(self.nodes.values())
        self.started_at = time.monotonic()
        for a, b in sorted(self.edges):
            self.connect(a, b)

    def connect(self, a, b):
        target = self.ordered[b]
        self.ordered[a].query('connect', {'npub': target.npub, 'transport': 'udp',
                                         'address': self.host(self.ports[target.name])})

    def tree_state(self, announcements=True):
        trees = [n.query('show_tree') for n in self.ordered]
        index = {n.node_addr: i for i, n in enumerate(self.ordered)}
        for i, (node, tree) in enumerate(zip(self.ordered, trees)):
            component = {i}
            for _ in self.ordered:
                component |= {b if a in component else a for a, b in self.edges
                              if a in component or b in component}
            root = min(self.ordered[j].node_addr for j in component)
            neighbors = {self.ordered[b if a == i else a].npub for a, b in self.edges if i in (a, b)}
            links, peers = node.query('show_links')['links'], node.query('show_peers')['peers']
            if (len(links) != len(neighbors) or {p['npub'] for p in peers} != neighbors or
                    any(l['state'] != ('connected' if node.name.startswith('rust') else 'active') for l in links)):
                raise NotYet({node.name: {'links': links, 'neighbors': neighbors}})
            for peer in tree['peers'] if announcements else ():
                neighbor = index.get(peer['node_addr'])
                if neighbor is None or peer.get('coords') != trees[neighbor]['my_coords']:
                    raise NotYet({node.name: {'stale_peer_announcement': peer}})
            coords = tree['my_coords']
            if (tree['root'] != root or tree['is_root'] != (node.node_addr == root) or
                    tree['depth'] >= self.count or len(coords) != tree['depth'] + 1 or
                    coords[0] != node.node_addr or coords[-1] != root or tree['peer_tree_count'] != len(neighbors)):
                raise NotYet({node.name: tree})
            if tree['is_root']:
                if tree['depth'] != 0:
                    raise NotYet(tree)
            else:
                parent = index.get(tree['parent'])
                if (parent is None or tuple(sorted((i, parent))) not in self.edges or
                        tree['depth'] != trees[parent]['depth'] + 1 or coords[1:] != trees[parent]['my_coords']):
                    raise NotYet({node.name: tree})
            filters = node.query('show_bloom')['peer_filters']
            if len(filters) != len(neighbors) or any(not f['has_filter'] for f in filters):
                raise NotYet({node.name: filters})
            if announcements:
                for filt in filters:
                    role = filt['tree_role']
                    if role == 'none':
                        continue
                    expected = sum((filt['peer'] in trees[j]['my_coords']) if role == 'child'
                                   else (node.node_addr not in trees[j]['my_coords']) for j in component)
                    if (filt.get('estimated_count') or 0) + 0.25 < expected:
                        raise NotYet({node.name: {'aggregate_filter_not_ready': filt, 'expected': expected}})
        return trees

    def converge(self, phase, start):
        trees = self.wait(f'{phase}: valid component roots, parent chains, coordinates, filters',
                          start + self.TREE_BOUND, self.tree_state)
        self.note(f'{phase} convergence={time.monotonic() - start:.3f}s bound={self.TREE_BOUND}s')
        return trees

    def probe_pair(self, source, target, started=None):
        reports = self.probe_reports(source, target, started)
        report = reports[-1]
        rtt = report['rtt']
        # A matching authenticated echo below the millisecond timestamp
        # resolution proves a route, but cannot produce a positive RTT sample
        # (UPSTREAM.md I-5: reason sub_millisecond or bad_timestamp_echo).
        # After the bounded no_echo retry, preserve all other diagnostic
        # failures; this is not a latency benchmark.
        quantized = quantized_echo(rtt)
        if (report['overall'] not in (('ok', 'partial') if quantized else ('ok',)) or
                any(report[s]['verdict'] not in ('ok', 'skipped') for s in ('bloom', 'discovery')) or
                report['path']['verdict'] not in ('ok', 'skipped') or
                (report['path']['verdict'] == 'skipped' and report['path']['reason'] != 'direct_peer') or
                report['session']['verdict'] not in ('ok', 'skipped') or not report['session']['established'] or
                (rtt['samples'] < 1 and not quantized)):
            raise Failure(f'{source.name}->{target.name}: {json.dumps(report)}')
        return report

    def datagram_pair(self, source, target):
        # Separate the session and application-delivery stages, as the
        # existing two-node C-initiated fixture does. Do not make the route
        # assertion depend on a one-shot native first write queued during XK.
        started = source.query('probe_start', {'npub': target.npub})
        def established():
            poll = source.query('probe_poll', {'probe_id': started['probe_id']})
            if poll['state'] != 'running':
                raise Failure(f'probe finished before payload adoption: {poll}')
            for node, other in ((source, target), (target, source)):
                sessions = node.query('show_sessions')['sessions']
                if not any(s['remote_addr'] == other.node_addr and s['state'] == 'established' and
                           s.get('identity') == 'proven' for s in sessions):
                    raise NotYet({node.name: sessions})
            if poll['report']['session']['verdict'] not in ('ok', 'skipped'):
                raise NotYet(poll)
            return True
        self.wait(f'{source.name}<->{target.name} authenticated session before payload',
                  time.monotonic() + self.args.session_timeout, established)
        # As in the paired diagnostics, seed both identities. In particular,
        # an indirect C receiver needs the sender's key to announce a native
        # flow to its client, not just an established session's address.
        reverse = target.query('probe_start', {'npub': source.npub})
        payload = b'multi-node UDP application datagram'
        before = target.query('show_metrics')['native']
        flow = NativeFlow(self.api_paths[source.name], target.npub, self.args.remote_port)
        try:
            flow.send(payload)
            def delivered():
                now = target.query('show_metrics')['native']
                if now['received_datagrams'] <= before['received_datagrams']:
                    raise NotYet(now)
                if now['received_bytes'] - before['received_bytes'] != len(payload):
                    raise Failure(f'wrong datagram length: {now}')
                return now
            try:
                self.wait(f'{source.name}->{target.name} application datagram',
                          time.monotonic() + self.args.session_timeout, delivered)
                if flow.recv() != self.reply_payload:
                    raise Failure(f'{target.name} native client returned the wrong datagram')
            except Failure:
                for node in (source, target):
                    for command in ('show_sessions', 'show_routing', 'show_metrics'):
                        self.note(f'{node.name} {command}: {json.dumps(node.query(command))}')
                raise
        finally:
            flow.close()
        report = self.probe_pair(source, target, started)
        if report['cleanup']['session_created_and_torn_down']:
            raise Failure(f'{source.name}->{target.name}: payload did not adopt the probe session: {report}')
        self.probe_pair(target, source, reverse)

    def probe_bidirectional(self, source, target):
        # A successful probe may remove the idle session it created while the
        # opposite probe still needs it. Adopt *every* pair with observed data
        # first, including C<->C and Rust<->Rust, not just mixed-language pairs.
        sender, receiver = source, target
        if source.name.startswith('c') and target.name.startswith('rust'):
            sender, receiver = target, source
        self.datagram_pair(sender, receiver)
        forward = source.query('probe_start', {'npub': target.npub})
        reverse = target.query('probe_start', {'npub': source.npub})
        for a, b, started in ((source, target, forward), (target, source, reverse)):
            report = self.probe_pair(a, b, started)
            if not report['session']['preexisting'] or report['cleanup']['session_created_and_torn_down']:
                raise Failure(f'{a.name}->{b.name}: paired probe did not retain the adopted session: {report}')

    def check_alive(self):
        super().check_alive()
        for receiver in self.receivers.values():
            if not self.receivers_stopping and not receiver.alive():
                raise Failure(f'{receiver.name} exited during the scenario')

    def shutdown(self):
        failures = []
        self.receivers_stopping = True
        for receiver in self.receivers.values():
            try:
                code = receiver.stop(self.args.shutdown_timeout)
            except Exception as e:  # noqa: BLE001 - keep stopping the rest
                failures.append(f'{receiver.name} stop: {type(e).__name__}: {e}')
                continue
            if receiver.forced or code not in (0, -signal.SIGTERM) or receiver.sanitizer_report():
                failures.append(f'{receiver.name} failed: exit={code}, forced={receiver.forced}\n{receiver.text()}')
        if self.receivers:
            def released():
                rows = {name: node.query('show_native_flows') for name, node in self.nodes.items()}
                if any(row['flows'] or row['listeners'] for row in rows.values()):
                    raise NotYet(rows)
                return rows
            try:
                self.wait('native clients released flows and listeners before daemon shutdown',
                          time.monotonic() + self.args.shutdown_timeout, released)
            except Exception as error:  # noqa: BLE001 - a dead daemon must not skip stopping the others
                failures.append(f'{type(error).__name__}: {error}' if not isinstance(error, Failure) else str(error))
        return failures + super().shutdown()

    def snapshot_failure(self):
        """Capture live control state before shutdown removes the sessions."""
        for node in self.nodes.values():
            deadline = time.monotonic() + 5
            node.deadline = deadline
            try:
                for command in ('show_sessions', 'show_peers', 'show_metrics'):
                    if time.monotonic() >= deadline:
                        break
                    try:
                        self.note(f'{node.name} failure {command}: {json.dumps(node.query(command))}')
                    except Exception as error:
                        self.note(f'{node.name} failure {command}: {type(error).__name__}: {error}')
            finally:
                node.deadline = None

    def dump_logs(self):
        super().dump_logs()
        for receiver in self.receivers.values():
            print(f'----- {self.tag}: {receiver.name} log -----\n{receiver.text()}', flush=True)

    def soak(self):
        end = time.monotonic() + 60 * self.args.soak_minutes
        baseline = self.tree_state()
        previous = {}
        link_ids = {}
        parent_positions = [(t['parent'], t['depth']) for t in baseline]
        parent_optimizations = 0
        samples = 0
        while time.monotonic() < end:
            self.check_alive()
            try:
                trees = self.tree_state(announcements=False)
            except (NotYet, ControlError) as e:
                raise Failure(f'tree/link instability during soak: {e}') from e
            for node, old, tree in zip(self.ordered, baseline, trees):
                if old['root'] != tree['root']:
                    raise Failure(f'{node.name}: root changed during soak: {old} -> {tree}')
                links = node.query('show_links')['links']
                signature = sorted((l['link_id'], l['direction']) for l in links)
                if node.name in link_ids and link_ids[node.name] != signature:
                    raise Failure(f'{node.name}: link replaced during soak')
                link_ids[node.name] = signature
                metrics = node.query('show_metrics')
                counters = {f'{g}.{k}': v for g in ('tree', 'bloom', 'lookup', 'forwarding', 'handshake')
                            for k, v in metrics[g].items() if type(v) is int}
                for key, value in counters.items():
                    if value < previous.get((node.name, key), 0):
                        raise Failure(f'{node.name}: counter {key} decreased')
                    previous[node.name, key] = value
            positions = [(t['parent'], t['depth']) for t in trees]
            parent_optimizations += sum(a != b for a, b in zip(parent_positions, positions))
            parent_positions = positions
            samples += 1
            # Snapshot control has no push stream; wake on child log output or
            # the next bounded observation, never infer stability from a sleep.
            with self.ordered[0].daemon.cond:
                self.ordered[0].daemon.cond.wait(min(POLL, max(0, end - time.monotonic())))
        self.note(f'soak passed: {self.args.soak_minutes:g} minutes, {samples} observations, stable root/links, monotone counters, {parent_optimizations} parent optimizations')

    def run(self):
        try:
            self.start()
            trees = self.converge(self.topology, self.started_at)
            parents = [(i, next(j for j, n in enumerate(self.ordered) if n.node_addr == t['parent']))
                       for i, t in enumerate(trees) if not t['is_root']]
            a, b = next(((i, j) for i, j in parents
                         if self.ordered[i].name.startswith('c') != self.ordered[j].name.startswith('c')), parents[0])
            edge = tuple(sorted((a, b)))

            def cut():
                start = time.monotonic()
                self.ordered[a].query('disconnect', {'npub': self.ordered[b].npub})
                self.edges.remove(edge)
                self.converge('parent edge removed', start)

            # A complete graph has no multi-hop path until an edge is removed.
            # Cut before creating sessions: this checks cold address discovery,
            # not recovery of stale established-session coordinates.
            if self.topology == 'full-mesh':
                cut()
            routed = False
            for i, source in enumerate(self.ordered):
                for j, target in enumerate(self.ordered[i + 1:], i + 1):
                    self.probe_bidirectional(source, target)
                    if (source.name.startswith('rust') != target.name.startswith('rust') and
                            (i, j) not in self.edges):
                        routed = True
                        self.note(f'{source.name}<->{target.name}: arrival proves >=2 UDP hops')
            if self.topology != 'full-mesh':
                cut()
            start = time.monotonic()
            self.edges.add(edge)
            self.connect(a, b)
            self.converge('parent edge restored', start)
            if not routed:
                raise Failure('scenario never proved a >=2-hop application datagram')
            if self.args.soak_minutes:
                self.soak()
        except Exception:
            self.snapshot_failure()
            raise
        finally:
            failures = self.shutdown()
            for path in self.api_paths.values():
                if path.exists():
                    failures.append(f'native API socket left behind: {path}')
        if failures:
            raise Failure('; '.join(failures))
        self.note('multi-node scenario passed')


def main():
    watch_lifeline()
    for signum in (signal.SIGTERM, signal.SIGHUP):
        signal.signal(signum, exit_on_signal)
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--fipsd', required=True)
    ap.add_argument('--rust', default=os.environ.get('FIPSD_UDP_INTEROP_RUST_BINARY', ''),
                    help='fips daemon built from the libfips-core/UPSTREAM.md pin (SKIP when unset)')
    ap.add_argument('--family', type=int, choices=(4, 6), default=4)
    ap.add_argument('--topology', choices=('line', 'star', 'ring', 'full-mesh', 'all'))
    ap.add_argument('--soak-minutes', type=float, default=0, help='opt-in stable-tree observation period per topology')
    ap.add_argument('--initiator', choices=('c', 'rust', 'both'), default='both')
    ap.add_argument('--rekey-after-secs', type=int, default=25,
                    help='node.rekey.after_secs for both daemons: with the 15 s jitter the first session '
                         'rekey falls 10-40 s after establishment, after the probes above have finished')
    ap.add_argument('--remote-port', type=int, default=4242)
    ap.add_argument('--native-tool', default=os.environ.get('FIPSD_NATIVE_CLIENT_TOOL', ''),
                    help='fipsd_native_client_tool, the C client over libfipsnative (required)')
    ap.add_argument('--rust-echo', default=os.environ.get('FIPSD_UDP_INTEROP_RUST_ECHO', ''),
                    help='optional native-echo client built from the pinned Rust tree; runs on fipsd\'s native API')
    ap.add_argument('--echo-port', type=int, default=4600)
    ap.add_argument('--startup-timeout', type=float, default=30)
    ap.add_argument('--link-timeout', type=float, default=30)
    ap.add_argument('--session-timeout', type=float, default=30)
    ap.add_argument('--probe-timeout', type=float, default=60)
    ap.add_argument('--rekey-timeout', type=float, default=60,
                    help='seconds after session establishment for the FSP K bit to flip')
    ap.add_argument('--link-rekey-timeout', type=float, default=120,
                    help='seconds after link establishment for the FMP K bit to flip')
    ap.add_argument('--shutdown-timeout', type=float, default=20)
    args = ap.parse_args()
    if args.soak_minutes < 0 or not args.soak_minutes < float('inf'):
        ap.error('--soak-minutes must be finite and nonnegative')
    if args.soak_minutes and not args.topology:
        ap.error('--soak-minutes requires --topology')
    # CTest passes --rust "" when the cache variable is unset; the environment
    # is the fallback so a configured tree can still opt in at run time.
    args.rust = args.rust or os.environ.get('FIPSD_UDP_INTEROP_RUST_BINARY', '')
    if not args.rust or not Path(args.rust).is_file():
        print('SKIP: set FIPSD_UDP_INTEROP_RUST_BINARY to a fips daemon built from the libfips-core/UPSTREAM.md pin')
        return 77
    args.native_tool = args.native_tool or os.environ.get('FIPSD_NATIVE_CLIENT_TOOL', '')
    if not args.native_tool or not Path(args.native_tool).is_file():
        print('FAIL: --native-tool (FIPSD_NATIVE_CLIENT_TOOL) must name the built fipsd_native_client_tool', file=sys.stderr)
        return 1
    args.rust_echo = args.rust_echo or os.environ.get('FIPSD_UDP_INTEROP_RUST_ECHO', '')
    if args.rust_echo and not Path(args.rust_echo).is_file():
        print(f'FAIL: --rust-echo names no file: {args.rust_echo}', file=sys.stderr)
        return 1
    root = os.environ.get('FIPSD_TEST_TMPDIR')
    if root:
        root = Path(root)
    else:
        root = Path(tempfile.mkdtemp(prefix='fd-', dir='/tmp'))
        atexit.register(shutil.rmtree, root, True)
    env = dict(os.environ, FIPSD_WORKERS='2', TOKIO_WORKER_THREADS='2', NO_COLOR='1')
    env['RUST_LOG'] = ','.join(filter(None, (env.get('RUST_LOG', 'info'),
        'fips::node::handlers::handshake=debug', 'fips::node::handlers::session=debug')))
    if args.topology:
        shapes = ('line', 'star', 'ring', 'full-mesh') if args.topology == 'all' else (args.topology,)
        for shape in shapes:
            scenario = MultiScenario(args, shape, root, env)
            try:
                scenario.run()
            except Exception as e:  # noqa: BLE001 - preserve all daemon logs
                traceback.print_exc()
                scenario.dump_logs()
                print(f'FAIL [{shape}]: {type(e).__name__}: {e}', file=sys.stderr, flush=True)
                return 1
        return 0
    initiators = ('c', 'rust') if args.initiator == 'both' else (args.initiator,)
    for initiator in initiators:
        scenario = Scenario(args, args.family, initiator, root, env)
        scenario.note(f'=== IPv{args.family}: {initiator} initiates the IK link ===')
        try:
            scenario.run()
        except Exception as e:  # noqa: BLE001 - every failure must carry both logs
            traceback.print_exc()
            scenario.dump_logs()
            print(f'FAIL [{scenario.tag}]: {type(e).__name__}: {e}', file=sys.stderr, flush=True)
            return 1
    print(f'C<->Rust UDP interop over IPv{args.family}: both role assignments passed')
    return 0


if __name__ == '__main__':
    sys.exit(main())
