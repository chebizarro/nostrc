#!/usr/bin/env python3
"""Opt-in IK/XK/datagram proof through an operator-started live Nym client.

The requester must allow only --rust-listen, which must be reachable from it.
The harness owns the two FIPS daemons, not the Nym credentials or processes.
--nym may be an executable SSH wrapper for a client on another machine; it is
queried for its version, not started/reconfigured. Never substitute a fixture.
"""
import argparse
import json
import os
from pathlib import Path
import shutil
import signal
import subprocess
import sys
import time
import traceback

from udp_interop import Daemon, Failure, NativeFlow, Node, NotYet, watch_lifeline


def observe(what, nodes, predicate, seconds=300):
    deadline = time.monotonic() + seconds
    last = None
    while True:
        for node in nodes:
            if not node.daemon.alive():
                raise Failure(f'{node.name} exited waiting for {what}')
        try:
            return predicate()
        except NotYet as exc:
            last = exc.args
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise Failure(f'{what} timed out: {last}')
        with nodes[0].daemon.cond:
            nodes[0].daemon.cond.wait(min(0.1, remaining))


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--fipsd', required=True)
    ap.add_argument('--native-tool', required=True)
    ap.add_argument('--rust', default=os.environ.get('FIPSD_UDP_INTEROP_RUST_BINARY', ''))
    ap.add_argument('--nym', default=os.environ.get('FIPSD_NYM_BINARY', ''))
    ap.add_argument('--socks5-addr', default=os.environ.get('FIPSD_NYM_SOCKS5_ADDR', ''))
    ap.add_argument('--rust-listen', default=os.environ.get('FIPSD_NYM_RUST_LISTEN', '127.0.0.1:18443'))
    args = ap.parse_args()
    args.rust = args.rust or os.environ.get('FIPSD_UDP_INTEROP_RUST_BINARY', '')
    nym, rust = shutil.which(args.nym), shutil.which(args.rust)
    if not nym or not rust or not args.socks5_addr:
        print('SKIP: provide FIPSD_NYM_BINARY, FIPSD_UDP_INTEROP_RUST_BINARY and '
              'FIPSD_NYM_SOCKS5_ADDR (an already-running live Nym client)')
        return 77
    version = subprocess.check_output([nym, '--version'], text=True, timeout=30)
    if 'nym-socks5-client' not in version:
        raise Failure('--nym did not identify itself as nym-socks5-client')
    print(version, flush=True)
    watch_lifeline()
    root = Path(os.environ['FIPSD_TEST_TMPDIR'])
    children, flow, failed = [], None, False
    timings = {}
    began = time.monotonic()
    try:
        r_sock, r_api = root / 'r.sock', root / 'ra.sock'
        c_sock, c_api = root / 'c.sock', root / 'ca.sock'
        r_config, c_config = root / 'rust.yaml', root / 'c.yaml'
        r_config.write_text(f'''node:
  identity: {{nsec: '{'2' * 64}', persistent: false}}
  control: {{enabled: true, socket_path: '{r_sock}'}}
  native_api: {{enabled: true, socket_path: '{r_api}'}}
tun: {{enabled: false}}
dns: {{enabled: false}}
transports:
  tcp: {{bind_addr: '{args.rust_listen}'}}
''')
        env = os.environ.copy()
        env['FIPSD_WORKERS'] = '2'
        rd = Daemon('rust', [rust, '-c', str(r_config)], env, root)
        children.append(rd)
        rd.await_log('control socket listening', time.monotonic() + 60)
        rd.await_log('native api socket listening', time.monotonic() + 60)
        r = Node('rust', rd, r_sock)
        r.npub = r.query('show_status')['npub']
        c_config.write_text(f'''node:
  identity: {{nsec: '{'1' * 64}', persistent: false}}
  control: {{enabled: true, socket_path: '{c_sock}'}}
  native_api: {{enabled: true, socket_path: '{c_api}'}}
tun: {{enabled: false}}
dns: {{enabled: false}}
transports:
  nym: {{socks5_addr: '{args.socks5_addr}'}}
peers:
  - npub: '{r.npub}'
    addresses: [{{transport: nym, addr: '{args.rust_listen}'}}]
''')
        linked_at = time.monotonic()
        cd = Daemon('fipsd', [args.fipsd, '-c', str(c_config)], env, root)
        children.append(cd)
        cd.await_log('link established with', time.monotonic() + 360)
        timings['c_to_rust_ik_s'] = round(time.monotonic() - linked_at, 3)
        cd.await_log('native api socket listening', time.monotonic() + 60)
        c = Node('c', cd, c_sock)
        c.npub = c.query('show_status')['npub']
        nodes = [c, r]

        def peers():
            rows = {n.name: n.query('show_peers')['peers'] for n in nodes}
            for n, other in [(c, r), (r, c)]:
                if len(rows[n.name]) != 1 or rows[n.name][0]['npub'] != other.npub:
                    raise NotYet(rows)
            return rows

        print('IK peers:', json.dumps(observe('both IK peers', nodes, peers)), flush=True)
        row = next(t for t in c.query('show_transports')['transports'] if t['type'] == 'nym')
        assert row['stats']['connections_established'] == 1, row
        assert row['stats']['socks5_errors'] == 0 and row['stats']['connect_timeouts'] == 0, row
        print('Nym transport:', json.dumps(row), flush=True)
        # Like the UDP interop gate, wait for routing before the first write.
        def routing_ready():
            trees = [n.query('show_tree') for n in nodes]
            blooms = [n.query('show_bloom')['peer_filters'] for n in nodes]
            if (trees[0]['root'] != trees[1]['root'] or
                    sorted(t['depth'] for t in trees) != [0, 1] or
                    any(len(b) != 1 or not b[0]['has_filter'] for b in blooms)):
                raise NotYet({'trees': trees, 'blooms': blooms})
            return trees

        observe('tree and bloom convergence', nodes, routing_ready)
        timings['routing_ready_after_c_start_s'] = round(time.monotonic() - linked_at, 3)
        reply = b'fipsd -> Rust over live Nym'
        tool = Daemon('native-client', [args.native_tool, 'serve', str(c_api), '7777', reply.hex()], env, root)
        children.append(tool)
        tool.await_log('holding port 7777', time.monotonic() + 30)
        flow = NativeFlow(r_api, c.npub, 7777, timeout=300)
        payload = bytes(range(64)) + b'Rust -> fipsd over live Nym'
        sent_at = time.monotonic()
        flow.send(payload)

        def sessions():
            rows = {n.name: n.query('show_sessions')['sessions'] for n in nodes}
            for n, other in [(c, r), (r, c)]:
                s = rows[n.name]
                if len(s) != 1 or s[0]['state'] != 'established' or s[0].get('identity') != 'proven':
                    raise NotYet(rows)
                assert s[0]['npub'] == other.npub and s[0]['is_initiator'] == (n is r), rows
            return rows

        print('XK sessions:', json.dumps(observe('XK on both daemons', nodes, sessions)), flush=True)
        timings['rust_initiated_xk_s'] = round(time.monotonic() - sent_at, 3)
        tool.await_log(f'received {payload.hex()}', time.monotonic() + 300)
        timings['first_datagram_delivery_including_xk_s'] = round(time.monotonic() - sent_at, 3)
        assert flow.recv() == reply, 'C->Rust application payload mismatch'
        timings['datagram_roundtrip_including_xk_s'] = round(time.monotonic() - sent_at, 3)
        native = c.query('show_metrics')['native']
        assert native['received_datagrams'] == 1 and native['received_bytes'] == len(payload), native
        print(f'Application datagrams: Rust->C {len(payload)} bytes; C->Rust {len(reply)} bytes', flush=True)
        print('C native counters:', json.dumps(native), flush=True)
        row = next(t for t in c.query('show_transports')['transports'] if t['type'] == 'nym')
        assert row['stats']['connections_established'] == 1, row
        assert all(row['stats'][key] == 0 for key in
                   ['socks5_errors', 'connect_timeouts', 'send_errors', 'recv_errors', 'mtu_exceeded']), row
        print('Final Nym transport:', json.dumps(row), flush=True)
        timings['total_s'] = round(time.monotonic() - began, 3)
        print('timings:', json.dumps(timings, sort_keys=True), flush=True)
    except Exception:
        failed = True
        traceback.print_exc()
    finally:
        if flow is not None:
            flow.close()
        for child in reversed(children):
            rc = child.stop(20)
            expected = -signal.SIGTERM if child.name == 'native-client' else 0
            if rc != expected or child.forced or child.sanitizer_report():
                failed = True
                print(f'FAIL: {child.name} shutdown rc={rc}, forced={child.forced}, sanitizer={child.sanitizer_report()}')
        for child in children:
            if failed:
                print(f'--- {child.name} ---\n{child.text()}')
        if any(path.exists() for path in [r_sock, r_api, c_sock, c_api]):
            failed = True
            print('FAIL: daemon control/native socket survived shutdown')
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
