#!/usr/bin/env python3
"""The cross-implementation gate for fipsctl: the C fipsctl against the pinned
Rust daemon and the pinned Rust fipsctl against fipsd, every documented
subcommand, with the two clients' outputs compared byte for byte wherever
the daemon's state is not itself changing between the two reads. SKIP (77)
without the Rust daemon (FIPSD_UDP_INTEROP_RUST_BINARY) or a Rust fipsctl
(FIPSD_FIPSCTL_EXECUTABLE, or `cargo build --locked --release --bin fipsctl`
from FIPSD_UPSTREAM_CONTROL_DIR into --cargo-target)."""
import argparse
import json
import os
from pathlib import Path
import re
import shutil
import socket
import subprocess
import sys
import time

sys.path.insert(0, str(Path(__file__).resolve().parent))
import fipsctl_e2e as e2e  # noqa: E402
from fipsctl_e2e import Client, Failure, Node, check_probe, link, local_commands, pretty, shape, wait  # noqa: E402
from udp_interop import Daemon, reserve_port  # noqa: E402

# Durations and timing figures differ between two runs of the same probe, and
# the target block names the key each run was given.
TIMING = re.compile(r'\b\d+(\.\d+)?(s| ms)\b')
TARGET = re.compile(r'^(probe |  npub       |  node_addr  |  ipv6       ).*$', re.M)


def rust_config(root, name, socket_path, port, nsec):
    path = root / f'{name}.yaml'
    path.write_text(f'''
node:
  identity: {{nsec: '{nsec}', persistent: false}}
  control: {{enabled: true, socket_path: '{socket_path}'}}
  native_api: {{enabled: false}}
  path: {{enabled: true}}
tun: {{enabled: false}}
dns: {{enabled: false}}
transports:
  udp: {{bind_addr: '127.0.0.1:{port}'}}
''')
    return path


