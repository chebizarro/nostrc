#!/usr/bin/env python3
"""Every documented fipsctl subcommand (docs/reference/cli-fipsctl.md) through
the built C fipsctl against two real fipsd daemons on loopback UDP: exit
codes, JSON shape, the IPv6 adapter composition over a fake status socket,
keygen/address without a daemon, and the link/path/probe flow once the two
daemons peer. Event-driven: state is awaited through the control socket under
one deadline, never slept for. `fipsctl_interop.py` reuses the helpers here."""
import argparse
import json
import os
from pathlib import Path
import re
import socket
import stat
import subprocess
import sys
import threading
import time

sys.path.insert(0, str(Path(__file__).resolve().parent))
import udp_interop  # noqa: E402
from udp_interop import Daemon, Failure, NotYet, control, reserve_port  # noqa: E402

# The keys whose values change between two reads of the same daemon state.
VOLATILE = {'version', 'pid', 'exe_path', 'control_socket', 'tun_name', 'allow_file', 'deny_file', 'uptime_secs',
            'started_at_ms', 'session_start_ms', 'authenticated_at_ms', 'last_seen_ms', 'last_activity_ms',
            'last_recv_ms', 'created_at_ms', 'initiated_ms', 'last_sent_ms', 'age_ms', 'last_used_ms', 'idle_ms',
            'first_seen_secs_ago', 'last_contact_secs_ago', 'since_secs', 'heard_ms_ago', 'last_rtt_ms',
            'min_rtt_ms', 'rtt_samples', 'score', 'etx', 'link_cost', 'srtt_ms', 'rttvar_ms', 'values',
            'sparklines', 'stats', 'forwarding', 'bytes_in', 'bytes_out', 'packets_in', 'packets_out',
            'frames_rx', 'bytes_rx', 'frames_failed', 'probes_unanswered', 'seq', 'sequence', 'counters',
            'peer_retention_seconds', 'window_seconds', 'series'}

# subcommand -> (control command, a key its data must carry)
QUERIES = [
    (['show', 'status'], 'show_status', 'npub'),
    (['show', 'peers'], 'show_peers', 'peers'),
    (['show', 'links'], 'show_links', 'links'),
    (['show', 'tree'], 'show_tree', 'root'),
    (['show', 'sessions'], 'show_sessions', 'sessions'),
    (['show', 'bloom'], 'show_bloom', 'peer_filters'),
    (['show', 'mmp'], 'show_mmp', 'peers'),
    (['show', 'cache'], 'show_cache', 'entries'),
    (['show', 'connections'], 'show_connections', 'connections'),
    (['show', 'transports'], 'show_transports', 'transports'),
    (['show', 'routing'], 'show_routing', 'pending_lookups'),
    (['show', 'identity-cache'], 'show_identity_cache', 'entries'),
    (['show', 'native-flows'], 'show_native_flows', 'flows'),
    (['acl', 'show'], 'show_acl', 'default_decision'),
    (['stats', 'list'], 'show_stats_list', 'metrics'),
    (['stats', 'metrics'], 'show_metrics', 'forwarding'),
    (['stats', 'peers'], 'show_stats_peers', 'peers'),
    (['stats', 'history', 'mesh_size'], 'show_stats_history', 'values'),
    (['stats', 'history', 'bytes_in', '--window', '1h', '--granularity', '1m'], 'show_stats_history', 'values'),
]


def redact(value):
    if isinstance(value, dict):
        return {k: ('<redacted>' if k in VOLATILE else redact(v)) for k, v in value.items()}
    if isinstance(value, list):
        return [redact(v) for v in value]
    return value


def shape(value):
    """What two reads of one live daemon must agree on: keys, array lengths,
    strings, booleans and nulls. Numbers are live counters, timings and
    metrics (a goodput read twice is two values) and are not compared;
    the printer's byte-exactness is proven against the goldens instead."""
    if isinstance(value, dict):
        return {k: ('<volatile>' if k in VOLATILE else shape(v)) for k, v in value.items()}
    if isinstance(value, list):
        return [shape(v) for v in value]
    if isinstance(value, bool) or value is None or isinstance(value, str):
        return value
    return '<number>'


