#!/usr/bin/env python3
"""Opt-in root/netns Ethernet IK + XK + datagrams + delete/recreate acceptance.

No sleeps or timer-based interface probes. Control observations are bounded
successive immutable snapshots, paced by replies (Rust publishes off-loop).
Uses the existing native API client helpers, never modifies the UDP harness.
"""
import argparse
import os
from pathlib import Path
import shutil
import signal
import subprocess
import sys
import tempfile
import time
from udp_interop import Daemon, NativeFlow, control, Failure


def run(argv):
    return subprocess.run(argv, check=True, text=True, capture_output=True, timeout=15)


def observe(nodes, what, predicate, seconds=30):
    deadline = time.monotonic() + seconds
    last = None
    while time.monotonic() < deadline:
        for n in nodes:
            if not n['daemon'].alive():
                raise Failure(f"{n['name']} exited during {what}")
        last = predicate()
        if last:
            return last
    raise Failure(f'{what} deadline expired; last={last!r}')


def row(node, command, key):
    return control(node['control'], command)[key]


def make_pair(names):
    run(['ip', '-n', names[0], 'link', 'add', 'eth0', 'type', 'veth', 'peer', 'name', 'eth1'])
    run(['ip', '-n', names[0], 'link', 'set', 'eth1', 'netns', names[1]])
    run(['ip', '-n', names[1], 'link', 'set', 'eth1', 'name', 'eth0'])
    for i, ns in enumerate(names):
        run(['ip', '-n', ns, 'link', 'set', 'eth0', 'address', f'02:00:00:00:00:0{i + 1}'])
        run(['ip', '-n', ns, 'link', 'set', 'eth0', 'mtu', '1400'])
        run(['ip', '-n', ns, 'link', 'set', 'eth0', 'up'])


