"""Daemons inside namespaces and the control-socket observations scenarios share.

Reuses fipsd/tests/udp_interop.py: `Daemon` (continuous merged log capture,
SIGTERM/kill stop, sanitizer markers), `control` (line-delimited JSON over the
Unix control socket, which path-based sockets reach across namespaces),
`NativeFlow` (the Rust native datagram API) and its NotYet/Failure wait
semantics. Every wait here is a deadline-bounded observation of a control
socket or a captured log; the only pause is the poll interval.
"""
import json
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / 'fipsd' / 'tests'))
from udp_interop import POLL, ControlError, Daemon, Failure, NativeFlow, Node, NotYet  # noqa: E402

HEALTHY = {'c': {'active', 'established', 'maintaining'}, 'rust': {'connected'}}
READY = {'c': 'fipsd running', 'rust': 'control socket listening'}
BECH32 = 'qpzry9x8gf2tvdw0s3jn54khce6mua7l'


def npub_hex(npub):
    """The x-only public key hex behind an npub (bech32, no checksum verification needed here)."""
    pos = npub.rfind('1')
    acc = bits = 0
    out = bytearray()
    for ch in npub[pos + 1:-6]:
        acc = (acc << 5) | BECH32.index(ch)
        bits += 5
        while bits >= 8:
            bits -= 8
            out.append((acc >> bits) & 0xff)
    return out.hex()


class Timeouts:
    """Observation bounds in seconds; `scale` stretches all of them (slow or sanitized hosts)."""
    DEFAULTS = dict(startup=30, link=60, tree=60, session=45, probe=60, datagram=30, dead=90,
                    reconnect=180, nat=120, session_rekey=90, link_rekey=180, shutdown=25)

    def __init__(self, scale=1.0):
        for key, value in self.DEFAULTS.items():
            setattr(self, key, value * scale)


class MeshNode(Node):
    def __init__(self, mesh, name, kind, ns, bind_ip, port):
        super().__init__(name, None, mesh.workdir / f'{mesh.tag}-{name}.sock')
        self.mesh, self.kind, self.ns, self.bind_ip, self.port = mesh, kind, ns, bind_ip, port
        self.api_path = mesh.workdir / f'{mesh.tag}-{name}-api.sock' if kind == 'rust' else None
        self.config_path = mesh.workdir / f'{mesh.tag}-{name}.yaml'
        self.peers = []      # (MeshNode, 'ip:port' or 'nat')
        self.nostr = None    # node.rendezvous.nostr mapping, when enabled
        self.rekey_after = None
        self.advertise = False
        self.pubkey = None
        self.started_at = None

    @property
    def healthy_states(self):
        return HEALTHY[self.kind]

    def query(self, command, params=None):
        try:
            return super().query(command, params)
        except TimeoutError as e:
            self.mesh.control_stalls.append(f'{self.name} {command}')
            raise ControlError(f'{self.name} {command}: no reply within the control timeout ({e})') from e

    def status(self):
        return self.query('show_status')

    def links(self):
        return self.query('show_links')['links']

    def healthy_links(self):
        return [l for l in self.links() if l['state'] in self.healthy_states]

    def peer_rows(self):
        return self.query('show_peers')['peers']

    def peer_row(self, other):
        for row in self.peer_rows():
            if row.get('npub') == other.npub:
                return row
        return None

    def sessions(self):
        return self.query('show_sessions')['sessions']

    def transport_mtus(self):
        return [t['mtu'] for t in self.query('show_transports')['transports']]

    def __repr__(self):
        return f'{self.name}({self.kind})'