def big_ints(value, path=''):
    """Key paths holding integers beyond i64, which a client must still print back."""
    found = []
    if isinstance(value, dict):
        for k, v in value.items():
            found += big_ints(v, f'{path}.{k}')
    elif isinstance(value, list):
        for i, v in enumerate(value):
            found += big_ints(v, f'{path}[{i}]')
    elif isinstance(value, int) and not isinstance(value, bool) and value > 2 ** 63 - 1:
        found.append(path)
    return found


# fipsctl's own five-second read and write deadlines, as it words them when a control request times out.
STALLED_CONTROL = re.compile(r'^error: (failed to (read response|send request)|no response from daemon)')


def reads_only(args):
    """Commands that change nothing in the daemon, so running one again is harmless."""
    return args[:1] in (['show'], ['stats']) or args[:2] in (['acl', 'show'], ['path', 'show'])


class Client:
    """One fipsctl binary, run per command with the given socket."""

    def __init__(self, name, binary, env):
        self.name = name
        self.binary = binary
        self.env = env
        self.table = []  # (subcommand, rc, note)

    def run(self, args, socket_path=None, timeout=60):
        """One invocation. A read-only command that gave up on a daemon starved for
        longer than fipsctl's five seconds (a host event, not a result) is run once
        more after the daemon answers again; a second stall is returned as it is."""
        result = self.run_once(args, socket_path, timeout)
        if socket_path is not None and reads_only(args) and STALLED_CONTROL.match(result.stderr):
            try:
                control(socket_path, 'show_status', timeout=30)  # blocks until the daemon answers
            except (udp_interop.ControlError, OSError, ValueError) as e:
                raise Failure(f'{self.name} {" ".join(args)}: {result.stderr.strip()}; the daemon has not answered '
                              f'a control request in the 30 s since ({type(e).__name__}: {e})') from e
            print(f'note: {self.name} {" ".join(args)} lost its request to a stalled daemon '
                  f'({result.stderr.strip()}); running it again', flush=True)
            result = self.run_once(args, socket_path, timeout)
        return result

    def run_once(self, args, socket_path, timeout):
        argv = [self.binary]
        if socket_path is not None:
            argv += ['-s', str(socket_path)]
        argv += args
        try:
            result = subprocess.run(argv, env=self.env, text=True, capture_output=True, timeout=timeout)
        except subprocess.TimeoutExpired as e:
            raise Failure(f'{self.name} {" ".join(args)}: still running after {timeout} s; '
                          f'stdout {(e.stdout or "")!r}, stderr {(e.stderr or "")!r}') from e
        if any(m.decode() in result.stderr for m in udp_interop.SANITIZER_MARKERS):
            raise Failure(f'{self.name}: sanitizer report from {args}: {result.stderr}')
        return result

    def expect(self, args, rc, socket_path=None, out=None, err=None, parse=False, note='', stall_ok=False):
        """Run and check. `stall_ok` is for a command that changes the daemon: fipsctl
        gives up after five seconds on a starved daemon that may apply the command
        afterwards, so that exit is no verdict on it. It is reported, returns None,
        and the state the caller awaits next shows whether the command took effect."""
        result = self.run(args, socket_path)
        label = ' '.join(args)
        if stall_ok and result.returncode == 1 and STALLED_CONTROL.match(result.stderr):
            print(f'note: {self.name} {label} gave up on a stalled daemon ({result.stderr.strip()}); '
                  'whether it took effect is checked by the state awaited next', flush=True)
            self.table.append((label, 1, 'stalled daemon; effect awaited'))
            return None
        if result.returncode != rc:
            raise Failure(f'{self.name} {label}: exit {result.returncode}, expected {rc}\n{result.stdout}{result.stderr}')
        if out is not None and out not in result.stdout:
            raise Failure(f'{self.name} {label}: stdout lacks {out!r}:\n{result.stdout}')
        if err is not None and err not in result.stderr:
            raise Failure(f'{self.name} {label}: stderr lacks {err!r}:\n{result.stderr}')
        if rc == 1 and not result.stderr.startswith('error: '):
            raise Failure(f'{self.name} {label}: a failure must say error: on stderr:\n{result.stderr}')
        if rc == 1 and result.stdout and not parse:
            raise Failure(f'{self.name} {label}: a failure must not print to stdout:\n{result.stdout}')
        self.table.append((label, result.returncode, note))
        if parse:
            return load_json(self, result)
        return result


