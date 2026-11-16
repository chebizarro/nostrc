"""MTU: the veth MTU drops below the daemons' UDP MTU; the link and session must carry on.

Neither fipsd nor the pinned Rust daemon sets DF or watches interface MTUs:
the UDP transport MTU is configured (1357 by default) and only session-level
`PathMtuNotification`/`MtuExceeded` signals from FIPS transit nodes lower a
session MTU. On a direct veth the kernel therefore fragments every datagram
larger than the interface MTU. This scenario asserts the link/transport-layer
contract that holds: with the veth at 600 bytes from the start, IK link,
spanning tree and the 1 KiB bloom announce all complete (the namespace's
`FragCreates` counter proves datagrams exceeded the MTU), probes and, in the
mixed variant, a maximum-payload Rust->C native datagram are delivered; then
the MTU is raised to 1500 and lowered again to 900 under a live link and the
same traffic still flows. The configured transport MTU and the session
`mmp.path_mtu` are recorded, not asserted to adapt. Namespaces: n1 (fipsd) and
n2 (fipsd for `cc`, Rust for `cr`) over one veth.
"""

VARIANTS = ('cc', 'cr')
LOW, MID, FULL = 600, 900, 1500


def run(ctx, variant):
    lab, mesh, t = ctx.lab, ctx.mesh(), ctx.timeouts
    n1ns, n2ns = lab.netns('n1'), lab.netns('n2')
    lab.veth(n1ns, 'eth0', '10.91.0.1/24', n2ns, 'eth0', '10.91.0.2/24')
    for ns in (n1ns, n2ns):
        ns.link_mtu('eth0', LOW)
    frags = {ns.name: ns.frag_creates() for ns in (n1ns, n2ns)}
    n2 = mesh.define('n2', 'rust' if variant == 'cr' else 'c', n2ns, '10.91.0.2')
    mesh.start(n2)
    n1 = mesh.define('n1', 'c', n1ns, '10.91.0.1', peers=[(n2, '10.91.0.2:2121')])
    mesh.start(n1)
    pair = {n1: [n2], n2: [n1]}
    result = {'veth_mtu_phases': [LOW, FULL, MID]}

    def frag_delta(label):
        now = {ns.name: ns.frag_creates() for ns in (n1ns, n2ns)}
        delta = {k: now[k] - frags[k] for k in now}
        frags.update(now)
        ctx.note(f'FragCreates since {label}: {delta}')
        return delta

    mesh.wait_links(pair, t.link)
    mesh.wait_tree({n1: 1, n2: 1}, t.tree)
    mesh.wait_bloom({n1: {n2: 1}, n2: {n1: 1}}, t.tree)
    delta = frag_delta('start at MTU %d' % LOW)
    if not all(v > 0 for v in delta.values()):
        raise ctx.Failure(f'link set-up at veth MTU {LOW} created no IPv4 fragments on both sides ({delta}); '
                          'the bloom announce alone exceeds that MTU, so the datagrams did not traverse the lowered link')
    result['frag_creates_link_setup'] = delta
    flow = None
    if variant == 'cr':
        # Rust-initiated session before any C probe (see nat.py).
        flow = mesh.open_flow(n2, n1)
        payload = bytes(range(256)) * (flow.data['max_payload'] // 256 + 1)
        payload = payload[:flow.data['max_payload']]
        mesh.send_native(flow, n2, n1, payload, f'max-payload ({len(payload)} B) datagram at veth MTU {LOW}')
        mesh.wait_session(n1, n2, n2, t.session)
        delta = frag_delta('the native datagram')
        if delta[n2ns.name] < 1:
            raise ctx.Failure(f'a {len(payload)}-byte datagram at veth MTU {LOW} created no fragments in {n2ns.name}')
        result['native_payload_bytes'] = len(payload)
        result['session_path_mtu_reported'] = n1.sessions()[0].get('mmp', {}).get('path_mtu')
    mesh.probe(n1, n2, t.probe)
    transport_before = {n.name: n.transport_mtus() for n in (n1, n2)}

    for mtu in (FULL, MID):
        ctx.note(f'mtu: setting both veth ends to {mtu} under the live link')
        for ns in (n1ns, n2ns):
            ns.link_mtu('eth0', mtu)
        mesh.wait_links(pair, t.link)
        mesh.probe(n1, n2, t.probe)
        if flow:
            mesh.send_native(flow, n2, n1, payload, f'max-payload datagram at veth MTU {mtu}')
            mesh.wait_session(n1, n2, n2, t.session)
        delta = frag_delta(f'MTU {mtu}')
        if mtu == MID and flow and delta[n2ns.name] < 1:
            raise ctx.Failure(f'the datagram at veth MTU {mtu} created no fragments in {n2ns.name}')
    mesh.wait_tree({n1: 1, n2: 1}, t.tree)
    transport_after = {n.name: n.transport_mtus() for n in (n1, n2)}
    result['transport_mtu_before_after'] = [transport_before, transport_after]
    result['transport_mtu_adapted'] = transport_before != transport_after
    if flow:
        flow.close()
    return result