class Mesh:
    def __init__(self, ctx, tag):
        self.ctx = ctx
        self.tag = tag
        self.workdir = ctx.workdir
        self.t = ctx.timeouts
        self.note = ctx.note
        self.nodes = []
        self.sent = {}  # (rust, c) -> payloads delivered over the native API
        self.probe_teardowns = 0
        self.control_stalls = []  # 'node command' for every control query that got no reply in time

    # -- definition and startup ------------------------------------------------
    def define(self, name, kind, ns, bind_ip, port=2121, peers=(), nostr=None, rekey_after=None, advertise=False, native=False):
        if kind == 'rust' and not self.ctx.rust:
            raise Failure('scenario variant needs the Rust daemon but none was supplied')
        node = MeshNode(self, name, kind, ns, bind_ip, port)
        if native:
            node.api_path = self.workdir / f'{self.tag}-{name}-api.sock'
        node.peers, node.nostr, node.rekey_after, node.advertise = list(peers), nostr, rekey_after, advertise
        self.nodes.append(node)
        return node

    def config_yaml(self, node):
        index = self.nodes.index(node) + 1
        nsec = str(index) * 64
        lines = ['node:', f"  identity: {{nsec: '{nsec}', persistent: false}}",
                 f"  control: {{enabled: true, socket_path: '{node.control}'}}"]
        if node.api_path:
            lines.append(f"  native_api: {{enabled: true, socket_path: '{node.api_path}'}}")
        if node.rekey_after:
            lines.append(f'  rekey: {{after_secs: {node.rekey_after}}}')
        if node.nostr:
            lines += ['  rendezvous:', '    nostr:']
            for key, value in node.nostr.items():
                lines.append(f'      {key}: {json.dumps(value)}')
        lines += ['tun: {enabled: false}', 'dns: {enabled: false}', 'transports:',
                  f"  udp: {{bind_addr: '{node.bind_ip}:{node.port}', advertise_on_nostr: {str(node.advertise).lower()}, public: false}}"]
        if node.peers:
            lines.append('peers:')
            for peer, addr in node.peers:
                if not peer.npub:
                    raise Failure(f'{node.name} is configured with peer {peer.name}, which must be started first (its npub is read from show_status)')
                lines += [f"  - npub: '{peer.npub}'", f"    alias: '{peer.name}'", '    addresses:',
                          f"      - transport: udp", f"        addr: '{addr}'",
                          '    connect_policy: auto_connect', '    auto_reconnect: true']
                if addr == 'nat':
                    lines.append('    via_nostr: true')
        return '\n'.join(lines) + '\n'

    def start(self, *nodes):
        deadline = time.monotonic() + self.t.startup
        for node in nodes:
            node.config_path.write_text(self.config_yaml(node))
            binary = self.ctx.fipsd if node.kind == 'c' else self.ctx.rust
            node.daemon = Daemon(node.name, node.ns.exec_argv([binary, '-c', str(node.config_path)]),
                                 self.ctx.env, str(self.workdir))
            node.started_at = time.monotonic()
        for node in nodes:
            node.daemon.await_log(READY[node.kind], deadline)
            status = self.wait(f'{node.name} show_status', deadline, node.status)
            node.npub, node.node_addr = status['npub'], status['node_addr']
            node.pubkey = npub_hex(node.npub)
            self.note(f'{node.name} ({node.kind}, netns {node.ns.name}, udp {node.bind_ip}:{node.port}): '
                      f'npub={node.npub} node_addr={node.node_addr}')

    # -- waits ------------------------------------------------------------------
    def check_alive(self):
        for node in self.nodes:
            if node.daemon and not node.daemon.alive():
                raise Failure(f'{node.name} exited with {node.daemon.proc.returncode} during the scenario')

    def wait(self, what, deadline, predicate):
        """Poll `predicate` until it returns a value; NotYet/ControlError mean keep waiting.

        `deadline` is absolute (monotonic). Returns the predicate's value and
        notes how long the observation took.
        """
        started, last = time.monotonic(), None
        while True:
            self.check_alive()
            try:
                value = predicate()
                self.note(f'ok ({time.monotonic() - started:.1f}s): {what}')
                return value
            except NotYet as e:
                last = e.args[0] if e.args else None
            except ControlError as e:
                last = str(e)
            if time.monotonic() >= deadline:
                raise Failure(f'timed out after {time.monotonic() - started:.1f}s waiting for {what}; '
                              f'last observation: {json.dumps(last, default=str)[:2000]}')
            time.sleep(POLL)

    def after(self, seconds):
        return time.monotonic() + seconds

    # -- links and tree -----------------------------------------------------------
    def links_state(self, node, peers, remote=None):
        """Raise NotYet unless `node` holds exactly a healthy link per peer in `peers`."""
        rows = node.peer_rows()
        seen = {'peers': [(r.get('npub'), r.get('connectivity')) for r in rows], 'links': node.links()}
        for peer in peers:
            row = next((r for r in rows if r.get('npub') == peer.npub), None)
            if row is None or (node.kind == 'c' and row.get('connectivity') != 'connected'):
                raise NotYet({node.name: seen})
        healthy = [l for l in seen['links'] if l['state'] in node.healthy_states]
        if len(healthy) != len(peers) or len(seen['links']) != len(peers):
            raise NotYet({node.name: seen})
        if remote:
            # A value ending in ':' is an address prefix: any port (the Rust
            # daemon punches and dials from a per-attempt socket, not :2121).
            match = (lambda a: a.startswith(remote)) if remote.endswith(':') else (lambda a: a == remote)
            if not any(match(l['remote_addr']) for l in healthy):
                raise NotYet({node.name: ('no healthy link to', remote, seen)})
        status = node.status()
        if status['link_count'] != len(peers) or status['peer_count'] != len(peers):
            raise NotYet({node.name: {'status': {k: status[k] for k in ('peer_count', 'link_count')}}})
        return healthy

    def wait_links(self, expect, timeout, remote=None):
        """`expect`: node -> peers it must hold healthy links to; `remote`: (node, peer) -> remote_addr."""
        def all_linked():
            out = {}
            for node, peers in expect.items():
                want = None
                if remote:
                    want = next((addr for (n, p), addr in remote.items() if n is node), None)
                out[node.name] = self.links_state(node, peers, want)
            return out

        what = 'links: ' + ', '.join(f'{n.name}<->{{{",".join(p.name for p in ps)}}}' for n, ps in expect.items())
        return self.wait(what, self.after(timeout), all_linked)

    def wait_links_gone(self, node, peer, remaining, timeout):
        def gone():
            rows = node.peer_rows()
            if any(r.get('npub') == peer.npub for r in rows):
                raise NotYet({node.name: [(r.get('npub'), r.get('connectivity')) for r in rows]})
            status = node.status()
            if status['link_count'] != remaining or status['peer_count'] != remaining:
                raise NotYet({node.name: {k: status[k] for k in ('peer_count', 'link_count')}})
            return status

        return self.wait(f'{node.name} no longer holds {peer.name} (links left: {remaining})', self.after(timeout), gone)

    def wait_tree(self, degree, timeout):
        """`degree`: node -> number of tree peers it must report; all must share one root with consistent depths."""
        def converged():
            trees = {n: n.query('show_tree') for n in degree}
            roots = {t['root'] for t in trees.values()}
            brief = {n.name: {k: t.get(k) for k in ('root', 'is_root', 'depth', 'parent', 'peer_tree_count')} for n, t in trees.items()}
            if len(roots) != 1 or any(t['peer_tree_count'] != d for (n, d), t in zip(degree.items(), trees.values())):
                raise NotYet(brief)
            root_nodes = [n for n, t in trees.items() if t['is_root']]
            if len(root_nodes) != 1 or trees[root_nodes[0]]['depth'] != 0:
                raise NotYet(brief)
            by_addr = {t['my_node_addr']: t for t in trees.values()}
            for n, t in trees.items():
                if n is root_nodes[0]:
                    continue
                parent = by_addr.get(t['parent'])
                if parent is None or t['depth'] != parent['depth'] + 1:
                    raise NotYet(brief)
            return trees, root_nodes[0]

        trees, root = self.wait('spanning tree converged across ' + ','.join(n.name for n in degree), self.after(timeout), converged)
        self.note('tree: root=%s; %s' % (root.name, '; '.join(f'{n.name} depth={t["depth"]} coords={t["my_coords"]}' for n, t in trees.items())))
        return trees

    def wait_bloom(self, expect, timeout):
        """`expect`: node -> {peer: minimum estimated element count of the filter held from that peer}.

        A filter arrives first with only the sender in it and is re-announced,
        merged with the sender's other tree peers, later; a lookup through a
        peer is only possible once that merged filter (count >= 2) has
        crossed, so the count is the observation, not merely `has_filter`.
        Rows without an estimate (the Rust daemon's view) count as 1 element.
        """
        def exchanged():
            blooms = {n.name: n.query('show_bloom')['peer_filters'] for n in expect}
            brief = {name: [(f.get('peer') or f.get('node_addr'), f.get('has_filter'), f.get('estimated_count')) for f in rows]
                     for name, rows in blooms.items()}
            for node, peers in expect.items():
                for peer, minimum in peers.items():
                    row = next((f for f in blooms[node.name] if (f.get('peer') or f.get('node_addr')) == peer.node_addr), None)
                    if row is None or not row.get('has_filter'):
                        raise NotYet(brief)
                    if row.get('estimated_count', 1) < minimum:
                        raise NotYet(brief)
            return blooms

        what = 'bloom filters: ' + ', '.join(f'{n.name}<-{{{",".join(f"{p.name}>={m}" for p, m in ps.items())}}}' for n, ps in expect.items())
        return self.wait(what, self.after(timeout), exchanged)

    # -- sessions, probes and datagrams ------------------------------------------
    def probe(self, prober, target, timeout):
        started = prober.query('probe_start', {'npub': target.npub})
        probe_id = started['probe_id']

        def done():
            poll = prober.query('probe_poll', {'probe_id': probe_id})
            if poll['state'] != 'done':
                raise NotYet({'state': poll['state'], 'overall': (poll.get('report') or {}).get('overall')})
            return poll['report']

        report = self.wait(f'{prober.name} probe {probe_id} -> {target.name} finished', self.after(timeout), done)
        self.note(f'{prober.name} probe report: {json.dumps(report)}')
        if report['overall'] != 'ok':
            raise Failure(f'{prober.name} probe -> {target.name} overall={report["overall"]}: {json.dumps(report)}')
        if report['rtt']['samples'] < 1 or report['rtt']['reports_seen'] < 1:
            raise Failure(f'{prober.name} probe -> {target.name} measured no RTT: {json.dumps(report["rtt"])}')
        if report['cleanup'].get('session_created_and_torn_down'):
            self.probe_teardowns += 1
        return report

    def open_flow(self, rust, c, remote_port=4242):
        flow = NativeFlow(rust.api_path, c.npub, remote_port)
        self.note(f'{rust.name} native connect -> {c.name}: {json.dumps(flow.data)}')
        return flow

    def send_native(self, flow, rust, c, payload, what):
        history = self.sent.setdefault((rust.name, c.name), [])
        flow.send(payload)
        history.append(payload)
        want_dgrams, want_bytes = len(history), sum(map(len, history))

        def delivered():
            now = c.query('show_metrics')['native']
            if now['received_datagrams'] < want_dgrams:
                raise NotYet(now)
            if now['received_datagrams'] != want_dgrams or now['received_bytes'] != want_bytes:
                raise Failure(f'{c.name} native counters {now} do not match {want_dgrams} datagram(s)/{want_bytes} byte(s) sent')
            return now

        return self.wait(f'{what}: {c.name} received {len(payload)} bytes over the native API', self.after(self.t.datagram), delivered)

    def session_established(self, a, b, initiator):
        rows = {}
        for node, other in ((a, b), (b, a)):
            s = node.sessions()
            rows[node.name] = s
            if len(s) != 1 or s[0]['state'] != 'established' or s[0].get('identity') != 'proven':
                raise NotYet(rows)
            if s[0].get('npub') != other.npub:
                raise Failure(f'{node.name} session peer {s[0].get("npub")} is not {other.npub}: {s}')
            if s[0]['is_initiator'] != (node is initiator):
                raise Failure(f'{node.name} is_initiator={s[0]["is_initiator"]} but {initiator.name} opened the session: {s}')
        return rows

    def wait_session(self, a, b, initiator, timeout):
        rows = self.wait(f'XK session {a.name}<->{b.name} established on both sides ({initiator.name} initiator)',
                         self.after(timeout), lambda: self.session_established(a, b, initiator))
        for name, s in rows.items():
            self.note(f'{name} show_sessions: {json.dumps(s[0])}')
        return rows

    def session_k_bits(self, a, b):
        return {n.name: n.sessions()[0]['current_k_bit'] for n in (a, b)}

    def link_k_bits(self, a, b):
        return {n.name: n.peer_row(other)['current_k_bit'] for n, other in ((a, b), (b, a))}

    def wait_session_rekey(self, a, b, initial, deadline):
        def flipped():
            rows = {n.name: n.sessions() for n in (a, b)}
            for name, s in rows.items():
                if len(s) != 1:
                    raise Failure(f'{name} holds {len(s)} sessions during rekey: {s}')
                if s[0]['state'] != 'established' or s[0]['current_k_bit'] == initial[name]:
                    raise NotYet({k: (v[0]['state'], v[0]['current_k_bit']) for k, v in rows.items()})
            return rows

        return self.wait('FSP session rekey: K bit flipped on both sides', deadline, flipped)

    def wait_link_rekey(self, a, b, initial, deadline):
        def flipped():
            rows = {n.name: n.peer_row(other) for n, other in ((a, b), (b, a))}
            for name, row in rows.items():
                if row is None:
                    raise Failure(f'{name} lost its peer during the link rekey wait')
                if row['current_k_bit'] == initial[name]:
                    raise NotYet({k: (v['current_k_bit'], v.get('rekey_in_progress')) for k, v in rows.items()})
            return rows

        return self.wait('FMP link rekey: K bit flipped on both sides', deadline, flipped)

    def protocol_counters(self, node):
        """Session/handshake counters that indicate corrupted or replayed frames (nonzero is a failure).

        A probe that tore its own session down removes it locally only (as
        upstream probe_teardown does), so the peer keeps reporting into it
        until its idle timeout and the prober counts those frames as
        `session.unknown_session`; after such a teardown that counter is
        expected and reported, not failed.
        """
        metrics = node.query('show_metrics')
        flat = {}
        for group in ('session', 'handshake'):
            for k, v in metrics.get(group, {}).items():
                flat[f'{group}.{k}'] = v
        self.note(f'{node.name} counters: {json.dumps(flat)}')
        expected = {'session.unknown_session'} if self.probe_teardowns else set()
        return {k: v for k, v in flat.items() if v and k not in expected
                and any(w in k for w in ('mismatch', 'decrypt', 'bad_state', 'unknown', 'replay'))}

    # -- shutdown and evidence ---------------------------------------------------
    def stop_all(self):
        failures = []
        for node in self.nodes:
            if not node.daemon:
                continue
            code = node.daemon.stop(self.t.shutdown)
            if node.daemon.forced:
                failures.append(f'{node.name} did not exit on SIGTERM within {self.t.shutdown}s (killed)')
            elif code != 0:
                failures.append(f'{node.name} exited {code}')
            if node.daemon.sanitizer_report():
                failures.append(f'{node.name} log carries a sanitizer report')
            if node.control.exists():
                failures.append(f'{node.name} left its control socket behind')
            if node.api_path and node.api_path.exists():
                failures.append(f'{node.name} left its native API socket behind')
        return failures

    SNAPSHOT_QUERIES = ('show_status', 'show_peers', 'show_links', 'show_tree', 'show_bloom', 'show_routing',
                        'show_cache', 'show_sessions', 'show_transports', 'show_metrics')

    def snapshot(self, node):
        """Every read-only control view of a live daemon, for the failure evidence."""
        out = {}
        for command in self.SNAPSHOT_QUERIES:
            try:
                out[command] = node.query(command)
            except Exception as e:  # noqa: BLE001 - a view that fails is itself evidence
                out[command] = f'{type(e).__name__}: {e}'
        return out

    def dump(self, out_dir):
        out_dir.mkdir(parents=True, exist_ok=True)
        for node in self.nodes:
            if node.config_path.exists():
                (out_dir / f'{node.name}.yaml').write_text(node.config_path.read_text())
            if node.daemon:
                (out_dir / f'{node.name}.log').write_text(node.daemon.text())
                if node.daemon.alive() and node.npub:
                    (out_dir / f'{node.name}.control.json').write_text(json.dumps(self.snapshot(node), indent=1) + '\n')