def load_json(client, result):
    """The JSON a client printed. When it printed none (an empty stdout with exit 0
    included), the message carries what it did print, its exit status and the
    command, so the failure names the client rather than the parser."""
    try:
        return json.loads(result.stdout)
    except ValueError as e:
        raise Failure(f'{client.name} {" ".join(map(str, result.args[1:]))}: exit {result.returncode}, '
                      f'stdout is not JSON ({e}): stdout {result.stdout!r}, stderr {result.stderr!r}') from e


def pretty(value):
    """serde_json::to_string_pretty for the values a daemon emits (no floats outside its own formatting)."""
    return json.dumps(value, indent=2, sort_keys=True, ensure_ascii=False) + '\n'


def fipsd_config(root, name, socket_path, port, nsec, extra=''):
    path = root / f'{name}.yaml'
    path.write_text(f'''
node:
  identity: {{nsec: '{nsec}', persistent: false}}
  control: {{enabled: true, socket_path: '{socket_path}'}}
  native_api: {{enabled: false}}
  path: {{enabled: true}}
{extra}
tun: {{enabled: false}}
dns: {{enabled: false}}
transports:
  udp: {{bind_addr: '127.0.0.1:{port}'}}
''')
    return path


class Node:
    # `ready` is the log line a daemon prints once its control socket is bound
    # and listening (fipsd: after its subsystems started, the control socket
    # among them; the Rust daemon: from the control task).
    def __init__(self, name, daemon, socket_path, port, ready='fipsd running'):
        self.name, self.daemon, self.socket, self.port, self.ready = name, daemon, socket_path, port, ready
        self.npub = self.node_addr = None
        self.deadline = None

    def query(self, command, params=None):
        timeout = 10 if self.deadline is None else min(10, max(0.001, self.deadline - time.monotonic()))
        return control(self.socket, command, params, timeout=timeout)


class Patience:
    """A deadline that is a stall window: `window` seconds without a new log byte
    from any watched daemon expire the wait, and one is never longer than HARD
    windows in all. A daemon the host starves is slow, not stuck, and shows it:
    startup took 0.3 s typically and 15 s once at a load average of 40, with the
    daemon's log growing meanwhile; a stuck one is silent, and is still reported
    after one window rather than the hard stop."""

    HARD = 4

    def __init__(self, nodes, window):
        self.nodes, self.window = nodes, window
        self.began = time.monotonic()
        self.hard = self.began + self.HARD * window
        self.end = self.began + window
        self.seen = self.logged()

    def logged(self):
        return sum(len(node.daemon.log) for node in self.nodes)

    def deadline(self):
        size = self.logged()
        if size != self.seen:
            self.seen = size
            self.end = min(self.hard, time.monotonic() + self.window)
        return self.end

    def expired(self, what):
        elapsed = time.monotonic() - self.began
        return (f'timed out after {elapsed:.1f} s waiting for {what}: '
                + (f'the {self.HARD * self.window:g} s hard stop' if self.end >= self.hard
                   else f'no daemon output for {self.window:g} s'))


def wait(nodes, what, deadline, predicate, patience=None):
    """Observe through the control sockets until the predicate holds. `deadline`
    is a stall window starting now (see Patience), or pass the Patience.

    The control socket is the readiness mechanism: it has no push, so each wake
    (any output from nodes[0], else POLL) re-asks it. A request that fails
    (refused, timed out, closed mid-reply) is an observation, not a verdict: it
    is kept as the last one and the wait goes on until it expires, where Failure
    carries it; main() adds every daemon's state."""
    patience = patience or Patience(nodes, deadline - time.monotonic())
    last = None
    began = time.monotonic()
    while True:
        for node in nodes:
            if not node.daemon.alive():
                raise Failure(f'{node.name} exited with {node.daemon.proc.returncode} while waiting for {what}')
            node.deadline = patience.deadline()
        try:
            value = predicate()
            print(f'ok: {what} ({time.monotonic() - began:.2f} s)', flush=True)
            return value
        except NotYet as e:
            last = e.args[0] if e.args else None
        except (udp_interop.ControlError, OSError, ValueError) as e:
            last = f'{type(e).__name__}: {e}'
        finally:
            for node in nodes:
                node.deadline = None
        deadline = patience.deadline()
        if time.monotonic() >= deadline:
            raise Failure(f'{patience.expired(what)}; last observation: {last}')
        with nodes[0].daemon.cond:
            nodes[0].daemon.cond.wait(min(udp_interop.POLL, max(0, deadline - time.monotonic())))


