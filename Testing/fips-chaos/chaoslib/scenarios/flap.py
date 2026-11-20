"""Link flap: a veth goes down, the link is declared dead, comes back and the tree re-converges.

Topology: n1 --veth-- n2 --veth-- n3 (three namespaces). n1 and n3 are fipsd
with n2 as a configured peer (`auto_reconnect`); n2 accepts and is fipsd (`cc`)
or the pinned Rust daemon (`cr`). The n1 end of the n1-n2 veth is taken down
administratively. Both ends must declare the link dead (the peer row and link
row disappear; the 30 s authenticated-silence floor bounds this), the n2-n3
pair must re-converge on its own, and after the link returns n1's configured
retry must re-establish the link and the three-node tree must converge again,
proven end to end by a two-hop n1 -> n3 probe.
"""
import time

VARIANTS = ('cc', 'cr')


def run(ctx, variant):
    lab, mesh, t = ctx.lab, ctx.mesh(), ctx.timeouts
    n1ns, n2ns, n3ns = lab.netns('n1'), lab.netns('n2'), lab.netns('n3')
    lab.veth(n1ns, 'eth0', '10.90.12.1/24', n2ns, 'eth1', '10.90.12.2/24')
    lab.veth(n2ns, 'eth2', '10.90.23.2/24', n3ns, 'eth0', '10.90.23.3/24')
    n2 = mesh.define('n2', 'rust' if variant == 'cr' else 'c', n2ns, '0.0.0.0')
    mesh.start(n2)
    n1 = mesh.define('n1', 'c', n1ns, '10.90.12.1', peers=[(n2, '10.90.12.2:2121')])
    n3 = mesh.define('n3', 'c', n3ns, '10.90.23.3', peers=[(n2, '10.90.23.2:2121')])
    mesh.start(n1, n3)
    full = {n1: [n2], n2: [n1, n3], n3: [n2]}
    mesh.wait_links(full, t.link)
    mesh.wait_tree({n1: 1, n2: 2, n3: 1}, t.tree)
    chain_filters = {n1: {n2: 2}, n2: {n1: 1, n3: 1}, n3: {n2: 2}}
    mesh.wait_bloom(chain_filters, t.tree)
    # Control: the two-hop path works before anything flaps.
    baseline = mesh.probe(n1, n3, t.probe)

    ctx.note('flap: taking n1 eth0 down')
    n1ns.link_down('eth0')
    down_at = time.monotonic()
    mesh.wait_links_gone(n1, n2, 0, t.dead)
    mesh.wait_links_gone(n2, n1, 1, t.dead)
    dead_after = time.monotonic() - down_at
    mesh.wait_tree({n2: 1, n3: 1}, t.tree)

    ctx.note('flap: bringing n1 eth0 back up')
    n1ns.link_up('eth0')
    up_at = time.monotonic()
    mesh.wait_links(full, t.reconnect)
    restored_after = time.monotonic() - up_at
    mesh.wait_tree({n1: 1, n2: 2, n3: 1}, t.tree)
    converged_after = time.monotonic() - up_at
    mesh.wait_bloom(chain_filters, t.tree)
    report = mesh.probe(n1, n3, t.probe)
    return {'dead_declared_after_s': round(dead_after, 1), 'link_restored_after_s': round(restored_after, 1),
            'tree_converged_after_s': round(converged_after, 1),
            'two_hop_probe_rtt_ms': {'before_flap': baseline['rtt'].get('srtt_ms'), 'after_flap': report['rtt'].get('srtt_ms')}}
