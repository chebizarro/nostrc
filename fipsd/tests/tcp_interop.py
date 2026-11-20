#!/usr/bin/env python3
"""Real C/Rust TCP link interop; the full UDP interop gate is f66sr.1.

`--scenario smoke` (the default): one fipsd dials one pinned Rust daemon over
loopback TCP and both control sockets report the link.

`--scenario rekey-saturated` (nostrc-3vl7v): fipsd dials the Rust daemon
through a bandwidth-limited TCP relay, and both daemons flood the other with
native-API datagrams at about twice the relay's rate. With 16 KiB socket
buffers at each end the writers block, so both directions' TCP data lanes
are full and refusing (`show_transports` `send_errors` rises on both sides).
fipsd's control lane then carries its link rekey msg1/msg2 past up to 64
queued old-key data frames, the case upstream's FIFO Rust daemon never
produces itself. `node.rekey.after_messages` makes the link rekey due as soon
as the 31 s link floor allows. The scenario asserts, on both sides: the link
K bit flips (rekey completed) within the normal deadline; the link, its id
and the single session survive (no re-handshake); no decrypt, replay,
unknown-index or handshake-state error counter moves through a full drain
window after the flip; every link frame either side received was
authenticated (the peer's `packets_recv` keeps rising after the flip); and
both daemons exit 0 on SIGTERM with no sanitizer report.

No build or source-checkout mutation here. Supply a binary built from the
libfips-core/UPSTREAM.md pin in a separate CARGO_TARGET_DIR.
"""
import argparse
import array
import errno
import json
import os
from pathlib import Path
import selectors
import signal
import socket
import subprocess
import threading
import time

SANITIZER_MARKERS = (b'AddressSanitizer', b'LeakSanitizer', b'ThreadSanitizer',
                     b'UndefinedBehaviorSanitizer', b'runtime error:')
SOCKET_BUFFER = 16384
# Node addresses (show_status) of the two fixed test identities: on a crossing
# link rekey the smaller node's attempt wins (rekey README, IK msg1 row).
NSEC_LARGER, NSEC_SMALLER = '1' * 64, '2' * 64  # 34da5e39..., 126ef405...


class Daemon:
    """A child whose merged stdout/stderr is drained continuously, so a busy
    log never blocks it on a full pipe."""

    def __init__(self, name, argv, env, cwd):
        self.name = name
        self.output = bytearray()
        self.cond = threading.Condition()
        self.proc = subprocess.Popen(argv, cwd=cwd, env=env, stdout=subprocess.PIPE,
                                     stderr=subprocess.STDOUT)
        self.pump = threading.Thread(target=self._pump, daemon=True)
        self.pump.start()

    def _pump(self):
        while True:
            chunk = os.read(self.proc.stdout.fileno(), 65536)
            with self.cond:
                if not chunk:
                    self.cond.notify_all()
                    return
                self.output.extend(chunk)
                self.cond.notify_all()

    def text(self):
        with self.cond:
            return self.output.decode(errors='replace')

    def await_log(self, needle, timeout=25):
        deadline = time.monotonic() + timeout
        with self.cond:
            while needle.encode() not in self.output.lower():
                remaining = deadline - time.monotonic()
                assert remaining > 0, f'{self.name}: timed out waiting for {needle}'
                assert self.proc.poll() is None or self.pump.is_alive(), f'{self.name} exited'
                self.cond.wait(min(remaining, 0.5))

    def count(self, needle):
        with self.cond:
            return self.output.lower().count(needle.encode())

    def stop(self, failures):
        if self.proc.poll() is None:
            self.proc.send_signal(signal.SIGTERM)
        try:
            code = self.proc.wait(timeout=15)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            self.proc.wait()
            failures.append(f'{self.name} did not join on SIGTERM')
            code = self.proc.returncode
        self.pump.join(5)
        self.proc.stdout.close()
        if code != 0:
            failures.append(f'{self.name} exited {code}')
        if any(m in self.output for m in SANITIZER_MARKERS):
            failures.append(f'{self.name} log carries a sanitizer report')


