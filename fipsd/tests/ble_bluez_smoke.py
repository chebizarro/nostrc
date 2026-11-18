#!/usr/bin/env python3
"""Opt-in BlueZ BLE smoke (nostrc-gfi8h.6.2): two daemons, each bound to its
own LE controller with no other transport, find each other and talk:
advert discovery -> advertised PSM -> L2CAP CoC -> IK link -> XK session ->
native datagrams both directions. C<->C, then, with --rust, C<->Rust with each
side starting the handshake.

The controllers are two virtual ones (btvirt over hci_vhci, BlueZ's emulated
air), created here and removed afterwards; FIPSD_BLE_BLUEZ_ADAPTERS=hciA,hciB
uses two existing ones instead. The second node listens on a non-default PSM,
so the first reaches it only through the PSM its advert carries.

Root only, FIPSD_BLE_BLUEZ_SMOKE=1. No sleeps: waits are bounded successive
observations paced by control or btmgmt replies, or daemon log lines.
"""
import argparse
import os
from pathlib import Path
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import time
from udp_interop import Daemon, NativeFlow, control, Failure

PSMS = (0x0085, 0x0091)
NSECS = ('1' * 64, '2' * 64)
NPUBS = ('npub1fu64hh9hes90w2808n8tjc2ajp5yhddjef0ctx4s7zmsgp6cwx4qgy4eg9',
         'npub1gekhljh9v0jukzdq6xrshdvqx3yqgctc0xs5jjw0yg597xaw8uns47vduw')


def run(argv, check=True):
    return subprocess.run(argv, check=check, text=True, capture_output=True, timeout=30)


def observe(what, predicate, seconds=30, alive=()):
    deadline = time.monotonic() + seconds
    last = None
    while time.monotonic() < deadline:
        for d in alive:
            if not d.alive():
                raise Failure(f'{d.name} exited during {what}')
        last = predicate()
        if last:
            return last
    raise Failure(f'{what} deadline expired; last={last!r}')


def controllers():
    """{name: address} of every controller the kernel has, from btmgmt."""
    out, found, name = run(['btmgmt', 'info'], check=False).stdout, {}, None
    for line in out.splitlines():
        m = re.match(r'^(hci\d+):', line)
        if m:
            name = m.group(1)
            continue
        m = re.match(r'^\s+addr ([0-9A-F:]{17})', line)
        if m and name:
            found[name] = m.group(1)
            name = None
    return found


class Radio:
    """Two LE controllers bluetoothd knows: virtual ones made here, or given."""

    def __init__(self, root, env):
        self.btvirt, self.loaded, self.started_bluetoothd = None, False, False
        given = os.environ.get('FIPSD_BLE_BLUEZ_ADAPTERS', '')
        if run(['systemctl', 'is-active', 'bluetooth'], check=False).stdout.strip() != 'active':
            run(['systemctl', 'start', 'bluetooth'])
            self.started_bluetoothd = True
        if given:
            names = given.split(',')
            have = controllers()
            self.adapters = [(n, have[n]) for n in names]
        else:
            before = controllers()
            self.loaded = not Path('/sys/module/hci_vhci').exists()
            run(['modprobe', 'hci_vhci'])
            self.btvirt = Daemon('btvirt', [shutil.which('btvirt'), '-l2'], env, root)

            def new():
                fresh = {k: v for k, v in controllers().items() if k not in before}
                return sorted(fresh.items(), key=lambda kv: int(kv[0][3:])) if len(fresh) >= 2 else None
            self.adapters = observe('two virtual controllers', new, 30, [self.btvirt])[:2]
        # bluetoothd must have adopted both before a daemon asks it for one.
        observe('bluetoothd sees both controllers',
                lambda: all(a in run(['bluetoothctl', 'list'], check=False).stdout for _, a in self.adapters), 30)
        print(f'radio: {self.adapters} ({"btvirt" if self.btvirt else "given"})', flush=True)

    def close(self):
        if self.btvirt:
            self.btvirt.stop(10)  # its controllers go with it
            if self.loaded:
                run(['modprobe', '-r', 'hci_vhci'], check=False)
        if self.started_bluetoothd:
            run(['systemctl', 'stop', 'bluetooth'], check=False)