def scenario(args, second, initiator, discovery=False):
    nodes, tools = [], []
    npubs = ['npub1fu64hh9hes90w2808n8tjc2ajp5yhddjef0ctx4s7zmsgp6cwx4qgy4eg9',
             'npub1gekhljh9v0jukzdq6xrshdvqx3yqgctc0xs5jjw0yg597xaw8uns47vduw']
    names = [f'fe{os.getpid()}a', f'fe{os.getpid()}b']
    created = []
    tag = f"C<->{('Rust' if second == args.rust else 'C')}, initiator={initiator}, announce={discovery}"
    with tempfile.TemporaryDirectory(prefix='feth-') as tmp:
        root = Path(tmp)
        env = dict(os.environ, FIPSD_WORKERS='2', TOKIO_WORKER_THREADS='2', NO_COLOR='1')
        try:
            for ns in names:
                run(['ip', 'netns', 'add', ns]); created.append(ns)
                run(['ip', '-n', ns, 'link', 'set', 'lo', 'up'])
            make_pair(names)
            for i, binary in enumerate((args.fipsd, second)):
                name = 'rust' if binary == args.rust else f'c{i}'
                sock, api = root / f'{i}.sock', root / f'{i}a.sock'
                cfg = root / f'{i}.yaml'
                cfg.write_text(f"""
node:
  identity: {{nsec: '{str(i + 1) * 64}', persistent: false}}
  control: {{enabled: true, socket_path: '{sock}'}}
  native_api: {{enabled: true, socket_path: '{api}'}}
tun: {{enabled: false}}
dns: {{enabled: false}}
transports:
  ethernet:
    interface: eth0
    listen: true
    announce: {str(discovery and i != initiator).lower()}
    auto_connect: {str(discovery and i == initiator).lower()}
    accept_connections: true
""")
                if i == initiator and not discovery:
                    with cfg.open('a') as f:
                        f.write(f"\npeers:\n  - npub: '{npubs[1-i]}'\n    addresses: [{{transport: ethernet, addr: 'eth0/02:00:00:00:00:0{2-i}'}}]\n")
                daemon = Daemon(name, ['ip', 'netns', 'exec', names[i], binary, '-c', str(cfg)], env, root)
                node = dict(name=name, daemon=daemon, control=sock, api=api, mac=f'02:00:00:00:00:0{i+1}')
                nodes.append(node)
                daemon.await_log('control socket listening' if name == 'rust' else 'fipsd running', time.monotonic() + 30)
                daemon.await_log('native api socket listening' if name != 'rust' else 'native', time.monotonic() + 30)
                status = control(sock, 'show_status'); node.update(npub=status['npub'], addr=status['node_addr']); assert node['npub'] == npubs[i]
            def bound():
                rows = [row(n, 'show_transports', 'transports') for n in nodes]
                return rows if all(len(rs) == 1 and 'local_addr' not in rs[0] and rs[0]['interface']['presence'] == 'present' and rs[0]['mtu'] == 1397
                                   for n, rs in zip(nodes, rows)) else None
            observe(nodes, 'raw sockets bound and interface-derived MTU', bound)
            source, target = nodes[initiator], nodes[1 - initiator]
            if not discovery:
                control(source['control'], 'connect', {'npub': target['npub'], 'address': 'eth0/' + target['mac'], 'transport': 'ethernet'})
            def linked():
                links = [row(n, 'show_links', 'links') for n in nodes]
                peers = [row(n, 'show_peers', 'peers') for n in nodes]
                for i, (n, ls, ps) in enumerate(zip(nodes, links, peers)):
                    if len(ls) != 1 or len(ps) != 1:
                        return None
                    assert ls[0]['direction'] == ('outbound' if i == initiator else 'inbound'), ls
                    if ls[0]['state'] != ('connected' if n['name'] == 'rust' else 'active'):
                        return None
                    assert ps[0]['npub'] == nodes[1-i]['npub'], ps
                return links
            observe(nodes, 'authenticated IK on both nodes', linked)
            print(f'PASSED {tag}: IK link', flush=True)
            control(source['control'], 'probe_start', {'npub': target['npub']})
            def session():
                sessions = [row(n, 'show_sessions', 'sessions') for n in nodes]
                for i, ss in enumerate(sessions):
                    if len(ss) != 1 or ss[0]['state'] != 'established' or ss[0].get('identity') != 'proven':
                        return None
                    assert ss[0]['npub'] == nodes[1-i]['npub'], ss
                    assert ss[0]['is_initiator'] == (i == initiator), ss
                return sessions
            observe(nodes, 'authenticated XK on both nodes', session)
            print(f'PASSED {tag}: XK session (complementary roles)', flush=True)
            payload, reply = b'Ethernet C-Rust datagram', b'Ethernet reply'
            tool = Daemon('native-client', [args.native_tool, 'serve', str(target['api']), '7654', reply.hex()], env, root)
            tools.append(tool); tool.await_log('holding port 7654', time.monotonic() + 15)
            def exchange(label):
                flow = NativeFlow(source['api'], target['npub'], 7654)
                try:
                    # Distinct bytes per stage: a pre-rebind receipt cannot
                    # satisfy the post-rebind application-delivery assertion.
                    sent_payload = payload + b':' + label.encode()
                    flow.send(sent_payload)
                    assert flow.recv() == reply, 'wrong returned application payload'
                    tool.await_log(f'received {sent_payload.hex()}', time.monotonic() + 15)
                finally:
                    flow.close()
                print(f'PASSED {tag}: {label} datagrams both directions, exact payloads', flush=True)
            exchange('initial')
            old_binds = [row(n, 'show_transports', 'transports')[0]['interface']['binds'] for n in nodes]
            run(['ip', '-n', names[0], 'link', 'del', 'eth0'])
            make_pair(names)
            def rebound():
                rs = [row(n, 'show_transports', 'transports')[0] for n in nodes]
                return rs if all(v['interface']['binds'] > old and v['interface']['presence'] == 'present'
                                 for v, old in zip(rs, old_binds)) else None
            observe(nodes, 'delete/recreate event-driven rebind with announce disabled', rebound, 45)
            observe(nodes, 'discovered/configured peer IK recovery after rebind', linked, 60)
            control(source['control'], 'probe_start', {'npub': target['npub']})
            observe(nodes, 'XK recovery after rebind', session, 60)
            exchange('after rebind')
            print(f'PASSED {tag}: delete/recreate rebind', flush=True)
        except BaseException:
            for n in nodes:
                print(f"===== {n['name']} =====\n{n['daemon'].text()[-16000:]}", file=sys.stderr)
            raise
        finally:
            for tool in tools:
                tool.stop(5)
                assert not tool.sanitizer_report(), tool.text()
            failures = []
            for n in reversed(nodes):
                rc = n['daemon'].stop(15)
                if rc != 0 or n['daemon'].sanitizer_report():
                    failures.append((n['name'], rc, n['daemon'].text()[-10000:]))
            for ns in reversed(created):
                run(['ip', 'netns', 'del', ns])
            if failures:
                raise Failure(f'daemon shutdown/sanitizer failures: {failures}')


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--fipsd', required=True)
    ap.add_argument('--native-tool', required=True)
    ap.add_argument('--rust', default='')
    args = ap.parse_args()
    if os.environ.get('FIPSD_ETHERNET_PRIVILEGED_SMOKE') != '1':
        print('SKIP: set FIPSD_ETHERNET_PRIVILEGED_SMOKE=1 and run as root on Linux'); return 77
    if sys.platform != 'linux' or os.geteuid() != 0 or not shutil.which('ip'):
        print('SKIP: requires Linux, root/CAP_NET_ADMIN+CAP_NET_RAW, and iproute2 netns/veth'); return 77
    caps = next((line.split()[1] for line in Path('/proc/self/status').read_text().splitlines()
                 if line.startswith('CapEff:')), '0')
    required = (1 << 12) | (1 << 13) | (1 << 21)  # NET_ADMIN, NET_RAW, SYS_ADMIN (netns mount)
    if int(caps, 16) & required != required:
        print('SKIP: effective capabilities lack CAP_NET_ADMIN, CAP_NET_RAW or CAP_SYS_ADMIN for netns/veth'); return 77
    def interrupted(signum, _frame):
        raise SystemExit(128 + signum)  # unwind scenario finally before the outer timeout kills us
    signal.signal(signal.SIGTERM, interrupted)
    for binary in (args.fipsd, args.native_tool, *([args.rust] if args.rust else [])):
        if not Path(binary).is_file():
            print(f'SKIP: missing prerequisite binary {binary}'); return 77
    for second in [args.fipsd] + ([args.rust] if args.rust else []):
        for initiator in (0, 1):
            scenario(args, second, initiator)
        scenario(args, second, 0, discovery=True)
    return 0


if __name__ == '__main__':
    sys.exit(main())