def query(path, command, params=None):
    with socket.socket(socket.AF_UNIX) as conn:
        conn.settimeout(10)
        conn.connect(str(path))
        request = {'command': command}
        if params is not None:
            request['params'] = params
        conn.sendall(json.dumps(request).encode() + b'\n')
        response = json.loads(conn.makefile('rb').readline())
    assert response['status'] == 'ok', response
    return response['data']


def observe(what, deadline, probe):
    """Successive control snapshots until `probe` returns a value. Rust's
    off-loop control view is tick-published, so a state the logs already show
    reaches its control socket up to a tick later; socket replies are the
    backpressure, plus a short pause, never a retried assertion."""
    while True:
        value = probe()
        if value:
            return value
        assert time.monotonic() < deadline, f'timed out waiting for {what}'
        time.sleep(0.1)


def reserve_port():
    # Reserve an address only to select a free port; the daemon's ordinary
    # exclusive listener bind is authoritative if another process races it.
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as reservation:
        reservation.bind(('127.0.0.1', 0))
        return reservation.getsockname()[1]


class Relay:
    """A loopback TCP relay limited to `rate` bytes per second each way.

    Each direction holds at most one small buffer and stops reading while it
    is full, with 8 KiB socket buffers on both of its sockets, so a daemon
    writing faster than the rate soon has a full kernel send buffer and a
    blocked writer. Bytes are forwarded unchanged and in order."""

    CAP = 4096

    def __init__(self, upstream_port, rate):
        self.upstream = ('127.0.0.1', upstream_port)
        self.rate = rate
        self.listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.listener.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 8192)
        self.listener.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 8192)
        self.listener.bind(('127.0.0.1', 0))
        self.listener.listen(4)
        self.listener.setblocking(False)
        self.port = self.listener.getsockname()[1]
        self.accepted = 0
        self.stop_r, self.stop_w = socket.socketpair()
        self.thread = threading.Thread(target=self._run, daemon=True)
        self.thread.start()

    def _accept(self, pipes):
        down, _ = self.listener.accept()
        up = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        for s in (down, up):
            s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 8192)
            s.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 8192)
            s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        up.connect(self.upstream)
        down.setblocking(False)
        up.setblocking(False)
        self.accepted += 1
        now = time.monotonic()
        pipes.append({'src': down, 'dst': up, 'buf': bytearray(), 'tokens': 0.0, 'at': now})
        pipes.append({'src': up, 'dst': down, 'buf': bytearray(), 'tokens': 0.0, 'at': now})

    def _run(self):
        pipes = []
        sel = selectors.DefaultSelector()
        while True:
            now = time.monotonic()
            sel.close()
            sel = selectors.DefaultSelector()
            sel.register(self.listener, selectors.EVENT_READ, None)
            sel.register(self.stop_r, selectors.EVENT_READ, 'stop')
            wants = {}
            for p in pipes:
                p['tokens'] = min(float(self.CAP), p['tokens'] + (now - p['at']) * self.rate)
                p['at'] = now
                if len(p['buf']) < self.CAP:
                    wants.setdefault(p['src'], 0)
                    wants[p['src']] |= selectors.EVENT_READ
                if p['buf'] and p['tokens'] >= min(len(p['buf']), 512):
                    wants.setdefault(p['dst'], 0)
                    wants[p['dst']] |= selectors.EVENT_WRITE
            for s, ev in wants.items():
                sel.register(s, ev, s)
            # The relay is a rate shaper: it wakes at least every 5 ms to refill
            # its token buckets. Nothing the harness asserts waits on it.
            for key, ev in sel.select(0.005):
                if key.data == 'stop':
                    for p in pipes:
                        p['src'].close()
                    self.listener.close()
                    return
                if key.data is None:
                    self._accept(pipes)
                    continue
                s = key.data
                for p in list(pipes):
                    if p['src'] is s and ev & selectors.EVENT_READ and len(p['buf']) < self.CAP:
                        try:
                            data = s.recv(self.CAP - len(p['buf']))
                        except (BlockingIOError, InterruptedError):
                            continue
                        except OSError:
                            data = b''
                        if not data:
                            pipes = [q for q in pipes if q['src'] not in (p['src'], p['dst'])]
                            p['src'].close()
                            p['dst'].close()
                            break
                        p['buf'].extend(data)
                    if p['dst'] is s and ev & selectors.EVENT_WRITE and p['buf']:
                        n = int(min(p['tokens'], len(p['buf'])))
                        try:
                            sent = s.send(p['buf'][:n])
                        except (BlockingIOError, InterruptedError):
                            continue
                        except OSError:
                            continue
                        del p['buf'][:sent]
                        p['tokens'] -= sent

    def close(self):
        self.stop_w.send(b'x')
        self.thread.join(10)
        self.stop_r.close()
        self.stop_w.close()


