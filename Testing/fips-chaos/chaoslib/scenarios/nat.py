"""NAT traversal: two nodes behind nftables NATs reach each other through STUN, Nostr signalling and a UDP punch.

Namespaces: `wan` (a bridge standing in for the internet at 198.18.0.0/24,
hosting the lab relay on :7777 and STUN responder on :3478), `nata`/`natb`
(routers: `wan` leg 198.18.0.10/.11, `lan` leg 172.31.{1,2}.1, `ip_forward`,
an nftables `masquerade` on the wan leg, or explicit port-preserving
SNAT/DNAT with `--nat-mode cone`) and `na`/`nb` (the nodes, 172.31.{1,2}.2,
default route through their router). Linux masquerade is a port-preserving,
endpoint-independent mapping with address-and-port-dependent filtering - a
port-restricted cone - so a punch from both sides must open it. The nodes
bind 0.0.0.0:2121 with `advertise_on_nostr: true`; fipsd runs with
`policy: open` (it discovers the peer's `udp:nat` advert); the Rust daemon
(`cr`) is given fipsd as a configured `addr: nat` peer, the upstream NAT lab
shape. The traversal is observed stage by stage so a stall names the stage:

  1. STUN:   the responder logged a binding request from each router's wan tuple
  2. advert: the relay accepted a kind-37195 advert from each node's pubkey
  3. signal: the relay accepted a kind-21059 signal addressed (`p`) to each node
  4. punch:  each router's conntrack holds the UDP entry node:2121 -> peer-wan:2121
  5. link:   both `show_links` hold a healthy link whose remote_addr is the peer's
             NAT-reflexive tuple, show_peers cross-match, the tree converges
  6. a probe (XK session) and, mixed, a native datagram flow through the NATs
"""
import re
import sys
import time
from pathlib import Path

from ..nodes import NotYet

VARIANTS = ('cc', 'cr')
SERVICES = Path(__file__).resolve().parents[2] / 'services'
WAN = '198.18.0'
SIDES = {'a': (10, '172.31.1'), 'b': (11, '172.31.2')}


# A router that accepts unsolicited inbound datagrams to itself confirms a
# conntrack entry for them (observed: the peer's first punch, arriving before
# this side had mapped its port, took the reply tuple :2121 so the node's own
# mapping was moved to a random port and the punch never met). Real
# masquerading routers drop unsolicited input, which leaves the entry
# unconfirmed and the mapping port-preserving; the filter below is that policy.
FILTER = ('table inet filter {\n  chain input { type filter hook input priority filter; policy accept;\n'
          '    iifname "wan" ct state new,invalid drop\n  }\n}\n')


def ruleset(mode, wan_addr, lan_host):
    if mode == 'masquerade':
        return FILTER + ('table ip nat {\n  chain postrouting { type nat hook postrouting priority srcnat; policy accept;\n'
                         '    oifname "wan" masquerade\n  }\n}\n')
    if mode == 'cone':
        return FILTER + ('table ip nat {\n'
                         '  chain prerouting { type nat hook prerouting priority dstnat; policy accept;\n'
                         f'    iifname "wan" ip protocol udp dnat to {lan_host}\n  }}\n'
                         '  chain postrouting { type nat hook postrouting priority srcnat; policy accept;\n'
                         f'    oifname "wan" ip protocol udp snat to {wan_addr}\n    oifname "wan" masquerade\n  }}\n}}\n')
    raise ValueError(mode)