def await_line(node, patience):
    """The daemon's readiness line, awaited on its own log's condition."""
    daemon = node.daemon
    needle = node.ready.encode()
    with daemon.cond:
        while needle not in daemon.log.lower():
            if daemon.eof:
                raise Failure(f'{node.name} exited before logging {node.ready!r}')
            remaining = patience.deadline() - time.monotonic()
            if remaining <= 0:
                raise Failure(patience.expired(f'{node.name} to log {node.ready!r}'))
            daemon.cond.wait(remaining)


def await_ready(nodes, window):
    """Every daemon logged its readiness line and answers show_status. Call it right
    after spawning them: one Patience covers both. The line is awaited, not polled
    for; show_status is asked once the line is there, since the control socket is
    bound before it."""
    patience = Patience(nodes, window)
    for node in nodes:
        await_line(node, patience)
        print(f'ok: {node.name} logged {node.ready!r} ({time.monotonic() - patience.began:.2f} s after spawn)', flush=True)
        status = wait([node], f'{node.name} show_status', 0, lambda n=node: n.query('show_status'), patience)
        node.npub, node.node_addr = status['npub'], status['node_addr']


# Control requests, in the order that says most about a stuck daemon.
STATE_QUERIES = ('show_status', 'show_links', 'show_peers', 'show_transports', 'show_tree')


def daemon_state(nodes, log_lines=40):
    """What every daemon looks like now, for the message of a failure: whether it
    is alive, what its control socket says (a short deadline each, and no further
    questions of a daemon that did not answer), and the tail of its log."""
    parts = []
    for node in nodes:
        alive = node.daemon.alive()
        lines = [f'--- {node.name}: {"alive" if alive else f"exited with {node.daemon.proc.returncode}"} ---']
        for command in STATE_QUERIES if alive else ():
            try:
                lines.append(f'{command}: {json.dumps(control(node.socket, command, timeout=2), sort_keys=True)}')
            except (udp_interop.ControlError, OSError, ValueError) as e:
                lines.append(f'{command}: {type(e).__name__}: {e}')
                break
        tail = node.daemon.text().splitlines()[-log_lines:]
        lines.append(f'log (last {len(tail)} lines):')
        lines.extend(f'  {line}' for line in tail)
        parts.append('\n'.join(lines))
    return '\n'.join(parts)


def fail_with_state(failure, nodes):
    """Re-raise a Failure with the state of every daemon, taken while they still run."""
    raise Failure(f'{failure}\n{daemon_state(nodes)}') from failure


class FakeAdapter:
    """A stand-in for the IPv6 adapter's status socket, answering one reply."""

    def __init__(self, path, reply):
        self.path = path
        self.reply = (json.dumps(reply) + '\n').encode()
        self.listener = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.listener.bind(str(path))
        self.listener.listen(8)
        self.connections = 0
        self.thread = threading.Thread(target=self.serve, daemon=True)
        self.thread.start()

    def serve(self):
        while True:
            try:
                conn, _ = self.listener.accept()
            except OSError:
                return
            with conn:
                self.connections += 1
                conn.makefile('rb').readline()
                conn.sendall(self.reply)

    def close(self):
        self.listener.close()
        self.thread.join(5)
        os.unlink(self.path)


class ReplayProxy:
    """A control socket that asks the daemon once per request line and answers every
    later client from that reply. A live series (stats history gains a sample each
    second) read by two client processes a moment apart is two different replies
    whenever the host is slow enough to let a sample land between them; through the
    proxy both clients render one reply, so their output must match byte for byte
    and a mismatch is the clients'. Used as a context manager."""

    def __init__(self, path, upstream):
        self.path, self.upstream = str(path), str(upstream)
        self.replies = {}
        self.closing = False
        self.listener = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.listener.bind(self.path)
        self.listener.listen(8)
        self.thread = threading.Thread(target=self.serve, daemon=True)
        self.thread.start()

    def serve(self):
        while True:
            try:
                conn, _ = self.listener.accept()
            except OSError:
                return
            with conn:
                if self.closing:
                    return
                conn.settimeout(10)
                try:
                    request = conn.makefile('rb').readline()
                    if request not in self.replies:
                        with socket.socket(socket.AF_UNIX) as up:
                            up.settimeout(10)
                            up.connect(self.upstream)
                            up.sendall(request)
                            self.replies[request] = up.makefile('rb').readline()
                    conn.sendall(self.replies[request])
                except OSError:
                    pass  # the client sees a closed connection and says so on stderr

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.closing = True
        with socket.socket(socket.AF_UNIX) as wake:  # accept() does not return on close() everywhere
            try:
                wake.connect(self.path)
            except OSError:
                pass
        self.thread.join(5)
        self.listener.close()
        os.unlink(self.path)