class Flood:
    """One native-API flow written at a fixed datagram rate on its own thread.

    The flow's descriptor is a connected AF_UNIX datagram half (SOCK_SEQPACKET
    on Linux, SOCK_DGRAM on macOS); writes the daemon has not yet read are
    refused (EAGAIN) and simply not offered. Pacing shapes load; no assertion
    waits on it."""

    def __init__(self, api_path, peer_npub, remote_port, rate, size):
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as conn:
            conn.settimeout(10)
            conn.connect(str(api_path))
            conn.sendall(json.dumps({'command': 'connect', 'params': {
                'peer': peer_npub, 'remote_port': remote_port}}).encode() + b'\n')
            buf, fds = b'', []
            while b'\n' not in buf:
                fdbuf = array.array('i')
                data, ancillary, _flags, _addr = conn.recvmsg(65536, socket.CMSG_LEN(4 * fdbuf.itemsize))
                assert data, 'native API closed the connection before replying'
                buf += data
                for level, kind, cdata in ancillary:
                    if level == socket.SOL_SOCKET and kind == socket.SCM_RIGHTS:
                        fdbuf.frombytes(cdata[:len(cdata) - len(cdata) % fdbuf.itemsize])
                        fds.extend(fdbuf)
        reply = json.loads(buf.split(b'\n', 1)[0])
        if reply.get('status') != 'ok' or len(fds) != 1:
            for fd in fds:
                os.close(fd)
            raise AssertionError(f'native API connect refused: {reply} ({len(fds)} descriptors)')
        self.sock = socket.socket(fileno=fds[0])
        self.sock.setblocking(False)
        self.payload = (bytes(range(256)) * 8)[:min(size, reply['data']['max_payload'])]
        self.rate = rate
        self.sent = self.refused = 0
        self.halt = threading.Event()
        self.thread = threading.Thread(target=self._run, daemon=True)

    def start(self):
        self.thread.start()

    def _run(self):
        began = time.monotonic()
        while not self.halt.is_set():
            due = int((time.monotonic() - began) * self.rate)
            while self.sent + self.refused < due and not self.halt.is_set():
                try:
                    self.sock.send(self.payload)
                    self.sent += 1
                except (BlockingIOError, InterruptedError):
                    self.refused += 1
                except OSError as e:
                    if e.errno in (errno.ENOBUFS, errno.EAGAIN):
                        self.refused += 1
                    else:
                        raise
            self.halt.wait(0.002)

    def stop(self):
        self.halt.set()
        if self.thread.is_alive():
            self.thread.join(10)
        self.sock.close()


