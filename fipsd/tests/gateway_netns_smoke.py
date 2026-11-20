#!/usr/bin/env python3
"""Root-only network-namespace smoke for fips-gateway (nostrc-iq1n9.3).

Reproduces docs/how-to/deploy-gateway.md (upstream; fipsd/docs/fips-gateway.md
here) in three namespaces so the host's own networking is untouched:

  gwh   the gateway host: fipsd A (TUN fips0, DNS on [::1]:5354) and
        fips-gateway (pool fd01::/112, LAN interface lan0, DNS on [::]:53,
        a TCP and a UDP port forward to the LAN host)
  peer  mesh node B: fipsd B with an HTTP server and a UDP echo on its mesh
        address; reached by A over a UDP transport on a veth underlay
  lan   an unmodified LAN host (fd02::20) routing the pool via the gateway,
        with an HTTP server and a UDP echo as the inbound forwards' targets

and checks each step of the how-to: the start-up log, the pool route, the
nftables table, outbound resolution and reachability (ping and TCP) through
the NAT, the control socket, inbound forwards from the mesh, composition with
the mesh firewall baseline, and a clean stop that removes the table, route and
proxy NDP entries and leaves the pool state for the next start.

Skips (77) unless run as root with FIPSD_GATEWAY_PRIVILEGED_SMOKE=1. Every
wait is a deadline-bounded observation: a log line, a kernel retransmission
(ping, curl) or, where the gateway's 10 s tick decides, its control socket.
"""
import argparse
import json
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'Testing' / 'fips-chaos'))
sys.path.insert(0, str(ROOT / 'fipsd' / 'tests'))
from chaoslib.net import Lab, LabError, run  # noqa: E402
from udp_interop import Daemon, Failure, control  # noqa: E402

GATEWAY_SOCKET = '/run/fips/gateway.sock'
STATE_DIR = Path('/var/run/fips-gateway')
NSEC_A, NSEC_B = '11' * 32, '22' * 32
POOL = 'fd01::/112'