def start_fipsd(name, binary, root, env, port, nsec, extra=''):
    sock = root / f'{name}.sock'
    config = fipsd_config(root, name, sock, port, nsec, extra)
    daemon = Daemon(name, [binary, '-c', str(config)], env, root)
    return Node(name, daemon, sock, port)


def link(client, a, b, deadline, link_state=('active', 'active')):
    """Dial b from a through `client`, then await the link on both sides."""
    data = client.expect(['connect', b.npub, f'127.0.0.1:{b.port}', 'udp'], 0, a.socket, parse=True, stall_ok=True)
    print(f'{client.name} connect -> {data}')

    def linked():
        seen = {}
        for node, state in ((a, link_state[0]), (b, link_state[1])):
            links = node.query('show_links')['links']
            seen[node.name] = links
            if len(links) != 1 or links[0]['state'] != state:
                raise NotYet(seen)
        return seen
    wait([a, b], 'link established on both sides', deadline, linked)

    def converged():
        trees = {n.name: n.query('show_tree') for n in (a, b)}
        if len({t['root'] for t in trees.values()}) != 1 or any(t['peer_tree_count'] != 1 for t in trees.values()):
            raise NotYet({k: (t['root'], t['peer_tree_count']) for k, t in trees.items()})
        blooms = {n.name: n.query('show_bloom')['peer_filters'] for n in (a, b)}
        if any(len(f) != 1 or not f[0].get('has_filter') for f in blooms.values()):
            raise NotYet(blooms)
        return trees
    wait([a, b], 'spanning tree converged', deadline, converged)


def probe_overall(stdout):
    m = re.search(r'^  overall    (\w+), \d+ ms', stdout, re.M)
    if not m:
        raise Failure(f'probe output has no overall line:\n{stdout}')
    return m.group(1)


def probe_run(client, node, args):
    """`fipsctl probe`, run once more if it lost a control request to a stalled daemon.

    The probe runs inside the daemon, on its own deadline; fipsctl polls it, and
    gives up on the first poll that times out (five seconds, the daemon's own
    deadline), exiting 1 with the stage block it had drawn and no `overall` line.
    A daemon starved for that long (ASan at `ctest -j10` beside a build) is a host
    event, not a probe result. A stalled daemon that answers again is run once
    more; one that does not, or a second stall, fails with what fipsctl said."""
    for attempt in (1, 2):
        result = client.run(args, node.socket)
        if attempt == 2 or not STALLED_CONTROL.match(result.stderr):
            return result
        wait([node], f'{node.name} answers again after a stalled probe',
             time.monotonic() + 30, lambda: node.query('show_status'))
        print(f'note: {client.name} {" ".join(args)} lost a control request to {node.name}: '
              f'{result.stderr.strip()}; running it again', flush=True)


def check_probe(client, node, target, expect_overall=None):
    """A human and a JSON probe; the exit code is 0 only for an overall of ok."""
    result = probe_run(client, node, ['probe', target])
    try:
        overall = probe_overall(result.stdout)
    except Failure as e:
        raise Failure(f'{client.name} probe exited {result.returncode}; stderr:\n{result.stderr}\n{e}') from e
    if result.returncode != (0 if overall == 'ok' else 1):
        raise Failure(f'{client.name} probe exit {result.returncode} for overall {overall}:\n{result.stdout}')
    for needle in ('probe ', '  npub       ', '  node_addr  ', '  ipv6       ', '  cleanup    '):
        if needle not in result.stdout:
            raise Failure(f'{client.name} probe output lacks {needle!r}:\n{result.stdout}')
    if expect_overall and overall != expect_overall:
        raise Failure(f'{client.name} probe overall {overall}, expected {expect_overall}:\n{result.stdout}')
    client.table.append((f'probe {target[:12]}...', result.returncode, f'overall {overall}'))
    result = probe_run(client, node, ['probe', target, '--json'])
    try:
        report = json.loads(result.stdout)
    except ValueError as e:
        raise Failure(f'{client.name} probe --json exited {result.returncode}; stderr:\n{result.stderr}\n'
                      f'stdout is not JSON ({e}):\n{result.stdout}') from e
    for key in ('probe_id', 'overall', 'elapsed_ms', 'tick_ms', 'target', 'bloom', 'discovery', 'path', 'session', 'rtt', 'cleanup'):
        if key not in report:
            raise Failure(f'{client.name} probe --json lacks {key}: {result.stdout}')
    if result.returncode != (0 if report['overall'] == 'ok' else 1):
        raise Failure(f'{client.name} probe --json exit {result.returncode} for overall {report["overall"]}')
    if result.stdout != pretty(report):
        raise Failure(f'{client.name} probe --json is not serde-pretty:\n{result.stdout}')
    client.table.append((f'probe {target[:12]}... --json', result.returncode, f'overall {report["overall"]}'))
    return report