def rust_config(path, root, port, scenario, nsec, after_messages):
    lines = [
        'node:',
        f"  identity: {{nsec: '{nsec}', persistent: false}}",
        f"  control: {{enabled: true, socket_path: '{root / 'r.sock'}'}}",
    ]
    if scenario == 'smoke':
        lines.append('  native_api: {enabled: false}')
        tcp = f"{{bind_addr: '127.0.0.1:{port}'}}"
    else:
        lines.append(f"  native_api: {{enabled: true, socket_path: '{root / 'ra.sock'}'}}")
        lines.append(f'  rekey: {{after_messages: {after_messages}}}')
        tcp = (f"{{bind_addr: '127.0.0.1:{port}', send_buf_size: {SOCKET_BUFFER}, "
               f'recv_buf_size: {SOCKET_BUFFER}}}')
    lines += ['tun: {enabled: false}', 'dns: {enabled: false}', 'transports:', f'  tcp: {tcp}', '']
    path.write_text('\n'.join(lines))


def c_config(path, root, rust_npub, port, scenario, nsec, after_messages):
    lines = [
        'node:',
        f"  identity: {{nsec: '{nsec}', persistent: false}}",
        f"  control: {{enabled: true, socket_path: '{root / 'c.sock'}'}}",
    ]
    if scenario == 'smoke':
        tcp = "{bind_addr: '127.0.0.1:0'}"
    else:
        lines.append(f"  native_api: {{enabled: true, socket_path: '{root / 'ca.sock'}'}}")
        lines.append(f'  rekey: {{after_messages: {after_messages}}}')
        # The debug line each late or duplicate link msg2 leaves is what
        # explains its handshake.unknown_connection increment.
        lines.append('  log_level: debug')
        lines += ['tun: {enabled: false}', 'dns: {enabled: false}']
        tcp = f"{{bind_addr: '127.0.0.1:0', send_buf_size: {SOCKET_BUFFER}, recv_buf_size: {SOCKET_BUFFER}}}"
    lines += ['transports:', f'  tcp: {tcp}', 'peers:', f"  - npub: '{rust_npub}'",
              f"    addresses: [{{transport: tcp, addr: '127.0.0.1:{port}'}}]", '']
    path.write_text('\n'.join(lines))


def after_messages(args):
    # What the less loaded direction seals in 20 s: below the 31 s link
    # floor, so the message trigger, not the 45-60 s time trigger, starts the
    # link rekey as soon as the floor allows.
    return max(64, int(min(args.c_load, args.rust_load) * args.relay_rate / args.payload * 20))


def drain_watch(args):
    # Upstream's link drain (rekey Config::fmp().drain, 10 s) plus two 1 s
    # ticks, plus the time the relay takes to carry a full data lane (64
    # frames, one held) and every buffer on the way: an old-key frame still
    # queued at the flip has then arrived and been authenticated or counted.
    backlog = 65 * (args.payload + 200) + 4 * SOCKET_BUFFER + 2 * Relay.CAP
    return 12 + backlog / args.relay_rate


def peer_row(path):
    peers = query(path, 'show_peers')['peers']
    return peers[0] if len(peers) == 1 else None


def link_rows(path):
    return query(path, 'show_links')['links']


def tcp_send_errors(path):
    for t in query(path, 'show_transports')['transports']:
        if t.get('type') == 'tcp':
            return t.get('stats', {}).get('send_errors', 0)
    raise AssertionError(f'no tcp transport in {path} show_transports')


# Counters that a frame no key opens, a replayed or out-of-window frame, a
# retired or unknown receiver index or a refused handshake step would raise.
PEER_ERRORS = ('replay_suppressed_total', 'consecutive_decrypt_failures')
METRIC_ERRORS = (('handshake', 'bad_state'), ('handshake', 'unknown_connection'),
                 ('dataplane', 'unknown_index'), ('dataplane', 'orphaned_index'),
                 ('session', 'unknown_session'), ('session', 'rekey_key_mismatch'))
