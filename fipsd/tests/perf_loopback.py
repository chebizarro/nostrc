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


def sustainable_search(probe, args):
    """Bracket then bisect; each result retains its actual offer/delivery."""
    probes = []
    best = None

    def test(rate):
        nonlocal best
        row = probe(rate)
        row['rate_achieved'] = abs(row['offered_mbps'] / rate - 1) <= .02
        row['sustainable'] = row['rate_achieved'] and row['loss_fraction'] < args.loss_threshold
        probes.append(row)
        print(f"  target={rate:.2f} offered={row['offered_mbps']:.2f} "
              f"delivered={row['throughput_mbps']:.2f} Mbps loss={row['loss_fraction']:.3%} "
              f"{'pass' if row['sustainable'] else 'fail'}", file=sys.stderr, flush=True)
        if row['sustainable']:
            best = row
        return row['sustainable']

    low, high = 0.0, min(100.0, args.paced_max_mbps)
    if test(high):
        low = high
        while high < args.paced_max_mbps:
            high = min(high * 2, args.paced_max_mbps)
            if not test(high):
                break
            low = high
    else:
        while high > 1:
            rate = max(1.0, high / 2)
            if test(rate):
                low = rate
                break
            high = rate
    while low and high - low > low * args.paced_resolution:
        rate = (low + high) / 2
        if test(rate):
            low = rate
        else:
            high = rate
    # Null is an explicit failed search, never a fabricated zero-loss rate.
    result = dict(best or probes[-1])
    result.update(sustainable_mbps=best['offered_mbps'] if best else None,
                  sustainable_loss=best['loss_fraction'] if best else None,
                  search_ceiling_reached=bool(best and low == args.paced_max_mbps),
                  probes=probes)
    return result


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
        captures = []
        if getattr(args, 'fipsctl', None):
            for i, node in enumerate(nodes):
                directory = args.profile_dir / root.name / str(i)
                command = [args.fipsctl, '--socket', str(root / f'{i}.sock'), 'profile', 'tick']
                armed = time.monotonic()  # the capture starts after this
                on = subprocess.run([*command, 'on', '--dir', str(directory)],
                                    text=True, capture_output=True, check=True, timeout=10)
                captures.append((command, json.loads(on.stdout), armed))
        probe_index = 0

        def probe(rate=None):
            nonlocal probe_index
            probe_index += 1
            before = [cpu_seconds(node.daemon.proc.pid) for node in nodes]
            start = time.monotonic()
            seconds = args.paced_seconds if rate is not None else args.seconds
            command = [args.native_tool, str(apis[0]), str(apis[1]), nodes[1].npub,
                       str(seconds), str(args.latency_samples)]
            if rate is not None:
                # Native listener teardown is asynchronous. A distinct port
                # isolates each probe without restarting either daemon.
                command.extend((str(rate), str(4243 + probe_index)))
            driver = subprocess.run(command, text=True, capture_output=True,
                                    timeout=seconds + 120, check=True)
            elapsed = time.monotonic() - start
            after = [cpu_seconds(node.daemon.proc.pid) for node in nodes]
            row = json.loads(driver.stdout)
            row['traffic_wall_s'] = elapsed
            row['cpu'] = [{'role': role, 'traffic_s': max(0, end - begin),
                           'traffic_percent_one_core': 100 * max(0, end - begin) / elapsed,
                           'us_per_delivered_datagram': (max(0, end - begin) * 1e6 / row['received']
                                                         if row['received'] else None)}
                          for role, begin, end in zip(roles, before, after)]
            return row

        result = sustainable_search(probe, args) if getattr(args, 'paced', False) else probe()
        if captures:
            result['tick_profiles'] = []
            result['pipeline_reports'] = [[line[line.index('[pipe '):] for line in node.daemon.text().splitlines()
                                           if '[pipe ' in line] for node in nodes]
            for command, on, armed in captures:
                status = subprocess.run([*command, 'status'], text=True, capture_output=True,
                                        check=True, timeout=10)
                off = subprocess.run([*command, 'off'], text=True, capture_output=True,
                                     check=True, timeout=10)
                # An upper bound on how long the capture ran: it ended before `off` returned.
                result['tick_profiles'].append({'on': on, 'status': json.loads(status.stdout),
                                                'off': json.loads(off.stdout),
                                                'window_s': time.monotonic() - armed})
        result['pairing'] = '-'.join(roles)
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
    p.add_argument('--fipsctl', help='Capture each node with a profiling-enabled fipsctl')
    p.add_argument('--profile-dir', type=Path, help='Capture sink (non-root; root uses /var/log/fips)')
    p.add_argument('--rust', default=os.environ.get('FIPSD_UDP_INTEROP_RUST_BINARY', ''))
    p.add_argument('--native-tool', required=True)
    p.add_argument('--paced', action='store_true', help='report-only sustainable offered-rate search')
    p.add_argument('--paced-seconds', type=float, default=3)
    p.add_argument('--loss-threshold', type=float, default=.01, help='fraction, strict upper bound')
    p.add_argument('--paced-resolution', type=float, default=.02, help='relative bracket width')
    p.add_argument('--paced-max-mbps', type=float, default=3200, help='search ceiling in payload Mbit/s')
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
    if not (.1 <= args.paced_seconds <= 3600 and 0 < args.loss_threshold < 1
            and .001 <= args.paced_resolution <= .5 and 1 <= args.paced_max_mbps <= 100000):
        p.error('invalid paced duration, loss fraction, resolution or ceiling')
    if args.paced and args.fipsctl:
        p.error('profile captures require unpaced mode (one measurement per capture)')
    for key in ('fipsd', 'rust', 'native_tool'):
        setattr(args, key, str(Path(getattr(args, key)).resolve(strict=True)))
    if args.fipsctl and not args.profile_dir:
        p.error('--fipsctl requires --profile-dir')
    args.work_dir.mkdir(parents=True, exist_ok=True)
    output = {'schema': 1, 'machine': {'system': platform.system(), 'release': platform.release(),
              'arch': platform.machine(), 'cpus': os.cpu_count()}, 'seconds': args.seconds, 'runs': []}
    if args.paced:
        output['paced'] = {key: getattr(args, key) for key in
                           ('paced_seconds', 'loss_threshold', 'paced_resolution', 'paced_max_mbps')}
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