class Comparison:
    """Both clients on one daemon, output by output."""

    def __init__(self):
        self.identical = []
        self.equivalent = []
        self.rows = []  # (daemon, subcommand, c rc, rust rc, verdict)

    def compare(self, daemon, c, r, args, parse_json=True, expect_rc=None):
        label = ' '.join(a if not a.startswith('npub1') else a[:12] + '...' for a in args)
        rc_out = r.run(args, daemon.socket)
        cc_out = c.run(args, daemon.socket)
        if rc_out.returncode != cc_out.returncode:
            raise Failure(f'[{daemon.name}] {label}: C exit {cc_out.returncode}, Rust exit {rc_out.returncode}\n'
                          f'C: {cc_out.stdout}{cc_out.stderr}\nRust: {rc_out.stdout}{rc_out.stderr}')
        if expect_rc is not None and cc_out.returncode != expect_rc:
            raise Failure(f'[{daemon.name}] {label}: exit {cc_out.returncode}, expected {expect_rc}: {cc_out.stderr}')
        verdict = 'identical'
        if cc_out.stdout == rc_out.stdout and cc_out.stderr == rc_out.stderr:
            self.identical.append(label)
        elif parse_json and cc_out.returncode == 0:
            c_data, r_data = json.loads(cc_out.stdout), json.loads(rc_out.stdout)
            if shape(c_data) != shape(r_data):
                raise Failure(f'[{daemon.name}] {label}: the clients disagree beyond live values\n'
                              f'C: {cc_out.stdout}\nRust: {rc_out.stdout}')
            if cc_out.stdout != pretty(c_data) and not re.search(r'\d\.\d|e[+-]\d', cc_out.stdout):
                raise Failure(f'[{daemon.name}] {label}: C output is not serde-pretty:\n{cc_out.stdout}')
            verdict = 'equivalent (live values differ)'
            self.equivalent.append(label)
        elif cc_out.returncode == 1 and cc_out.stderr.split(':')[:2] == rc_out.stderr.split(':')[:2]:
            verdict = 'equivalent (error detail differs)'
            self.equivalent.append(label)
        else:
            raise Failure(f'[{daemon.name}] {label}: outputs differ\nC: {cc_out.stdout}{cc_out.stderr}\n'
                          f'Rust: {rc_out.stdout}{rc_out.stderr}')
        self.rows.append((daemon.name, label, cc_out.returncode, rc_out.returncode, verdict))
        return cc_out, rc_out

    def compare_probe_text(self, daemon, c, r, fresh, label):
        """Probes of unannounced targets through both clients: identical rendering
        once timing and the target block are masked. The daemon's first miss
        starts its lookup backoff and the next probe is held down instead, so
        the Rust client's probe is repeated until both clients saw the daemon
        in the same state; three probes are enough for the backoff to be in
        force for both."""
        mask = lambda text: TARGET.sub(r'\1<target>', TIMING.sub('<t>', text))  # noqa: E731
        rc_out = r.run(['probe', fresh(r)], daemon.socket)
        cc_out = c.run(['probe', fresh(c)], daemon.socket)
        for _ in range(3):
            if mask(cc_out.stdout) == mask(rc_out.stdout) and cc_out.stderr == rc_out.stderr and cc_out.returncode == rc_out.returncode:
                break
            rc_out = r.run(['probe', fresh(r)], daemon.socket)
        else:
            raise Failure(f'[{daemon.name}] probe {label}: renderings differ\nC ({cc_out.returncode}):\n{cc_out.stdout}'
                          f'{cc_out.stderr}\nRust ({rc_out.returncode}):\n{rc_out.stdout}{rc_out.stderr}')
        self.identical.append(f'probe {label} (timing masked)')
        self.rows.append((daemon.name, f'probe {label}', cc_out.returncode, rc_out.returncode, 'identical (timing masked)'))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--fipsd', required=True)
    ap.add_argument('--fipsctl', required=True, help='the built C fipsctl')
    ap.add_argument('--rust', default=os.environ.get('FIPSD_UDP_INTEROP_RUST_BINARY', ''))
    ap.add_argument('--upstream', default='')
    ap.add_argument('--rust-fipsctl', default='')
    ap.add_argument('--cargo-target', default='')
    ap.add_argument('--startup-timeout', type=float, default=30)
    ap.add_argument('--link-timeout', type=float, default=60)
    args = ap.parse_args()
    args.rust = args.rust or os.environ.get('FIPSD_UDP_INTEROP_RUST_BINARY', '')
    if not args.rust or not Path(args.rust).is_file():
        print('SKIP: set FIPSD_UDP_INTEROP_RUST_BINARY to a fips daemon built from the libfips-core/UPSTREAM.md pin')
        return 77
    root = Path(os.environ['FIPSD_TEST_TMPDIR'])
    env = dict(os.environ, FIPSD_WORKERS='2', TOKIO_WORKER_THREADS='2', NO_COLOR='1')
    env.setdefault('RUST_LOG', 'info')
    rust_client = args.rust_fipsctl or os.environ.get('FIPSD_FIPSCTL_EXECUTABLE', '')
    if not rust_client:
        cargo = shutil.which('cargo')
        manifest = Path(args.upstream) / 'Cargo.toml' if args.upstream else None
        if not cargo or not manifest or not manifest.is_file() or not args.cargo_target:
            print('SKIP: set FIPSD_FIPSCTL_EXECUTABLE, or FIPSD_UPSTREAM_CONTROL_DIR for a cargo build of the pinned fipsctl')
            return 77
        subprocess.run([cargo, 'build', '--locked', '--release', '--bin', 'fipsctl', '--manifest-path', str(manifest)],
                       env=dict(env, CARGO_TARGET_DIR=args.cargo_target), cwd=root, check=True)
        rust_client = str(Path(args.cargo_target) / 'release' / 'fipsctl')
    if not Path(rust_client).is_file():
        print(f'SKIP: no Rust fipsctl at {rust_client}')
        return 77
    c, r = Client('C fipsctl', args.fipsctl, env), Client('Rust fipsctl', rust_client, env)
    # Local commands through both; a key one generated must be read by the other.
    keys_c, keys_r = root / 'kc', root / 'kr'
    npub, _ = local_commands(c, root)
    c.expect(['keygen', '-d', str(keys_c)], 0)
    r.expect(['keygen', '-d', str(keys_r)], 0)
    for key in ('fips.key', 'fips.pub'):
        if c.expect(['address', '-k', str(keys_r / key)], 0).stdout != r.expect(['address', '-k', str(keys_r / key)], 0).stdout:
            raise Failure(f'the clients derive different addresses from the Rust keygen {key}')
        if c.expect(['address', '-k', str(keys_c / key)], 0).stdout != r.expect(['address', '-k', str(keys_c / key)], 0).stdout:
            raise Failure(f'the clients derive different addresses from the C keygen {key}')
    if c.expect(['address', npub], 0).stdout != r.expect(['address', npub], 0).stdout:
        raise Failure('address <npub> differs')
    usage_c, usage_r = c.run(['bogus']).returncode, r.run(['bogus']).returncode
    print(f'usage error exit codes: C {usage_c} (documented), Rust {usage_r} (clap)')
    port_c, port_r = reserve_port(socket.AF_INET), reserve_port(socket.AF_INET)
    fipsd = e2e.start_fipsd('fipsd', args.fipsd, root, env, port_c, '1' * 64)
    rsock = root / 'rrust.sock'
    rust = Node('rust', Daemon('rust', [args.rust, '-c', str(rust_config(root, 'rrust', rsock, port_r, '2' * 64))], env, root), rsock, port_r)
    cmp = Comparison()
    try:
        deadline = time.monotonic() + args.startup_timeout
        fipsd.daemon.await_log('fipsd running', deadline)
        rust.daemon.await_log('control socket listening', deadline)
        for node in (fipsd, rust):
            status = wait([node], f'{node.name} show_status', deadline, lambda n=node: n.query('show_status'))
            node.npub, node.node_addr = status['npub'], status['node_addr']
        queries = [q[0] for q in e2e.QUERIES] + [['stats', 'history', 'mesh_size', '--plot'], ['stats', 'history', 'bogus'],
                                                   ['stats', 'history', 'srtt_ms'], ['path', 'show', 'npub1qq8s4f7x'],
                                                   ['disconnect', 'npub1qq8s4f7x'], ['connect', 'npub1qq8s4f7x', '[fd00::1]:2121', 'udp'],
                                                   ['connect', 'npub1qq8s4f7x', '127.0.0.1:1', 'bogus']]
        for daemon in (fipsd, rust):
            for q in queries:
                cmp.compare(daemon, c, r, q)
            # Rust fipsctl against fipsd and C fipsctl against the Rust daemon, the documented way.
            for client in (c, r):
                e2e.query_round(client, daemon, f'{client.name} vs {daemon.name}')
            cmp.compare_probe_text(daemon, c, r, e2e.fresh_npub, 'of an unannounced address')
        # Peer the daemons through the C client against fipsd, then the live flow through both clients.
        link(c, fipsd, rust, time.monotonic() + args.link_timeout, ('active', 'connected'))
        for daemon, peer in ((fipsd, rust), (rust, fipsd)):
            for q in [a for a in queries if a[0] in ('show', 'acl', 'stats') and 'bogus' not in a and 'srtt_ms' not in a]:
                cmp.compare(daemon, c, r, q)
            cmp.compare(daemon, c, r, ['path', 'show', peer.npub], expect_rc=0)
            shown = c.expect(['path', 'show', peer.npub], 0, daemon.socket, parse=True)
            path = shown['paths'][0]
            for client in (c, r):
                # The transport by the numeric id `path show` prints, then one path
                # on it. An instance name resolves a pin only where the daemon gave
                # the transport one: upstream reports null for a bare
                # `transports.udp` and takes the id; fipsd names it by its type.
                client.expect(['path', 'pin', peer.npub, str(path['transport_id'])], 0, daemon.socket, note=f'vs {daemon.name}')
                pinned = client.expect(['path', 'show', peer.npub], 0, daemon.socket, parse=True)
                if pinned['pin'] is None:
                    raise Failure(f'{client.name} pin not shown by {daemon.name}: {pinned}')
                client.expect(['path', 'pin', peer.npub, str(path['transport_id']), path['addr']], 0, daemon.socket, note=f'vs {daemon.name}')
                if path['transport']:
                    client.expect(['path', 'pin', peer.npub, path['transport']], 0, daemon.socket, note=f'by name vs {daemon.name}')
                    client.expect(['path', 'pin', peer.npub, path['transport'], path['addr']], 0, daemon.socket, note=f'by name vs {daemon.name}')
                client.expect(['path', 'unpin', peer.npub], 0, daemon.socket, note=f'vs {daemon.name}')
                if client.expect(['path', 'show', peer.npub], 0, daemon.socket, parse=True)['pin'] is not None:
                    raise Failure(f'{client.name}: pin survived unpin on {daemon.name}')
            cmp.compare(daemon, c, r, ['path', 'pin', peer.npub, 'no-such-transport'], expect_rc=1)

            def tracked(d=daemon, p=peer):
                peers = d.query('show_stats_peers')['peers']
                if not any(x.get('npub') == p.npub for x in peers):
                    raise e2e.NotYet(peers)
            wait([fipsd, rust], f'{peer.name} tracked in {daemon.name} stats history', time.monotonic() + args.link_timeout, tracked)
            cmp.compare(daemon, c, r, ['stats', 'history', 'srtt_ms', '--peer', peer.npub], expect_rc=0)
            cmp.compare(daemon, c, r, ['stats', 'history', 'bytes_in', '--peer', peer.npub, '--plot'], parse_json=False, expect_rc=0)
            for client in (c, r):
                check_probe(client, daemon, peer.npub)
        c.expect(['disconnect', rust.npub], 0, fipsd.socket, parse=True)

        def unlinked():
            links = fipsd.query('show_links')['links']
            if links:
                raise e2e.NotYet(links)
        wait([fipsd, rust], 'link torn down', time.monotonic() + args.link_timeout, unlinked)
    finally:
        failures = []
        for node in (fipsd, rust):
            rc = node.daemon.stop(20)
            if rc != 0 or node.daemon.sanitizer_report():
                failures.append(f'{node.name}: exit {rc}\n{node.daemon.text()[-4000:]}')
        if failures:
            raise Failure('\n'.join(failures))
    print('\ndaemon  subcommand                                    C  Rust  verdict')
    for daemon, label, crc, rrc, verdict in cmp.rows:
        print(f'{daemon:<7} {label[:44]:<44} {crc:>2} {rrc:>5}  {verdict}')
    print(f'\nC fipsctl vs Rust fipsctl: {len(cmp.identical)} byte-identical, {len(cmp.equivalent)} equivalent; '
          f'{len(c.table)} C and {len(r.table)} Rust invocations passed against both daemons')
    return 0


if __name__ == '__main__':
    try:
        sys.exit(main())
    except Failure as e:
        print(f'FAIL: {e}', file=sys.stderr)
        sys.exit(1)