# Reported, not asserted: how the link rekeys met (crossing, resends), and
# the end-to-end session's own rekey. Its setup/ack/msg3 are session
# datagrams, on the data lane in both daemons; under this queueing a resent
# setup draws a second ack, which upstream counts as session bad_state
# (rekey Reason::LateReply/UnknownAttempt, handlers/rekey.rs session_reject).
METRIC_NOTES = (('handshake', 'rekey_pending'), ('handshake', 'rekey_tiebreak'), ('handshake', 'answered_before'),
                ('session', 'bad_state'))


# Each daemon's own logged causes for two of those counters. Upstream's FIFO
# data lane produces both under this load by itself: a rekey msg2 its full
# data lane refused (HandshakeStats::bad_state counts "msg2/msg3 send failed"),
# and a duplicate or late msg2 copy for an attempt already completed or given
# up (handlers/handshake.rs msg2_to_link, "Rekey msg2 for no attempt in
# flight", counted as unknown_connection). C counts and logs the second the
# same way (handlers/rekey.c, hs_metrics_rekey), under the same text; its
# control lane is never refused, so the first never occurs there. The same
# rule holds for both: every increment must carry a line of its own cause,
# and any other still fails.
def explained(log):
    lines = log.splitlines()
    return {
        'handshake.bad_state': sum('Failed to send rekey msg2' in x and 'queue full' in x for x in lines),
        'handshake.unknown_connection': sum('Rekey msg2 for no attempt in flight' in x and
                                            ('reason=LateReply' in x or 'reason=UnknownAttempt' in x) for x in lines),
    }


def error_counters(path):
    peer = peer_row(path) or {}
    metrics = query(path, 'show_metrics')
    out = {k: peer.get(k) for k in PEER_ERRORS}
    for family, key in METRIC_ERRORS:
        out[f'{family}.{key}'] = metrics[family][key]
    notes = {f'{family}.{key}': metrics[family][key] for family, key in METRIC_NOTES}
    return out, notes


def smoke(c_sock, r_sock, c):
    c.await_log('link established')
    for name, path in [('Rust', r_sock), ('C', c_sock)]:
        links = observe(f'{name} show_links', time.monotonic() + 10, lambda p=path: link_rows(p))
        print(f'{name} control show_links: {len(links)} link(s)')