def run(ctx, variant):
    lab, mesh, t = ctx.lab, ctx.mesh(), ctx.timeouts
    wan = lab.netns('wan')
    lab.bridge(wan, 'br0', f'{WAN}.1/24')
    ns = {}
    for side, (idx, lan) in SIDES.items():
        router, node = lab.netns(f'nat{side}'), lab.netns(f'n{side}')
        lab.veth(wan, f'wan{side}', None, router, 'wan', f'{WAN}.{idx}/24')
        lab.enslave(wan, f'wan{side}', 'br0')
        lab.veth(router, 'lan', f'{lan}.1/24', node, 'eth0', f'{lan}.2/24')
        node.ip('route', 'add', 'default', 'via', f'{lan}.1')
        router.sysctl('net.ipv4.ip_forward', '1')
        router.nft(ruleset(ctx.nat_mode, f'{WAN}.{idx}', f'{lan}.2'))
        ns[side] = (router, node)
    relay = ctx.service('relay', wan.exec_argv([sys.executable, '-I', str(SERVICES / 'relay.py'), '--bind', f'{WAN}.1', '--port', '7777']))
    stun = ctx.service('stun', wan.exec_argv([sys.executable, '-I', str(SERVICES / 'stun.py'), '--bind', f'{WAN}.1', '--port', '3478']))
    relay.await_log('listening on', ctx.after(t.startup))
    stun.await_log('listening on', ctx.after(t.startup))

    nostr = {'enabled': True, 'advertise': True, 'advert_relays': [f'ws://{WAN}.1:7777'], 'dm_relays': [f'ws://{WAN}.1:7777'],
             'stun_servers': [f'stun:{WAN}.1:3478'], 'share_local_candidates': False, 'advert_ttl_secs': 60,
             'advert_refresh_secs': 20, 'punch_start_delay_ms': 500, 'punch_interval_ms': 100, 'punch_duration_ms': 2500}
    na = mesh.define('na', 'c', ns['a'][1], '0.0.0.0', nostr=dict(nostr, policy='open'), advertise=True)
    mesh.start(na)
    if variant == 'cr':
        nb = mesh.define('nb', 'rust', ns['b'][1], '0.0.0.0', nostr=nostr, peers=[(na, 'nat')], advertise=True)
    else:
        nb = mesh.define('nb', 'c', ns['b'][1], '0.0.0.0', nostr=dict(nostr, policy='open'), advertise=True)
    mesh.start(nb)
    # fipsd punches from its bound :2121; the Rust daemon from a per-attempt
    # socket, so the peer's reflexive tuple is matched on address only.
    wan_of = {na: f'{WAN}.10:', nb: f'{WAN}.11:'}
    started = time.monotonic()

    def stages():
        stun_log, relay_log = stun.text(), relay.text()
        seen = {
            'stun': {n.name: bool(re.search(rf'binding request from {re.escape(wan_of[n])}\d+', stun_log)) for n in (na, nb)},
            'advert': {n.name: bool(re.search(rf'EVENT kind=37195 pubkey={n.pubkey} .*ok=true', relay_log)) for n in (na, nb)},
            'signal_to': {n.name: len(re.findall(rf'EVENT kind=21059 pubkey=\w+ id=\w+ p=[\w,]*{n.pubkey}', relay_log)) for n in (na, nb)},
            'punch_conntrack': {side: bool(re.search(rf'src={lan}\.2 dst={WAN}\.{SIDES[o][0]} sport=\d+ dport=\d+', ns[side][0].conntrack_udp()))
                                for side, (idx, lan) in SIDES.items() for o in SIDES if o != side},
        }
        return seen

    def traversed():
        seen = stages()
        try:
            links = {n.name: mesh.links_state(n, [other], wan_of[other]) for n, other in ((na, nb), (nb, na))}
        except NotYet as e:
            raise NotYet({'stages': seen, 'links': e.args[0] if e.args else None})
        return seen, links

    seen, links = mesh.wait('NAT traversal: STUN -> advert -> offer/answer -> punch -> IK link through both NATs',
                            ctx.after(t.nat), traversed)
    traversal_s = time.monotonic() - started
    ctx.note(f'stages: {seen}')
    for name, rows in links.items():
        ctx.note(f'{name} show_links: {rows}')
    mesh.wait_tree({na: 1, nb: 1}, t.tree)
    result = {'nat_mode': ctx.nat_mode, 'link_after_s': round(traversal_s, 1), 'stages': seen,
              'remote_addr': {name: rows[0]['remote_addr'] for name, rows in links.items()}}
    if variant == 'cr':
        # The Rust-initiated session first, as udp_interop does: a C probe's
        # teardown is local only, and a Rust peer left holding that session
        # would send its datagrams into it.
        flow = mesh.open_flow(nb, na)
        mesh.send_native(flow, nb, na, b'through two NATs', 'Rust->C native datagram through both NATs')
        mesh.wait_session(na, nb, nb, t.session)
        mesh.probe(nb, na, t.probe)
    mesh.probe(na, nb, t.probe)
    if variant == 'cr':
        flow.close()
    return result