class Smoke:
    def __init__(self, args):
        self.args = args
        self.work = Path(tempfile.mkdtemp(prefix='fipsgw-smoke-'))
        self.lab = Lab(args.tag)
        self.daemons = []
        self.steps = []
        self.env = dict(os.environ, FIPSD_WORKERS='2')
        self.created_run_fips = not Path('/run/fips').exists()

    # -- reporting -------------------------------------------------------------
    def step(self, name, fn):
        started = time.monotonic()
        try:
            detail = fn()
        except (Failure, LabError, AssertionError, subprocess.SubprocessError, OSError) as e:
            self.steps.append((name, 'FAIL', str(e)))
            print(f'FAIL  {name}: {e}', flush=True)
            raise
        took = time.monotonic() - started
        self.steps.append((name, 'ok', detail or ''))
        print(f'ok    {name} ({took:.1f}s){": " + detail if detail else ""}', flush=True)

    # -- helpers ---------------------------------------------------------------
    def spawn(self, name, ns, argv):
        d = Daemon(name, ns.exec_argv(argv), self.env, str(self.work))
        self.daemons.append(d)
        return d

    def sh(self, ns, *argv, **kw):
        return ns.sh(*argv, **kw).stdout

    def fipsd_yaml(self, name, nsec, ul_addr, peers=(), gateway=None):
        lines = ['node:', f"  identity: {{nsec: '{nsec}', persistent: false}}",
                 f"  control: {{enabled: true, socket_path: '{self.work / (name + '.sock')}'}}",
                 'tun: {enabled: true, name: fips0}',
                 "dns: {enabled: true, bind_addr: '::1', port: 5354}",
                 'transports:', f"  udp: {{bind_addr: '{ul_addr}:2121', advertise_on_nostr: false, public: false}}"]
        if peers:
            lines.append('peers:')
            for npub, addr in peers:
                lines += [f"  - npub: '{npub}'", '    addresses:', '      - transport: udp',
                          f"        addr: '{addr}:2121'", '    connect_policy: auto_connect', '    auto_reconnect: true']
        if gateway:
            lines.append(gateway)
        path = self.work / f'{name}.yaml'
        path.write_text('\n'.join(lines) + '\n')
        return path

    def status(self, name):
        return control(self.work / f'{name}.sock', 'show_status')

    def gateway_query(self, command):
        return control(GATEWAY_SOCKET, command)

    def observe(self, what, seconds, fn):
        """Poll fn (a control-socket read) until it returns a truthy value."""
        deadline = time.monotonic() + seconds
        last = None
        while time.monotonic() < deadline:
            last = fn()
            if last:
                return last
            time.sleep(0.5)
        raise Failure(f'{what}: not observed within {seconds}s; last: {json.dumps(last)[:500]}')

    # -- the run ---------------------------------------------------------------
    def run(self):
        a = self.args
        self.gwh, self.peer, self.lan = self.lab.netns('gwh'), self.lab.netns('peer'), self.lab.netns('lan')
        self.lab.veth(self.gwh, 'ul0', '10.200.0.1/30', self.peer, 'ul0', '10.200.0.2/30')
        self.lab.veth(self.gwh, 'lan0', None, self.lan, 'eth0', None)
        self.gwh.ip('-6', 'addr', 'add', 'fd02::1/64', 'dev', 'lan0', 'nodad')
        self.lan.ip('-6', 'addr', 'add', 'fd02::20/64', 'dev', 'eth0', 'nodad')

        def daemons():
            b_cfg = self.fipsd_yaml('b', NSEC_B, '10.200.0.2')
            self.b = self.spawn('fipsd-b', self.peer, [a.fipsd, '-c', str(b_cfg)])
            self.b.await_log('fipsd running', time.monotonic() + 60)
            sb = self.status('b')
            self.npub_b, self.mesh_b = sb['npub'], sb['ipv6_addr']
            gateway = '\n'.join([
                'gateway:', '  enabled: true', f'  pool: "{POOL}"', '  lan_interface: "lan0"',
                '  dns: {listen: "[::]:53", upstream: "[::1]:5354", ttl: 60}',
                '  port_forwards:', '    - {listen_port: 8080, proto: tcp, target: "[fd02::20]:80"}',
                '    - {listen_port: 5353, proto: udp, target: "[fd02::20]:53"}'])
            self.a_cfg = self.fipsd_yaml('a', NSEC_A, '10.200.0.1', [(self.npub_b, '10.200.0.2')], gateway)
            self.a = self.spawn('fipsd-a', self.gwh, [a.fipsd, '-c', str(self.a_cfg)])
            self.a.await_log('fipsd running', time.monotonic() + 60)
            self.a.await_log('link established', time.monotonic() + 60)
            sa = self.status('a')
            self.npub_a, self.mesh_a = sa['npub'], sa['ipv6_addr']
            return f'A {self.mesh_a}, B {self.mesh_b} ({self.npub_b[:16]}...), link up'
        self.step('daemon prerequisites: fipsd A (TUN fips0, DNS [::1]:5354) linked to B', daemons)

        def sysctls():
            self.gwh.sysctl('net.ipv6.conf.all.forwarding', 1)
            self.gwh.sysctl('net.ipv6.conf.all.proxy_ndp', 1)
            return 'forwarding=1 proxy_ndp=1 in the gateway namespace'
        self.step('kernel sysctls', sysctls)

        # The LAN host's services: an HTTP server and a UDP echo, the inbound targets.
        self.spawn('lan-http', self.lan, [sys.executable, '-m', 'http.server', '--bind', 'fd02::20', '80', '--directory', str(self.work)])
        self.spawn('lan-echo', self.lan, [sys.executable, '-c', ECHO, 'fd02::20', '53'])
        self.spawn('peer-http', self.peer, [sys.executable, '-m', 'http.server', '--bind', self.mesh_b, '8000', '--directory', str(self.work)])
        self.spawn('peer-echo', self.peer, [sys.executable, '-c', ECHO, self.mesh_b, '9000'])
        (self.work / 'index.html').write_text('fips-gateway smoke\n')

        def start():
            self.gw = self.spawn('fips-gateway', self.gwh, [a.gateway, '-c', str(self.a_cfg), '-l', 'debug'])
            deadline = time.monotonic() + 60
            for line in ('gateway config loaded', 'dns upstream is reachable', "created nftables table 'fips_gateway'",
                         'gateway control socket listening', 'fips-gateway running'):
                self.gw.await_log(line, deadline)
            return 'logged: Gateway config loaded, DNS upstream is reachable, Created nftables table, fips-gateway running'
        self.step('start the service (minimum configuration plus DNS listen and forwards)', start)

        def route():
            out = self.sh(self.gwh, 'ip', '-6', 'route', 'show', 'table', 'local')
            assert POOL in out, out
            return f'local {POOL} dev lo present'
        self.step('pool route', route)

        def table():
            text = self.sh(self.gwh, 'nft', 'list', 'table', 'inet', 'fips_gateway')
            for needle in ('chain prerouting', 'chain raw_prerouting', 'chain input', 'chain forward', 'fips_mesh_sources',
                           'tcp dport 8080 dnat ip6 to [fd02::20]:80', 'udp dport 5353 dnat ip6 to [fd02::20]:53'):
                assert needle in text, f'{needle!r} missing from\n{text}'
            return 'inet fips_gateway with its six chains, the set and both forwards'
        self.step('nftables table', table)

        def outbound_dns():
            self.lan.ip('-6', 'route', 'add', POOL, 'via', 'fd02::1')
            out = self.sh(self.lan, 'dig', '+time=5', '+tries=1', '@fd02::1', f'{self.npub_b}.fips', 'AAAA', '+short').strip()
            assert out.startswith('fd01::'), out
            self.vip = out.split()[0]
            refused = self.sh(self.lan, 'dig', '+time=5', '+tries=1', '@fd02::1', 'example.com', 'AAAA')
            assert 'status: REFUSED' in refused, refused
            return f'{self.npub_b[:16]}....fips AAAA {self.vip}; example.com REFUSED'
        self.step('outbound: per-host route, resolver pointed at the gateway, dig', outbound_dns)

        def outbound_ping():
            out = self.sh(self.lan, 'ping', '-6', '-c', '1', '-w', '20', self.vip)
            assert ' 0% packet loss' in out or '1 received' in out, out
            return f'ping {self.vip} answered through DNAT to {self.mesh_b}'
        self.step('outbound: ping6 the pool address', outbound_ping)

        def outbound_tcp():
            out = self.sh(self.lan, 'curl', '-sS', '--max-time', '20', f'http://[{self.vip}]:8000/index.html')
            assert 'fips-gateway smoke' in out, out
            reply = self.sh(self.lan, sys.executable, '-c', SEND_UDP, self.vip, '9000', 'outbound-udp')
            assert reply.strip() == 'outbound-udp', reply
            return 'TCP (HTTP) and UDP through the NAT to the mesh node'
        self.step('outbound: TCP and UDP to the pool address', outbound_tcp)

        def control_socket():
            gw = self.observe('a show_gateway snapshot with the mapping', 30,
                              lambda: (lambda r: r if r.get('pool_allocated', 0) + r.get('pool_active', 0) >= 1 else None)(self.gateway_query('show_gateway')))
            maps = self.observe('the mapping carried traffic (used)', 30,
                                lambda: (lambda m: m if m and m[0].get('used') else None)(self.gateway_query('show_mappings')['mappings']))
            m = maps[0]
            assert m['virtual_ip'] == self.vip and m['mesh_addr'] == self.mesh_b, m
            raw = subprocess.run(['nc', '-U', GATEWAY_SOCKET], input='{"command":"show_gateway"}\n', text=True,
                                 capture_output=True, timeout=10).stdout
            assert raw.startswith('{"status":"ok","data":{'), raw
            return (f"show_gateway pool_total={gw['pool_total']} nat_mappings={gw['nat_mappings']}; show_mappings "
                    f"{m['virtual_ip']} -> {m['mesh_addr']} state={m['state']} used={m['used']}; nc -U answers")
        self.step('operate and verify: the control socket (show_gateway, show_mappings, nc -U)', control_socket)

        def inbound():
            out = self.sh(self.peer, 'curl', '-sS', '--max-time', '20', f'http://[{self.mesh_a}]:8080/index.html')
            assert 'fips-gateway smoke' in out, out
            reply = self.sh(self.peer, sys.executable, '-c', SEND_UDP, self.mesh_a, '5353', 'inbound-udp')
            assert reply.strip() == 'inbound-udp', reply
            return f'mesh peer B reached [fd02::20]:80 and :53 through {self.mesh_a}:8080/tcp and :5353/udp'
        self.step('inbound: port forwards from the mesh', inbound)

        def firewall():
            baseline = (ROOT / 'fipsd' / 'packaging' / 'common' / 'fips.nft').read_text()
            dropins = self.work / 'fips.d'
            (dropins / 'forward').mkdir(parents=True)
            baseline = baseline.replace('/etc/fips/fips.d/', f'{dropins}/')
            (dropins / 'forward' / 'port-forwards.nft').write_text(
                f'ip6 daddr fd02::20 tcp dport 80 ip6 saddr {{ {self.mesh_b} }} accept\n'
                'ip6 daddr fd02::20 tcp dport 80 counter drop\n')
            self.gwh.nft(baseline)
            out = self.sh(self.peer, 'curl', '-sS', '--max-time', '20', f'http://[{self.mesh_a}]:8080/index.html')
            assert 'fips-gateway smoke' in out, out
            (dropins / 'forward' / 'port-forwards.nft').write_text(
                'ip6 daddr fd02::20 tcp dport 80 ip6 saddr { fd00::1 } accept\n'
                'ip6 daddr fd02::20 tcp dport 80 counter drop\n')
            self.gwh.nft(baseline)
            refused = self.peer.sh('curl', '-sS', '--max-time', '3', f'http://[{self.mesh_a}]:8080/index.html', check=False)
            assert refused.returncode != 0, refused.stdout
            chain = self.sh(self.gwh, 'nft', 'list', 'chain', 'inet', 'fips', 'forward')
            assert 'counter packets 0' not in chain.split('tcp dport 80 counter')[1].split('\n')[0], chain
            self.gwh.sh('nft', 'delete', 'table', 'inet', 'fips')
            return 'baseline loaded: an allowing forward drop-in passes the forward, a refusing one drops it (counter moved)'
        self.step('inbound: compose with the mesh firewall baseline', firewall)

        def stop():
            self.gw.stop(30)
            assert self.gw.returncode == 0, f'exit {self.gw.returncode}'
            assert not self.gw.sanitizer_report(), self.gw.text()[-4000:]
            log = self.gw.text().lower()
            for line in ('fips-gateway shutting down', 'final pool status', "deleted nftables table 'fips_gateway'", 'fips-gateway shutdown complete'):
                assert line in log, line
            tables = self.sh(self.gwh, 'nft', 'list', 'tables')
            assert 'fips_gateway' not in tables, tables
            assert POOL not in self.sh(self.gwh, 'ip', '-6', 'route', 'show', 'table', 'local')
            assert self.vip not in self.sh(self.gwh, 'ip', '-6', 'neigh', 'show', 'proxy')
            assert not Path(GATEWAY_SOCKET).exists()
            state = json.loads((STATE_DIR / 'pool.json').read_text())
            assert state['span'] == 0 and state['held'], state
            return f"exit 0; table, route, proxy NDP and socket gone; pool.json holds {state['held']} for {state['hold_secs']}s"
        self.step('clean stop', stop)

        def restart():
            self.gw = self.spawn('fips-gateway-2', self.gwh, [a.gateway, '-c', str(self.a_cfg)])
            self.gw.await_log("pool state restored; holding the previous run's addresses", time.monotonic() + 60)
            self.gw.await_log('fips-gateway running', time.monotonic() + 60)
            out = self.sh(self.lan, 'dig', '+time=5', '+tries=1', '@fd02::1', f'{self.npub_b}.fips', 'AAAA', '+short').strip()
            assert out.startswith('fd01::') and out != self.vip, f'{out} (previous {self.vip})'
            self.gw.stop(30)
            assert self.gw.returncode == 0 and not self.gw.sanitizer_report()
            return f'restored holds; a new name resolved to {out}, not the held {self.vip}'
        self.step('restart keeps the previous run\'s answers held', restart)

        for d in (self.a, self.b):
            assert not d.sanitizer_report(), d.text()[-4000:]

    def teardown(self):
        for d in reversed(self.daemons):
            try:
                d.stop(15)
            except Exception:  # noqa: BLE001 - best-effort cleanup
                pass
        failed = self.lab.teardown()
        shutil.rmtree(STATE_DIR, ignore_errors=True)
        if self.created_run_fips:
            shutil.rmtree('/run/fips', ignore_errors=True)
        if not self.args.keep:
            shutil.rmtree(self.work, ignore_errors=True)
        return failed