def rekey_saturated(args, root, c_sock, r_sock, c, rust, relay):
    nodes = {'C': c_sock, 'Rust': r_sock}
    c.await_log('link established')
    up = time.monotonic()
    links = {n: observe(f'{n} link', up + 10, lambda p=p: link_rows(p)) for n, p in nodes.items()}
    link_ids = {n: [row['link_id'] for row in rows] for n, rows in links.items()}
    k0 = {n: observe(f'{n} peer', up + 10, lambda p=p: peer_row(p))['current_k_bit'] for n, p in nodes.items()}
    status = {n: query(p, 'show_status') for n, p in nodes.items()}
    print(f'link up through relay :{relay.port} (rate {args.relay_rate} B/s each way); '
          f'links {link_ids}, K bits {k0}')

    # Session first from C, then Rust's flow rides the same session.
    c_rate = args.c_load * args.relay_rate / args.payload
    r_rate = args.rust_load * args.relay_rate / args.payload
    print(f"node_addr C {status['C']['node_addr']} Rust {status['Rust']['node_addr']}: "
          f"{args.smaller} is smaller and wins a crossing rekey; after_messages {after_messages(args)}")
    c_flood = Flood(root / 'ca.sock', status['Rust']['npub'], 7000, c_rate, args.payload)
    c_flood.start()

    def one_session(path):
        sessions = query(path, 'show_sessions')['sessions']
        return sessions if len(sessions) == 1 and sessions[0].get('state') == 'established' else None
    for n, p in nodes.items():
        observe(f'{n} session', up + 20, lambda p=p: one_session(p))
    r_flood = Flood(root / 'ra.sock', status['C']['npub'], 7001, r_rate, args.payload)
    r_flood.start()
    sat0 = {n: tcp_send_errors(p) for n, p in nodes.items()}
    print(f'flooding C->Rust at {c_rate:.0f}/s and Rust->C at {r_rate:.0f}/s ({args.payload} B datagrams); '
          f'send_errors at start {sat0}')

    # Normal deadline: the link floor (link age 30 s plus a tick), the longest
    # time trigger (after raised to 45 s, plus 15 s jitter) and one handshake.
    deadline = up + 45 + 15 + 30 + args.extra_deadline
    flipped = {}
    while len(flipped) < 2:
        for n, p in nodes.items():
            if n not in flipped:
                row = peer_row(p)
                assert row, f'{n} lost its peer during the saturated rekey'
                if row['current_k_bit'] != k0[n]:
                    flipped[n] = (time.monotonic(), tcp_send_errors(p))
        if time.monotonic() >= deadline:
            for n, p in nodes.items():
                row = peer_row(p) or {}
                print(f"{n}: peer {json.dumps({k: v for k, v in row.items() if k not in ('paths', 'mmp')})}")
                print(f"{n}: handshake {query(p, 'show_metrics')['handshake']}")
            raise AssertionError(f'link rekey did not complete under saturation: flipped {flipped}')
        time.sleep(0.2)
    for n in nodes:
        print(f'{n}: link K bit flipped {flipped[n][0] - up:.1f} s after link-up '
              f'(send_errors {sat0[n]} -> {flipped[n][1]})')
        assert flipped[n][1] > sat0[n], f'{n} data lane was not refusing in the run-up to the rekey'
    recv1 = {n: peer_row(p)['stats']['packets_recv'] for n, p in nodes.items()}

    # Keep both lanes saturated through a full drain window after the later flip.
    settle = max(t for t, _ in flipped.values()) + drain_watch(args)
    while time.monotonic() < settle:
        time.sleep(min(0.5, max(0.0, settle - time.monotonic())))
    sat2 = {n: tcp_send_errors(p) for n, p in nodes.items()}
    errors, notes = {}, {}
    for n, p in nodes.items():
        errors[n], notes[n] = error_counters(p)
    recv2 = {n: peer_row(p)['stats']['packets_recv'] for n, p in nodes.items()}
    c_flood.stop()
    r_flood.stop()
    for n, p in nodes.items():
        rows = link_rows(p)
        sessions_now = query(p, 'show_sessions')['sessions']
        print(f'{n}: error counters {errors[n]}; rekey notes {notes[n]}; send_errors after the flip {flipped[n][1]} -> {sat2[n]}; '
              f'packets_recv {recv1[n]} -> {recv2[n]}; links {[r["link_id"] for r in rows]}; '
              f'sessions {len(sessions_now)}')
        assert sat2[n] > flipped[n][1], f'{n} data lane stopped refusing after the rekey'
        assert recv2[n] > recv1[n], f'{n} received no link frames under the new key'
        assert [r['link_id'] for r in rows] == link_ids[n], f'{n} link changed: {link_ids[n]} -> {rows}'
        assert len(sessions_now) == 1, f'{n} session count changed: {sessions_now}'
        causes = explained((rust if n == 'Rust' else c).text())
        bad = {k: v for k, v in errors[n].items() if v and v > causes.get(k, 0)}
        print(f'{n}: increments explained by its own logged causes: {causes}')
        assert not bad, f'{n} error counters moved across the saturated rekey: {bad}'
    print(f'flood C->Rust sent {c_flood.sent} refused {c_flood.refused}; '
          f'Rust->C sent {r_flood.sent} refused {r_flood.refused}; relay accepted {relay.accepted} connection(s)')
    assert relay.accepted == 1, 'the link redialled through the relay'
    assert c.count('link established') == 1, 'C re-established the link'
    # Which side initiated each link cutover: upstream logs its role at
    # debug (handlers/rekey.rs link_cutover); C's is the other one.
    log = rust.text().splitlines()
    roles = ['Rust initiator, C responder (C answered msg2 on its control lane)' if 'initiator' in line
             else 'C initiator, Rust responder (C sent msg1 on its control lane)'
             for line in log if 'Rekey cutover complete (initiator)' in line or 'K-bit flip promoting' in line]
    refused = sum('Failed to send rekey msg1' in line and 'queue full' in line for line in log)
    print(f'link cutovers: {roles}; Rust link msg1 refused by its own full data lane: {refused}')
    assert roles, 'Rust logged no link cutover'


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--fipsd', required=True)
    ap.add_argument('--rust', default=os.environ.get('FIPSD_TCP_INTEROP_RUST_BINARY', ''))
    ap.add_argument('--scenario', choices=('smoke', 'rekey-saturated'), default='smoke')
    # rekey-saturated only:
    ap.add_argument('--relay-rate', type=int, default=262144, help='relay bytes/s each way')
    ap.add_argument('--c-load', type=float, default=2.0, help="C's offered datagram bytes, times the relay rate")
    ap.add_argument('--rust-load', type=float, default=1.1, help="Rust's offered datagram bytes, times the relay rate")
    ap.add_argument('--smaller', choices=('c', 'rust'), default='c',
                    help="the side whose link rekey attempt wins a crossing (the smaller node address)")
    ap.add_argument('--payload', type=int, default=1000, help='datagram payload bytes (capped at max_payload)')
    ap.add_argument('--extra-deadline', type=float, default=0.0, help='seconds added to the rekey deadline')
    args = ap.parse_args()
    args.rust = args.rust or os.environ.get('FIPSD_TCP_INTEROP_RUST_BINARY', '')
    if not args.rust or not Path(args.rust).is_file():
        print('SKIP: set FIPSD_TCP_INTEROP_RUST_BINARY to a pinned Rust fips daemon')
        return 77
    root = Path(os.environ['FIPSD_TEST_TMPDIR'])
    r_sock, c_sock = root / 'r.sock', root / 'c.sock'
    rust_port = reserve_port()
    env = dict(os.environ, FIPSD_WORKERS='2', TOKIO_WORKER_THREADS='2', NO_COLOR='1')
    daemons, relay, failures = [], None, []
    try:
        c_nsec, rust_nsec = NSEC_LARGER, NSEC_SMALLER
        if args.scenario != 'smoke' and args.smaller == 'c':
            c_nsec, rust_nsec = rust_nsec, c_nsec
        messages = after_messages(args)
        rust_config(root / 'rust.yaml', root, rust_port, args.scenario, rust_nsec, messages)
        rust_env = env if args.scenario == 'smoke' else dict(
            env, RUST_LOG='info,fips::node::handlers::rekey=debug,fips::node::handlers::handshake=debug')
        rust = Daemon('rust', [args.rust, '-c', str(root / 'rust.yaml')], rust_env, root)
        daemons.append(rust)
        rust.await_log('control socket listening')
        rust_npub = query(r_sock, 'show_status')['npub']
        dial = rust_port
        if args.scenario != 'smoke':
            relay = Relay(rust_port, args.relay_rate)
            dial = relay.port
        c_config(root / 'c.yaml', root, rust_npub, dial, args.scenario, c_nsec, messages)
        c = Daemon('c', [args.fipsd, '-c', str(root / 'c.yaml')], env, root)
        daemons.append(c)
        if args.scenario == 'smoke':
            smoke(c_sock, r_sock, c)
        else:
            rekey_saturated(args, root, c_sock, r_sock, c, rust, relay)
        return 0
    except BaseException:
        for d in daemons:
            print(f'---- {d.name} log ----\n{d.text()[-20000:]}')
        raise
    finally:
        for d in reversed(daemons):
            d.stop(failures)
        if relay:
            relay.close()
        assert not failures, '; '.join(failures)


if __name__ == '__main__':
    raise SystemExit(main())
