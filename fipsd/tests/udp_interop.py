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
   equal to what was written). C->Rust session traffic through a C probe
   (CoordsWarmup and MMP reports over the session; Rust's session
   `packets_recv` rises and C records RTT samples) and Rust->C through a
   Rust probe. fipsd has no native API yet (Tier B4), so no C->Rust
   *application* datagram exists to send; README records this boundary.
5. One rekey cycle each for the FSP session and the FMP link, driven by
   `node.rekey.after_secs` (25 s, so with the 15 s jitter the first session
   rekey falls 10-40 s after establishment; links apply the 45 s floor and the
   30 s link-age hold on both daemons): the K bit flips on both sides from
   the value first observed (`show_sessions` and `show_peers`
   `current_k_bit`), the session count stays one, then traffic flows under
   the new keys in both directions.
6. Clean shutdown: SIGTERM -> exit 0 for both, no sanitizer report in either
   log, both daemons' control sockets and the Rust API socket unlinked.

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

POLL = 0.1
SANITIZER_MARKERS = (b'AddressSanitizer', b'LeakSanitizer', b'ThreadSanitizer',
                     b'UndefinedBehaviorSanitizer', b'runtime error:')


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
    """A child whose merged stdout/stderr is captured continuously."""

    def __init__(self, name, argv, env, cwd):
        self.name = name
        self.proc = subprocess.Popen(argv, cwd=cwd, env=env, stdin=subprocess.DEVNULL,
                                     stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
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

    def stop(self, timeout):
        if self.proc.poll() is None:
            self.proc.send_signal(signal.SIGTERM)
        try:
            self.returncode = self.proc.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            self.forced = True
            self.proc.kill()
            self.returncode = self.proc.wait()
        with self.cond:
            if not self.eof:
                self.cond.wait(5)
        self.proc.stdout.close()
        return self.returncode


class NativeFlow:
    """One flow on the Rust native datagram API (docs/reference/native-api.md).

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

    def close(self):
        self.sock.close()


class Node:
    def __init__(self, name, daemon, control_path):
        self.name = name
        self.daemon = daemon
        self.control = control_path
        self.npub = None
        self.node_addr = None

    def query(self, command, params=None):
        return control(self.control, command, params)


def reserve_port(family):
    """Pick a free loopback port. The daemon's own bind is authoritative if another process races it."""
    with socket.socket(family, socket.SOCK_DGRAM) as reservation:
        reservation.bind(('::1' if family == socket.AF_INET6 else '127.0.0.1', 0))
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

    # -- infrastructure -----------------------------------------------------
    def note(self, line):
        stamp = time.strftime('%H:%M:%S')
        text = f'[{stamp} {self.tag}] {line}'
        self.lines.append(text)
        print(text, flush=True)

    def host(self, port):
        return f'[::1]:{port}' if self.family == 6 else f'127.0.0.1:{port}'

    def check_alive(self):
        for node in self.nodes.values():
            if not node.daemon.alive():
                raise Failure(f'{node.name} exited with {node.daemon.proc.returncode} during the scenario')

    def wait(self, what, deadline, predicate):
        """Poll `predicate` until it returns a value; NotYet/ControlError mean keep waiting."""
        last = None
        while True:
            self.check_alive()
            try:
                value = predicate()
                self.note(f'ok: {what}')
                return value
            except NotYet as e:
                last = e.args[0] if e.args else None
            except ControlError as e:
                last = str(e)
            if time.monotonic() >= deadline:
                raise Failure(f'timed out waiting for {what}; last observation: {last}')
            time.sleep(POLL)

    def start(self):
        af = socket.AF_INET6 if self.family == 6 else socket.AF_INET
        c_port, rust_port = reserve_port(af), reserve_port(af)
        r_sock, r_api, c_sock = (self.root / f'r{self.tag}.sock', self.root / f'r{self.tag}a.sock',
                                 self.root / f'c{self.tag}.sock')
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
        self.ports = {'rust': rust_port, 'c': c_port}
        deadline = time.monotonic() + self.args.startup_timeout
        rust.await_log('control socket listening', deadline)
        c.await_log('fipsd running', deadline)
        for node in self.nodes.values():
            status = self.wait(f'{node.name} show_status', deadline, lambda n=node: n.query('show_status'))
            node.npub, node.node_addr = status['npub'], status['node_addr']
            self.note(f'{node.name}: npub={node.npub} node_addr={node.node_addr} udp={self.host(self.ports[node.name])}')
        self.other = {'c': self.nodes['rust'], 'rust': self.nodes['c']}

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
            peers = {n.name: n.query('show_peers')['peers'][0] for n in self.nodes.values()}
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

    def probe(self, prober, expect_preexisting):
        """Run a diagnostic probe from `prober` toward the other node and return its report."""
        target = self.other[prober.name]
        started = prober.query('probe_start', {'npub': target.npub})
        probe_id = started['probe_id']
        self.note(f'{prober.name} probe_start -> {started}')

        def done():
            poll = prober.query('probe_poll', {'probe_id': probe_id})
            if poll['state'] != 'done':
                raise NotYet({'state': poll['state'], 'overall': poll['report'].get('overall') if poll.get('report') else None})
            return poll['report']

        report = self.wait(f'{prober.name} probe {probe_id} finished', time.monotonic() + self.args.probe_timeout, done)
        self.note(f'{prober.name} probe report: {json.dumps(report)}')
        if report['overall'] != 'ok':
            raise Failure(f'{prober.name} probe overall={report["overall"]}: {json.dumps(report)}')
        # A direct peer needs no bloom lookup or discovery, and a session that
        # already exists is reported as a skipped (preexisting) session stage.
        for stage, accepted in (('bloom', ('ok', 'skipped')), ('discovery', ('ok', 'skipped')), ('path', ('ok',)),
                                ('session', ('skipped',) if expect_preexisting else ('ok',)), ('rtt', ('ok',))):
            if report[stage]['verdict'] not in accepted:
                raise Failure(f'{prober.name} probe stage {stage} verdict {report[stage]["verdict"]}: {json.dumps(report[stage])}')
        if not report['session']['established']:
            raise Failure(f'{prober.name} probe ran without an established session: {json.dumps(report["session"])}')
        if report['session']['preexisting'] != expect_preexisting:
            raise Failure(f'{prober.name} probe session.preexisting={report["session"]["preexisting"]}, expected {expect_preexisting}')
        if report['rtt']['samples'] < 1 or report['rtt']['reports_seen'] < 1:
            raise Failure(f'{prober.name} probe measured no RTT: {json.dumps(report["rtt"])}')
        if report['cleanup']['session_created_and_torn_down']:
            raise Failure(f'{prober.name} probe tore the shared session down: {json.dumps(report["cleanup"])}')
        # An RTT sample exists only if the target received the prober's session
        # frames (CoordsWarmup) and its MMP report came back over the session:
        # traffic in both directions under the current keys. Both daemons expose
        # the session SRTT only once it has been measured.
        for node in (prober, target):
            rows = self.sessions(node)
            if len(rows) != 1:
                raise Failure(f'{node.name} holds {len(rows)} sessions after the {prober.name} probe: {rows}')
        srtt = self.sessions(prober)[0].get('mmp', {}).get('srtt_ms')
        if srtt is None:
            raise Failure(f'{prober.name} session shows no SRTT after its probe: {json.dumps(self.sessions(prober)[0])}')
        self.note(f'{prober.name} session SRTT {srtt} ms after probe {probe_id}')
        return report

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

            def done():
                poll = c.query('probe_poll', {'probe_id': started['probe_id']})
                if poll['state'] != 'done':
                    raise NotYet(poll['report'].get('overall') if poll.get('report') else poll['state'])
                return poll['report']

            report = self.wait('c probe finished', time.monotonic() + self.args.probe_timeout, done)
            self.note(f'c probe report: {json.dumps(report)}')
            if report['overall'] != 'ok' or report['session']['verdict'] != 'ok' or report['rtt']['verdict'] != 'ok':
                raise Failure(f'c probe did not complete cleanly: {json.dumps(report)}')
            if report['session']['preexisting'] or not report['session']['established']:
                raise Failure(f'c probe did not open the session itself: {json.dumps(report["session"])}')
            if report['rtt']['samples'] < 1:
                raise Failure(f'c probe measured no RTT: {json.dumps(report["rtt"])}')
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
        peers = {n.name: n.query('show_peers')['peers'][0] for n in self.nodes.values()}
        self.initial_link_k = {name: row['current_k_bit'] for name, row in peers.items()}
        if len(set(self.initial_link_k.values())) != 1:
            raise Failure(f'link K bits disagree: {self.initial_link_k}')
        self.note(f'initial K bits: session={self.initial_session_k} link={self.initial_link_k}')
        # Session traffic in both directions over the live session.
        self.probe(c, expect_preexisting=True)
        self.probe(rust, expect_preexisting=True)

    # -- 5. rekey ---------------------------------------------------------------
    def rekey(self):
        def session_k_flipped():
            rows = {n.name: self.sessions(n) for n in self.nodes.values()}
            for name, s in rows.items():
                if len(s) != 1:
                    raise Failure(f'{name} holds {len(s)} sessions during rekey: {s}')
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
        failures = []
        if self.flow:
            self.flow.close()
        for name in ('c', 'rust'):
            node = self.nodes.get(name)
            if not node:
                continue
            code = node.daemon.stop(self.args.shutdown_timeout)
            if node.daemon.forced:
                failures.append(f'{name} did not exit on SIGTERM within {self.args.shutdown_timeout}s (killed)')
            elif code != 0:
                failures.append(f'{name} exited {code}')
            if node.daemon.sanitizer_report():
                failures.append(f'{name} log carries a sanitizer report')
            if node.control.exists():
                failures.append(f'{name} left its control socket {node.control} behind')
        if hasattr(self, 'api_path') and self.api_path.exists():
            failures.append(f'rust left its native API socket {self.api_path} behind')
        return failures

    def run(self):
        try:
            self.start()
            self.link()
            self.tree_and_filters()
            self.session_and_datagrams()
            self.rekey()
        finally:
            failures = self.shutdown()
        if failures:
            raise Failure('; '.join(failures))
        self.note('scenario passed')

    def dump_logs(self):
        for node in self.nodes.values():
            print(f'----- {self.tag}: {node.name} log -----', flush=True)
            for line in node.daemon.text().splitlines():
                print(f'[{node.name}] {line}')
        print(f'----- {self.tag}: harness trace -----', flush=True)
        print('\n'.join(self.lines), flush=True)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--fipsd', required=True)
    ap.add_argument('--rust', default=os.environ.get('FIPSD_UDP_INTEROP_RUST_BINARY', ''),
                    help='fips daemon built from the libfips-core/UPSTREAM.md pin (SKIP when unset)')
    ap.add_argument('--family', type=int, choices=(4, 6), default=4)
    ap.add_argument('--initiator', choices=('c', 'rust', 'both'), default='both')
    ap.add_argument('--rekey-after-secs', type=int, default=25,
                    help='node.rekey.after_secs for both daemons: with the 15 s jitter the first session '
                         'rekey falls 10-40 s after establishment, after the probes above have finished')
    ap.add_argument('--remote-port', type=int, default=4242)
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
    if not args.rust or not Path(args.rust).is_file():
        print('SKIP: set FIPSD_UDP_INTEROP_RUST_BINARY to a fips daemon built from the libfips-core/UPSTREAM.md pin')
        return 77
    root = os.environ.get('FIPSD_TEST_TMPDIR')
    if root:
        root = Path(root)
    else:
        root = Path(tempfile.mkdtemp(prefix='fd-', dir='/tmp'))
        atexit.register(shutil.rmtree, root, True)
    env = dict(os.environ, FIPSD_WORKERS='2', TOKIO_WORKER_THREADS='2', NO_COLOR='1')
    env.setdefault('RUST_LOG', 'info')
    initiators = ('c', 'rust') if args.initiator == 'both' else (args.initiator,)
    for initiator in initiators:
        scenario = Scenario(args, args.family, initiator, root, env)
        scenario.note(f'=== IPv{args.family}: {initiator} initiates the IK link ===')
        try:
            scenario.run()
        except Exception as e:  # noqa: BLE001 - every failure must carry both logs
            scenario.dump_logs()
            print(f'FAIL [{scenario.tag}]: {type(e).__name__}: {e}', file=sys.stderr, flush=True)
            return 1
    print(f'C<->Rust UDP interop over IPv{args.family}: both role assignments passed')
    return 0


if __name__ == '__main__':
    sys.exit(main())