ECHO = '''
import socket, sys
s = socket.socket(socket.AF_INET6, socket.SOCK_DGRAM)
s.bind((sys.argv[1], int(sys.argv[2])))
while True:
    data, src = s.recvfrom(2048)
    s.sendto(data, src)
'''
SEND_UDP = '''
import socket, sys
s = socket.socket(socket.AF_INET6, socket.SOCK_DGRAM)
s.settimeout(3)
for attempt in range(6):
    s.sendto(sys.argv[3].encode(), (sys.argv[1], int(sys.argv[2])))
    try:
        print(s.recvfrom(2048)[0].decode()); break
    except socket.timeout:
        continue
'''


def main():
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument('--fipsd', required=True)
    p.add_argument('--gateway', required=True)
    p.add_argument('--tag', default='gw')
    p.add_argument('--keep', action='store_true', help='keep the work directory with configs and logs')
    args = p.parse_args()
    if os.geteuid() != 0:
        print('SKIP: network namespaces, nftables and routes need root')
        return 77
    if os.environ.get('FIPSD_GATEWAY_PRIVILEGED_SMOKE') != '1':
        print('SKIP: the privileged gateway smoke also needs FIPSD_GATEWAY_PRIVILEGED_SMOKE=1')
        return 77
    for tool in ('ip', 'nft', 'dig', 'curl', 'ping', 'nc'):
        if not shutil.which(tool):
            print(f'SKIP: {tool} is not installed')
            return 77
    smoke = Smoke(args)
    rc = 0
    try:
        smoke.run()
    except Exception as e:  # noqa: BLE001 - report then tear down
        rc = 1
        print(f'gateway smoke failed: {e}', file=sys.stderr)
        for d in smoke.daemons:
            print(f'--- {d.name} log (tail) ---\n{d.text()[-3000:]}', file=sys.stderr)
        try:
            print(smoke.lab.diagnostics(), file=sys.stderr)
        except Exception:  # noqa: BLE001
            pass
    finally:
        failed = smoke.teardown()
        if failed:
            print(f'namespaces left behind: {failed}', file=sys.stderr)
            rc = 1
    print(json.dumps({'steps': [{'step': s, 'result': r, 'detail': d} for s, r, d in smoke.steps]}, indent=1))
    return rc


if __name__ == '__main__':
    sys.exit(main())
