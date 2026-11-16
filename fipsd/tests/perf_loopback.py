#!/usr/bin/env python3
"""Opt-in same-host UDP/XK/native-API benchmark. No TUN, root, or relays.

The C traffic driver uses libfipsnative with BOTH implementations. Throughput
is delivered 1200-byte payload Mbit/s under unpaced offered load (loss reported); latency is
serial same-clock one-way and round-trip, not RTT/2. CPU samples cover the
traffic driver interval; wait4-equivalent getrusage deltas also record each
child's full lifetime. Setup observations are outside all timed intervals.
"""
import argparse
import json
import os
from pathlib import Path
import platform
import resource
import socket
import subprocess
import sys
import tempfile
import time

from udp_interop import Daemon, Node, Failure, ControlError, NativeFlow, reserve_port


def wait(nodes, description, predicate, seconds=45):
    deadline = time.monotonic() + seconds
    last = None
    while time.monotonic() < deadline:
        for node in nodes:
            if not node.daemon.alive():
                raise Failure(f'{node.name} exited during {description}')
            node.deadline = deadline
        try:
            last = predicate()
            if last:
                return last
        except (ControlError, OSError) as error:
            last = str(error)
        # Control snapshots are published once per tick by Rust; this is a
        # bounded readiness observation, not a timed traffic pacing loop.
        with nodes[0].daemon.cond:
            nodes[0].daemon.cond.wait(min(.1, max(0, deadline - time.monotonic())))
    raise Failure(f'{description} timed out: {last}')


def cpu_seconds(pid):
    if sys.platform.startswith('linux'):
        # comm may contain spaces or parentheses; fields after the last ')'
        # begin at stat field 3 (state), so utime/stime are offsets 11/12.
        fields = Path(f'/proc/{pid}/stat').read_text().rsplit(')', 1)[1].split()
        return (int(fields[11]) + int(fields[12])) / os.sysconf('SC_CLK_TCK')
    text = subprocess.check_output(['ps', '-p', str(pid), '-o', 'time='], text=True).strip()
    days = 0
    if '-' in text:
        day, text = text.split('-', 1)
        days = int(day)
    parts = [float(x) for x in text.split(':')]
    result = 0.0
    for part in parts:
        result = result * 60 + part
    return result + days * 86400


