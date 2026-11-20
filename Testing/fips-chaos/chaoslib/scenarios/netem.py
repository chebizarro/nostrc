"""Loss and latency: netem on one leg, optional iperf3 load; the link and session survive and rekey.

Namespaces n1 (fipsd) and n2 (fipsd or Rust) over one veth; n1's egress
carries `netem delay 40ms 10ms loss 5% 25%` from before the daemons start, so
the IK handshake, tree and bloom exchange already happen under impairment.
When iperf3 is available a 10 Mbit/s UDP stream n1 -> n2 shares the impaired
leg for the duration. With `node.rekey.after_secs: 25` the harness then waits
for the FMP link rekey (K bit flips on both peer rows) and, in the mixed
variant where a Rust native datagram holds an XK session open, the FSP session
rekey too; traffic (probes, datagrams) must flow under the new keys and no
mismatch/decrypt/replay counter may rise.
"""
import json
import shutil
import time

from ..nodes import NotYet

VARIANTS = ('cc', 'cr')
IMPAIRMENT = 'delay 40ms 10ms loss 5% 25%'
REKEY_AFTER = 25


def run(ctx, variant):
    lab, mesh, t = ctx.lab, ctx.mesh(), ctx.timeouts
    n1ns, n2ns = lab.netns('n1'), lab.netns('n2')
    lab.veth(n1ns, 'eth0', '10.92.0.1/24', n2ns, 'eth0', '10.92.0.2/24')
    n1ns.netem('eth0', IMPAIRMENT)
    n2 = mesh.define('n2', 'rust' if variant == 'cr' else 'c', n2ns, '10.92.0.2', rekey_after=REKEY_AFTER)
    mesh.start(n2)
    n1 = mesh.define('n1', 'c', n1ns, '10.92.0.1', peers=[(n2, '10.92.0.2:2121')], rekey_after=REKEY_AFTER)
    mesh.start(n1)
    pair = {n1: [n2], n2: [n1]}
    mesh.wait_links(pair, t.link)
    link_up_at = time.monotonic()
    mesh.wait_tree({n1: 1, n2: 1}, t.tree)
    result = {'netem': IMPAIRMENT}

    load = None
    if shutil.which('iperf3'):
        # --forceflush: iperf3 block-buffers a pipe, and the harness reads its readiness line.
        server = ctx.service('iperf3-server', n2ns.exec_argv(['iperf3', '-s', '-p', '5201', '-1', '--forceflush']))

        def listening():
            if not server.alive():
                raise ctx.Failure(f'iperf3 server exited: {server.text()[-500:]}')
            if ':5201' not in n2ns.sh('ss', '-Hltn', check=False).stdout:
                raise NotYet('iperf3 server socket not listening yet')
            return True

        mesh.wait('iperf3 server listening on n2', ctx.after(t.startup), listening)
        load = ctx.service('iperf3-client', n1ns.exec_argv(['iperf3', '-c', '10.92.0.2', '-p', '5201', '-u', '-b', '10M',
                                                           '-t', str(int(t.link_rekey)), '-l', '1200', '--forceflush']))
        result['background_load'] = 'iperf3 udp 10M n1->n2'
    else:
        result['background_load'] = 'none (iperf3 not installed)'

    flow = None
    if variant == 'cr':
        flow = mesh.open_flow(n2, n1)
        mesh.send_native(flow, n2, n1, b'netem: opening datagram', 'Rust->C datagram opening the session')
        mesh.wait_session(n1, n2, n2, t.session)
        session_up_at = time.monotonic()
        session_k = mesh.session_k_bits(n1, n2)
    link_k = mesh.link_k_bits(n1, n2)
    ctx.note(f'initial K bits: link={link_k}' + (f' session={session_k}' if flow else ''))
    mesh.probe(n1, n2, t.probe)

    if flow:
        rows = mesh.wait_session_rekey(n1, n2, session_k, session_up_at + t.session_rekey)
        result['session_rekey_after_s'] = round(time.monotonic() - session_up_at, 1)
        ctx.note('session rekey: ' + json.dumps({k: v[0]['current_k_bit'] for k, v in rows.items()}))
    rows = mesh.wait_link_rekey(n1, n2, link_k, link_up_at + t.link_rekey)
    result['link_rekey_after_s'] = round(time.monotonic() - link_up_at, 1)
    ctx.note('link rekey: ' + json.dumps({k: v['current_k_bit'] for k, v in rows.items()}))

    mesh.probe(n1, n2, t.probe)
    if flow:
        mesh.send_native(flow, n2, n1, b'netem: datagram under the new keys', 'post-rekey Rust->C datagram')
        mesh.probe(n2, n1, t.probe)
        mesh.wait_session(n1, n2, n2, t.session)
    mesh.wait_links(pair, t.link)
    suspicious = {n.name: mesh.protocol_counters(n) for n in (n1, n2)}
    if any(suspicious.values()):
        raise ctx.Failure(f'protocol failure counters rose under impairment: {suspicious}')
    if load and load.alive():
        load.stop(5)
    if flow:
        flow.close()
    return result