def config(root, i, rust, adapter, initiator):
    sock, api = root / f'{i}.sock', root / f'{i}a.sock'
    cfg = root / f'{i}.yaml'
    cfg.write_text(f"""
node:
  identity: {{nsec: '{NSECS[i]}', persistent: false}}
  control: {{enabled: true, socket_path: '{sock}'}}
  native_api: {{enabled: true, socket_path: '{api}'}}
tun: {{enabled: false}}
dns: {{enabled: false}}
transports:
  ble:
    adapter: {adapter}
    psm: {PSMS[i]}
    advertise: true
    scan: true
    auto_connect: {str(i == initiator).lower()}
    accept_connections: true
    probe_cooldown_secs: 5
""")
    return cfg, sock, api


def scenario(args, radio, second, initiator):
    rust_side = second == args.rust
    tag = f"C<->{'Rust' if rust_side else 'C'}, node {initiator} starts the handshake"
    nodes, tools, times = [], [], {}
    with tempfile.TemporaryDirectory(prefix='fble-') as tmp:
        root = Path(tmp)
        env = dict(os.environ, FIPSD_WORKERS='2', TOKIO_WORKER_THREADS='2', NO_COLOR='1')
        env.setdefault('FIPSD_LOG_LEVEL', 'debug')
        env.setdefault('RUST_LOG', 'info,fips::transport::ble=debug')
        start = time.monotonic()
        try:
            for i, binary in enumerate((args.fipsd, second)):
                rust = binary == args.rust
                name = 'rust' if rust else f'c{i}'
                adapter = radio.adapters[i][0]
                cfg, sock, api = config(root, i, rust, adapter, initiator)
                daemon = Daemon(name, [binary, '-c', str(cfg)], env, root)
                node = dict(name=name, rust=rust, daemon=daemon, control=sock, api=api, adapter=adapter,
                            mac=radio.adapters[i][1])
                nodes.append(node)
                daemon.await_log('control socket listening', time.monotonic() + 30)
                daemon.await_log('native api socket listening' if not rust else 'native', time.monotonic() + 30)
                status = control(sock, 'show_status')
                node.update(npub=status['npub'])
                assert node['npub'] == NPUBS[i], status
            daemons = [n['daemon'] for n in nodes]
            for n in nodes:  # listener bound, advertising and scanning
                n['daemon'].await_log('ble transport started' if n['rust'] else 'ble transport up on', time.monotonic() + 30)
            times['bound'] = time.monotonic() - start

            def linked():
                links = [control(n['control'], 'show_links')['links'] for n in nodes]
                peers = [control(n['control'], 'show_peers')['peers'] for n in nodes]
                for i, (n, ls, ps) in enumerate(zip(nodes, links, peers)):
                    if len(ls) != 1 or len(ps) != 1:
                        return None
                    if ls[0]['state'] != ('connected' if n['rust'] else 'active'):
                        return None
                    assert ps[0]['npub'] == nodes[1 - i]['npub'], ps
                return links
            links = observe('authenticated IK link over BLE on both nodes', linked, 60, daemons)
            times['link'] = time.monotonic() - start
            print(f'PASSED {tag}: IK link ({[(n["name"], l[0].get("direction")) for n, l in zip(nodes, links)]})', flush=True)
            for n in nodes:
                ble = [t for t in control(n['control'], 'show_transports')['transports'] if t.get('type') == 'ble']
                assert len(ble) == 1, ble
                n['transport'] = ble[0]
            source, target = nodes[initiator], nodes[1 - initiator]
            control(source['control'], 'probe_start', {'npub': target['npub']})

            def session():
                sessions = [control(n['control'], 'show_sessions')['sessions'] for n in nodes]
                for i, ss in enumerate(sessions):
                    if len(ss) != 1 or ss[0]['state'] != 'established' or ss[0].get('identity') != 'proven':
                        return None
                    assert ss[0]['npub'] == nodes[1 - i]['npub'], ss
                return sessions
            observe('authenticated XK session over BLE on both nodes', session, 60, daemons)
            times['session'] = time.monotonic() - start
            print(f'PASSED {tag}: XK session', flush=True)
            payload, reply = b'BLE ' + tag.encode(), b'BLE reply'
            tool = Daemon('native-client', [args.native_tool, 'serve', str(target['api']), '7654', reply.hex()], env, root)
            tools.append(tool)
            tool.await_log('holding port 7654', time.monotonic() + 15)
            flow = NativeFlow(source['api'], target['npub'], 7654)
            try:
                sent = time.monotonic()
                flow.send(payload)
                assert flow.recv() == reply, 'wrong returned application payload'
                times['datagram_rtt_ms'] = (time.monotonic() - sent) * 1000
                tool.await_log(f'received {payload.hex()}', time.monotonic() + 15)
            finally:
                flow.close()
            times['datagrams'] = time.monotonic() - start
            print(f'PASSED {tag}: native datagrams both directions, exact payloads', flush=True)
            rows = {n['name']: {k: n['transport'].get(k) for k in ('state', 'mtu', 'local_addr')} for n in nodes}
            print(f'TIMINGS {tag}: ' + ', '.join(f'{k} {v:.0f} ms' if k.endswith('_ms') else f'{k} {v * 1000:.0f} ms'
                                                for k, v in times.items()) + f'; transports {rows}', flush=True)
        except BaseException:
            for n in nodes:
                print(f"===== {n['name']} =====\n{n['daemon'].text()[-20000:]}", file=sys.stderr)
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
            keep = os.environ.get('FIPSD_BLE_BLUEZ_SMOKE_LOGS')
            if keep:  # evidence of passing runs too
                for n in nodes:
                    Path(keep).mkdir(parents=True, exist_ok=True)
                    (Path(keep) / f"{tag.replace(' ', '_').replace('<->', '-')}.{n['name']}.log").write_text(n['daemon'].text())
            if failures:
                raise Failure(f'daemon shutdown/sanitizer failures: {failures}')


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--fipsd', required=True)
    ap.add_argument('--native-tool', required=True)
    ap.add_argument('--rust', default='')
    args = ap.parse_args()
    for key in ('fipsd', 'native_tool', 'rust'):  # the daemons run in their scenario directory
        if getattr(args, key):
            setattr(args, key, str(Path(getattr(args, key)).resolve()))
    if os.environ.get('FIPSD_BLE_BLUEZ_SMOKE') != '1':
        print('SKIP: set FIPSD_BLE_BLUEZ_SMOKE=1 and run as root on Linux with BlueZ'); return 77
    if sys.platform != 'linux' or os.geteuid() != 0:
        print('SKIP: requires Linux and root (L2CAP sockets, bluetoothd, hci_vhci)'); return 77
    tools = ['btmgmt', 'bluetoothctl', 'systemctl'] + ([] if os.environ.get('FIPSD_BLE_BLUEZ_ADAPTERS') else ['btvirt', 'modprobe'])
    missing = [t for t in tools if not shutil.which(t)]
    if missing:
        print(f'SKIP: missing {missing} (bluez, bluez-test-tools)'); return 77
    for binary in (args.fipsd, args.native_tool, *([args.rust] if args.rust else [])):
        if not Path(binary).is_file():
            print(f'SKIP: missing prerequisite binary {binary}'); return 77

    def interrupted(signum, _frame):
        raise SystemExit(128 + signum)  # unwind the scenario and the radio before the outer timeout kills us
    signal.signal(signal.SIGTERM, interrupted)
    with tempfile.TemporaryDirectory(prefix='fble-radio-') as tmp:
        radio = Radio(Path(tmp), dict(os.environ))
        try:
            scenario(args, radio, args.fipsd, 0)
            if args.rust:
                scenario(args, radio, args.rust, 0)
                scenario(args, radio, args.rust, 1)
        finally:
            radio.close()
    return 0


if __name__ == '__main__':
    sys.exit(main())