def run_pair(args, roles, root):
    env = dict(os.environ, FIPSD_WORKERS='2', TOKIO_WORKER_THREADS='2', NO_COLOR='1')
    nodes, apis, ports = [], [], []
    result = None
    failure = None
    lifetime = []
    warm_flow = None
    try:
        for i, role in enumerate(roles):
            ctl, api = root / f'{i}.sock', root / f'{i}a.sock'
            if len(os.fsencode(api)) >= 100:
                raise Failure(f'Unix socket path too long; use a shorter --work-dir: {api}')
            port = reserve_port(socket.AF_INET)
            while port in ports:
                port = reserve_port(socket.AF_INET)
            config = root / f'{i}.yaml'
            config.write_text(f"""node:
  identity: {{nsec: '{str(i + 1) * 64}', persistent: false}}
  control: {{enabled: true, socket_path: '{ctl}'}}
  native_api: {{enabled: true, socket_path: '{api}'}}
  rekey: {{after_secs: 3600}}
tun: {{enabled: false}}
dns: {{enabled: false}}
transports:
  udp: {{bind_addr: '127.0.0.1:{port}'}}
""")
            daemon = Daemon(f'{role}{i}', [args.fipsd if role == 'c' else args.rust, '-c', str(config)], env, root)
            nodes.append(Node(f'{role}{i}', daemon, ctl)); apis.append(api); ports.append(port)
            daemon.await_log('fipsd running' if role == 'c' else 'control socket listening', time.monotonic() + 45)
            if role == 'c':
                daemon.await_log('native api socket listening', time.monotonic() + 45)
        for node in nodes:
            node.npub = wait(nodes, 'identity', lambda: node.query('show_status'))['npub']
        nodes[0].deadline = None
        nodes[0].query('connect', {'npub': nodes[1].npub, 'address': f'127.0.0.1:{ports[1]}', 'transport': 'udp'})

        def linked():
            for node, role in zip(nodes, roles):
                links = node.query('show_links')['links']
                peers = node.query('show_peers')['peers']
                if len(links) != 1 or len(peers) != 1 or links[0]['state'] != ('active' if role == 'c' else 'connected'):
                    return False
            trees = [node.query('show_tree') for node in nodes]
            return (trees[0]['root'] == trees[1]['root'] and all(t['peer_tree_count'] == 1 for t in trees)
                    and all(len(filters) == 1 and filters[0].get('has_filter')
                            for filters in [node.query('show_bloom')['peer_filters'] for node in nodes]))

        wait(nodes, 'IK/tree/bloom', linked)
        nodes[0].deadline = None
        warm_flow = NativeFlow(apis[0], nodes[1].npub, 4243)
        warm_flow.send(b'perf XK setup')

        def established():
            for i, node in enumerate(nodes):
                sessions = node.query('show_sessions')['sessions']
                if len(sessions) != 1:
                    return False
                s = sessions[0]
                if s['state'] != 'established' or s.get('identity') != 'proven':
                    return False
                if s['npub'] != nodes[1 - i].npub or s['is_initiator'] != (i == 0):
                    raise Failure(f'wrong XK peer/role: {s}')
            return True

        wait(nodes, 'proven XK', established)
        before = [cpu_seconds(node.daemon.proc.pid) for node in nodes]
        start = time.monotonic()
        driver = subprocess.run([args.native_tool, str(apis[0]), str(apis[1]), nodes[1].npub,
                                 str(args.seconds), str(args.latency_samples)],
                                text=True, capture_output=True, timeout=args.seconds + 120, check=True)
        elapsed = time.monotonic() - start
        after = [cpu_seconds(node.daemon.proc.pid) for node in nodes]
        result = json.loads(driver.stdout)
        result['pairing'] = '-'.join(roles)
        result['traffic_wall_s'] = elapsed
        result['cpu'] = [{'role': role, 'traffic_s': max(0, end - begin),
                          'traffic_percent_one_core': 100 * max(0, end - begin) / elapsed}
                         for role, begin, end in zip(roles, before, after)]
    except BaseException as error:
        if isinstance(error, subprocess.CalledProcessError):
            print(error.stdout, error.stderr, file=sys.stderr)
        failure = error
    finally:
        if warm_flow:
            warm_flow.close()
        for i, node in enumerate(nodes):
            before = resource.getrusage(resource.RUSAGE_CHILDREN)
            rc = node.daemon.stop(10)
            after = resource.getrusage(resource.RUSAGE_CHILDREN)
            lifetime.append({'role': roles[i], 'user_s': after.ru_utime - before.ru_utime,
                             'system_s': after.ru_stime - before.ru_stime})
            (root / f'{node.name}.log').write_text(node.daemon.text())
            if rc != 0 or node.daemon.forced or node.daemon.sanitizer_report():
                failure = failure or Failure(f'{node.name}: shutdown rc={rc}, forced={node.daemon.forced}')
        if failure:
            for node in nodes:
                print(f'--- {node.name} ---\n{node.daemon.text()}', file=sys.stderr)
            raise failure
    result['lifetime_cpu'] = lifetime
    return result


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--fipsd', required=True)
    p.add_argument('--rust', default=os.environ.get('FIPSD_UDP_INTEROP_RUST_BINARY', ''))
    p.add_argument('--native-tool', required=True)
    p.add_argument('--seconds', type=float, default=5)
    p.add_argument('--latency-samples', type=int, default=1000)
    p.add_argument('--runs', type=int, default=3)
    p.add_argument('--work-dir', type=Path, default=Path(__file__).resolve().parents[2] / 'build-perf-tmp')
    p.add_argument('--output', type=Path)
    args = p.parse_args()
    if not args.rust or not Path(args.rust).is_file():
        print('SKIP: supply the pinned Rust fips binary via --rust', file=sys.stderr)
        return 77
    if not .1 <= args.seconds <= 3600 or not 100 <= args.latency_samples <= 100000 or args.runs < 1:
        p.error('seconds=.1..3600, latency-samples=100..100000, runs>=1 required')
    for key in ('fipsd', 'rust', 'native_tool'):
        setattr(args, key, str(Path(getattr(args, key)).resolve(strict=True)))
    args.work_dir.mkdir(parents=True, exist_ok=True)
    output = {'schema': 1, 'machine': {'system': platform.system(), 'release': platform.release(),
              'arch': platform.machine(), 'cpus': os.cpu_count()}, 'seconds': args.seconds, 'runs': []}
    for run in range(args.runs):
        rows = []
        # Rotate pair ordering across runs, without parallel measurements.
        pairs = [('c', 'c'), ('rust', 'rust'), ('c', 'rust')]
        for roles in pairs[run % 3:] + pairs[:run % 3]:
            print(f'run {run + 1}: {roles}', file=sys.stderr, flush=True)
            with tempfile.TemporaryDirectory(prefix='p', dir=args.work_dir) as tmp:
                rows.append(run_pair(args, roles, Path(tmp)))
        output['runs'].append(rows)
    text = json.dumps(output, indent=2, allow_nan=False) + '\n'
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(text)
    else:
        print(text, end='')
    return 0


if __name__ == '__main__':
    try:
        sys.exit(main())
    except (Failure, OSError, subprocess.SubprocessError, ValueError) as error:
        print(f'FAIL: {error}', file=sys.stderr)
        sys.exit(1)