def fresh_npub(client):
    """An identity nobody on the mesh has announced."""
    return client.expect(['keygen', '--stdout'], 0).stdout.splitlines()[1]


def local_commands(client, root):
    """keygen, address, version, help and usage errors: no daemon involved."""
    v = client.expect(['-V'], 0, out='fipsctl ')
    if not re.match(r'^fipsctl \S+( \(rev [0-9a-f]+(-dirty)?\))?\n$', v.stdout):
        raise Failure(f'-V printed {v.stdout!r}')
    client.expect(['--version'], 0, out='target: ')
    client.expect(['--help'], 0, out='Usage: fipsctl')
    client.expect(['-h'], 0, out='Usage: fipsctl')
    client.expect(['show', '--help'], 0, out='Usage: fipsctl show')
    client.expect(['help', 'stats', 'history'], 0, out='--granularity')
    for bad in ([], ['bogus'], ['show'], ['show', 'bogus'], ['probe'], ['address', 'x', '-k', 'y'], ['--bogus'],
                ['connect', 'a', 'b'], ['path', 'pin', 'x'], ['probe', 'x', '--timeout', 'abc']):
        client.expect(bad, 1, err='Usage: fipsctl', note='usage error')
    client.expect(['-s', str(root / 'none.sock'), 'show', 'status'], 1, err='Is the FIPS daemon running?')
    keys = root / 'keys'
    out = client.expect(['keygen', '--stdout'], 0)
    nsec, npub = out.stdout.splitlines()
    if not (nsec.startswith('nsec1') and npub.startswith('npub1') and out.stderr == ''):
        raise Failure(f'keygen --stdout: {out.stdout!r} {out.stderr!r}')
    client.expect(['keygen', '-d', str(keys)], 0, err='Key files written to: ')
    if stat.S_IMODE((keys / 'fips.key').stat().st_mode) != 0o600 or stat.S_IMODE((keys / 'fips.pub').stat().st_mode) != 0o644:
        raise Failure('keygen modes')
    client.expect(['keygen', '-d', str(keys)], 1, err='key file already exists')
    before = (keys / 'fips.pub').read_text()
    client.expect(['keygen', '--dir', str(keys), '--force'], 0)
    after = (keys / 'fips.pub').read_text().strip()
    if before.strip() == after:
        raise Failure('--force did not replace the key')
    from_key = client.expect(['address', '-k', str(keys / 'fips.key')], 0).stdout
    from_pub = client.expect(['address', '--key', str(keys / 'fips.pub')], 0).stdout
    from_npub = client.expect(['address', after], 0).stdout
    if not (from_key == from_pub == from_npub and from_key.startswith('fd')):
        raise Failure(f'address disagree: {from_key!r} {from_pub!r} {from_npub!r}')
    if client.expect(['address', after.upper()], 0).stdout != from_key:
        raise Failure('an upper-case npub is the same npub')
    client.expect(['address', 'npub1notarealkey'], 1, err='invalid npub')
    client.expect(['address', 'no-such-host'], 1, err="unknown host 'no-such-host'")
    client.expect(['address', '-k', str(root / 'missing.key')], 1, err='cannot read ')
    return after, from_key


def query_round(client, node, note=''):
    """Every read-only subcommand: exit 0, JSON, the documented key, printed as serde would."""
    outputs = {}
    for args, command, key in QUERIES:
        result = client.run(args, node.socket)
        label = ' '.join(args)
        if result.returncode != 0:
            raise Failure(f'{client.name} {label} against {node.name}: exit {result.returncode}: {result.stderr}')
        data = load_json(client, result)
        if key not in data:
            raise Failure(f'{client.name} {label}: data lacks {key}: {result.stdout[:200]}')
        if result.stdout != pretty(data) and not re.search(r'\d\.\d|e[+-]\d', result.stdout):
            raise Failure(f'{client.name} {label}: not serde-pretty output:\n{result.stdout[:400]}')
        for path in big_ints(data):
            print(f'note: {node.name} {command}{path} exceeds i64 and was printed back intact')
        outputs[label] = (data, result.stdout)
        client.table.append((label, 0, note))
    plot = client.expect(['stats', 'history', 'mesh_size', '--plot'], 0, node.socket, note=note)
    if not plot.stdout.startswith('mesh_size (nodes) — ') and not plot.stdout.startswith('mesh_size: no data yet'):
        raise Failure(f'plot header: {plot.stdout!r}')
    client.expect(['stats', 'history', 'bogus'], 1, node.socket, err='error: ', note=note)
    client.expect(['stats', 'history', 'srtt_ms'], 1, node.socket, err='error: ', note=note)
    return outputs


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--fipsd', required=True)
    ap.add_argument('--fipsctl', required=True)
    ap.add_argument('--startup-timeout', type=float, default=30,
                    help='seconds without daemon output before startup is reported stuck (a stall window; 4x is the hard stop)')
    ap.add_argument('--link-timeout', type=float, default=60,
                    help='seconds without daemon output before a link/tree/teardown wait is reported stuck (stall window; 4x hard stop)')
    args = ap.parse_args()
    root = Path(os.environ['FIPSD_TEST_TMPDIR'])
    env = dict(os.environ, FIPSD_WORKERS='2', NO_COLOR='1')
    client = Client('fipsctl', args.fipsctl, env)
    npub, _ = local_commands(client, root)
    port_a, port_b = reserve_port(socket.AF_INET), reserve_port(socket.AF_INET)
    a = start_fipsd('a', args.fipsd, root, env, port_a, '1' * 64)
    b = start_fipsd('b', args.fipsd, root, env, port_b, '3' * 64)
    passed = False
    try:
        await_ready((a, b), args.startup_timeout)
        # Idle daemon: every query, the TUN keys at their fallback.
        first = query_round(client, a, 'idle')
        status = first['show status'][0]
        if (status['tun_state'], status['tun_name'], status['effective_ipv6_mtu']) != ('disabled', '-', 0):
            raise Failure(f'TUN fallback keys: {status}')
        # The adapter's socket beside the control socket, also through a symlink to it.
        adapter = FakeAdapter(root / 'a-ipv6tun.sock', {'status': 'ok', 'data': {
            'tun_state': 'active', 'tun_name': 'fips-e2e', 'effective_ipv6_mtu': 1323, 'host_drops': 7}})
        link_path = root / 'link.sock'
        os.symlink(a.socket, link_path)
        try:
            for path in (a.socket, link_path):
                composed = client.expect(['show', 'status'], 0, path, parse=True, note='adapter beside socket')
                if (composed['tun_state'], composed['tun_name'], composed['effective_ipv6_mtu']) != ('active', 'fips-e2e', 1323):
                    raise Failure(f'composition via {path}: {composed}')
                if 'host_drops' in composed:
                    raise Failure('a key beyond the three TUN keys was merged')
            peers = client.expect(['show', 'peers'], 0, a.socket, parse=True)
            if 'tun_state' in peers or adapter.connections != 2:
                raise Failure(f'only show status asks the adapter (asked {adapter.connections} times)')
        finally:
            adapter.close()
            os.unlink(link_path)
        client.expect(['connect', b.npub, '[fd00::1]:2121', 'udp'], 1, a.socket, err='is a FIPS mesh address (fd00::/8)')
        client.expect(['connect', b.npub, f'127.0.0.1:{port_b}', 'bogus'], 1, a.socket, err='error: ')
        client.expect(['disconnect', b.npub], 1, a.socket, err='error: ')
        client.expect(['path', 'show', b.npub], 1, a.socket, err='error: ')
        client.expect(['path', 'unpin', 'no-such-host'], 1, a.socket, err="unknown host 'no-such-host'")
        client.expect(['stats', 'history', 'srtt_ms', '--peer', b.npub], 1, a.socket, err='error: ')
        # An address nobody announced: the bloom stage fails at once and the block
        # ends there. The first miss starts the daemon's lookup backoff, so a
        # probe that follows it is held down instead; both are failures the
        # client renders from the daemon's reason, so both are accepted here.
        unknown = probe_run(client, a, ['probe', npub])
        if unknown.returncode != 1 or 'bloom       failed' not in unknown.stdout or 'no peer filter holds this address' not in unknown.stdout:
            raise Failure(f'probe of an unknown address:\n{unknown.stdout}{unknown.stderr}')
        if '\x1b[' in unknown.stdout:
            raise Failure('escape sequences off a terminal')
        client.table.append(('probe <unknown npub>', 1, 'bloom_miss'))
        report = check_probe(client, a, fresh_npub(client), 'failed')
        if report['bloom']['reason'] not in ('bloom_miss', 'backoff_suppressed'):
            raise Failure(f'unknown-address probe reason {report["bloom"]}')
        # Peer the two daemons and run the live flow.
        link(client, a, b, time.monotonic() + args.link_timeout)
        linked = query_round(client, a, 'linked')
        if linked['show status'][0]['peer_count'] != 1 or len(linked['show peers'][0]['peers']) != 1:
            raise Failure('peer counts after the link')
        shown = client.expect(['path', 'show', b.npub], 0, a.socket, parse=True, note='linked')
        if not shown['enabled'] or len(shown['paths']) < 1 or shown['pin'] is not None:
            raise Failure(f'path show: {shown}')
        client.expect(['path', 'pin', b.npub, 'udp'], 0, a.socket, note='pin a transport')
        pinned = client.expect(['path', 'show', b.npub], 0, a.socket, parse=True)
        if pinned['pin'] is None:
            raise Failure(f'pin not shown: {pinned}')
        path = shown['paths'][0]
        # One path: the transport by the numeric id `path show` prints, or by
        # its configured name, plus the path's address.
        client.expect(['path', 'pin', b.npub, str(path['transport_id']), path['addr']], 0, a.socket, note='pin one path')
        client.expect(['path', 'pin', b.npub, path['transport'], path['addr']], 0, a.socket, note='pin one path by name')
        client.expect(['path', 'pin', b.npub, 'no-such-transport'], 1, a.socket, err='error: ')
        client.expect(['path', 'unpin', b.npub], 0, a.socket)
        if client.expect(['path', 'show', b.npub], 0, a.socket, parse=True)['pin'] is not None:
            raise Failure('pin survived unpin')

        def tracked():
            peers = a.query('show_stats_peers')['peers']
            if not any(p.get('npub') == b.npub for p in peers):
                raise NotYet(peers)
        wait([a, b], 'peer tracked in stats history', time.monotonic() + args.link_timeout, tracked)
        client.expect(['stats', 'history', 'srtt_ms', '--peer', b.npub], 0, a.socket, parse=True)
        client.expect(['stats', 'history', 'loss_rate', '--peer', b.npub, '--plot'], 0, a.socket)
        for node, target in ((a, b.npub), (b, a.npub)):
            check_probe(client, node, target)
        client.expect(['disconnect', b.npub], 0, a.socket, parse=True, stall_ok=True)

        def unlinked():
            links = a.query('show_links')['links']
            if links:
                raise NotYet(links)
        wait([a, b], 'link torn down', time.monotonic() + args.link_timeout, unlinked)
        passed = True
    except Failure as e:
        fail_with_state(e, (a, b))
    finally:
        failures = []
        for node in (a, b):
            rc = node.daemon.stop(20)
            if rc != 0 or node.daemon.sanitizer_report():
                failures.append(f'{node.name}: exit {rc}\n{node.daemon.text()[-4000:]}')
        if failures and passed:
            raise Failure('\n'.join(failures))
        if failures:  # the failure in flight is the cause; this is what the daemons said at the end
            print('daemons at shutdown:\n' + '\n'.join(failures), file=sys.stderr)
    print('\nsubcommand                                   rc  note')
    for label, rc, note in client.table:
        print(f'{label[:44]:<44} {rc:>2}  {note}')
    print(f'\nfipsctl e2e: {len(client.table)} invocations against two fipsd daemons passed')
    return 0


if __name__ == '__main__':
    try:
        sys.exit(main())
    except Failure as e:
        print(f'FAIL: {e}', file=sys.stderr)
        sys.exit(1)
