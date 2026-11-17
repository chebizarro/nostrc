#!/usr/bin/env python3
"""Opt-in: a Rust node finds fipsd over Tor from its Nostr advert alone (nostrc-r4aeo).

fipsd runs transports.tor in directory mode with the control-port monitor
and advertises only its onion (udp/tcp advertise_on_nostr off) on a local
stdlib NIP-01 relay (Testing/fips-chaos/services/relay.py). fipsd starts as
soon as tor has written HiddenServiceDir/hostname (tor's "Bootstrapped 0%"
notice follows the key load), before any descriptor exists, so its advert
waits for tor's HS_DESC UPLOADED. The pinned Rust daemon gets fipsd's npub
as a `via_nostr` peer with no address and dials through a second local tor
(socks5 mode); the IK link must come up over the onion. Waits are events:
tor notices and control events, relay EVENT log lines, daemon log lines.
Timings are printed as JSON. SKIP (77) without a tor binary (--tor /
FIPSD_TOR_BINARY) or the Rust daemon (--rust / FIPSD_UDP_INTEROP_RUST_BINARY).
Every tor data directory lives under FIPSD_TEST_TMPDIR.
"""
import argparse
import base64
import json
import os
from pathlib import Path
import socket
import struct
import subprocess
import sys
import time

sys.dont_write_bytecode = True  # never leave __pycache__ in the source tree
sys.path.insert(0, str(Path(__file__).resolve().parent))
import tor_interop as ti  # noqa: E402 - LogTap, TorControl, start/stop/query helpers

RELAY = Path(__file__).resolve().parents[2] / 'Testing' / 'fips-chaos' / 'services' / 'relay.py'
DESCRIPTOR_SECS = 300
LINK_SECS = 420
BECH32 = 'qpzry9x8gf2tvdw0s3jn54khce6mua7l'


def npub_hex(npub):
    acc = bits = 0
    out = bytearray()
    for c in npub[npub.rindex('1') + 1:-6]:
        acc, bits = (acc << 5) | BECH32.index(c), bits + 5
        while bits >= 8:
            bits -= 8
            out.append((acc >> bits) & 0xff)
    return bytes(out[:32]).hex()


def relay_events(port, filt):
    """One REQ over a minimal RFC 6455 client: the stored events up to EOSE."""
    with socket.create_connection(('127.0.0.1', port), timeout=30) as s:
        key = base64.b64encode(os.urandom(16)).decode()
        s.sendall((f'GET / HTTP/1.1\r\nHost: 127.0.0.1:{port}\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n'
                   f'Sec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n\r\n').encode())
        f = s.makefile('rb')
        assert b' 101 ' in f.readline(), 'relay refused the websocket upgrade'
        while f.readline() not in (b'\r\n', b''):
            pass
        payload, mask = json.dumps(['REQ', 'advert', filt]).encode(), os.urandom(4)
        assert len(payload) < 126
        s.sendall(bytes([0x81, 0x80 | len(payload)]) + mask + bytes(b ^ mask[i % 4] for i, b in enumerate(payload)))
        events = []
        while True:
            b0, b1 = f.read(2)
            n = b1 & 0x7f
            if n == 126:
                n = struct.unpack('>H', f.read(2))[0]
            elif n == 127:
                n = struct.unpack('>Q', f.read(8))[0]
            body = f.read(n)
            if b0 & 0x0f != 1:
                continue
            msg = json.loads(body)
            if msg[0] == 'EVENT':
                events.append(msg[2])
            elif msg[0] == 'EOSE':
                return events


def launch_tor(tor, root, name, procs, listen_port=None):
    d = root / name
    d.mkdir(mode=0o700)
    socks, ctl = ti.free_port(), ti.free_port()
    password = os.urandom(12).hex()
    hashed = subprocess.run([tor, '--quiet', '--hash-password', password], check=True,
                            capture_output=True, text=True).stdout.strip().splitlines()[-1]
    hidden = f'HiddenServiceDir {d}/hs\nHiddenServicePort 443 127.0.0.1:{listen_port}\n' if listen_port else ''
    (d / 'torrc').write_text(f'''DataDirectory {d}/data
SocksPort 127.0.0.1:{socks}
ControlPort 127.0.0.1:{ctl}
HashedControlPassword {hashed}
CookieAuthentication 1
CookieAuthFile {d}/control.authcookie
{hidden}Log notice stdout
''')
    tap = ti.start([tor, '-f', str(d / 'torrc')], name, root, procs)
    tap.wait('opened control listener', 60)
    control = ti.TorControl(ctl, password)
    assert control.command('SETEVENTS STATUS_CLIENT HS_DESC')[-1].startswith('250')
    return {'dir': d, 'socks': socks, 'ctl': ctl, 'listen': listen_port, 'control': control, 'tap': tap,
            'cookie': d / 'control.authcookie'}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--fipsd', required=True)
    ap.add_argument('--rust', default='')
    ap.add_argument('--tor', default='')
    args = ap.parse_args()
    args.rust = args.rust or os.environ.get('FIPSD_UDP_INTEROP_RUST_BINARY', '')
    args.tor = args.tor or os.environ.get('FIPSD_TOR_BINARY', '')
    if not args.tor or not Path(args.tor).is_file() or not args.rust or not Path(args.rust).is_file():
        print('SKIP: set FIPSD_TOR_BINARY to a tor daemon and FIPSD_UDP_INTEROP_RUST_BINARY to a pinned Rust fips')
        return 77
    root = Path(os.environ['FIPSD_TEST_TMPDIR'])
    procs, failures, timings, rc = [], [], {}, 1
    began = time.monotonic()

    def mark(name):
        timings[name] = round(time.monotonic() - began, 1)
    try:
        port = ti.free_port()
        relay = ti.start([sys.executable, '-I', str(RELAY), '--bind', '127.0.0.1', '--port', str(port)], 'relay', root, procs)
        relay.wait('listening on', 30)
        url = f'ws://127.0.0.1:{port}'
        nostr = (f"    nostr:\n      enabled: true\n      advert_relays: ['{url}']\n      dm_relays: ['{url}']\n"
                 "      stun_servers: []\n")
        c_tor = launch_tor(args.tor, root, 'tor-c', procs, ti.free_port())
        r_tor = launch_tor(args.tor, root, 'tor-rust', procs)
        c_tor['tap'].wait('Bootstrapped 0%', 60)  # logged after tor loads (or creates) the service keys
        hostname = c_tor['dir'] / 'hs' / 'hostname'
        assert hostname.is_file(), f'{hostname} missing after tor reported Bootstrapped 0%'
        onion = hostname.read_text().strip()
        service = onion[:-len('.onion')]
        mark('tor_c_hostname_s')
        c_sock, r_sock = root / 'c.sock', root / 'r.sock'
        (root / 'c.yaml').write_text(f'''node:
  identity: {{nsec: '{'1' * 64}', persistent: false}}
  control: {{enabled: true, socket_path: '{c_sock}'}}
  log_level: info
  rendezvous:
{nostr}      advertise: true
transports:
  udp: {{bind_addr: '127.0.0.1:0', advertise_on_nostr: false}}
  tcp: {{bind_addr: '127.0.0.1:0', advertise_on_nostr: false}}
  tor:
    mode: directory
    socks5_addr: '127.0.0.1:{c_tor['socks']}'
    control_addr: '127.0.0.1:{c_tor['ctl']}'
    control_auth: 'cookie:{c_tor['cookie']}'
    advertise_on_nostr: true
    directory_service: {{hostname_file: '{hostname}', bind_addr: '127.0.0.1:{c_tor['listen']}'}}
''')
        c = ti.start([args.fipsd, '-c', str(root / 'c.yaml')], 'fipsd', root, procs)
        c.wait('control socket listening', 60)
        mark('fipsd_started_s')
        npub = ti.query(c_sock, 'show_status')['npub']
        pubkey = npub_hex(npub)
        print(f'fipsd {npub} ({pubkey}) onion {onion}')
        # Tor's own view of the upload, on the script's control connection.
        line = c_tor['control'].wait_event(lambda l: l.startswith(f'650 HS_DESC UPLOADED {service} '), DESCRIPTOR_SECS)
        assert line, f'tor-c: no HS_DESC UPLOADED for {onion} within {DESCRIPTOR_SECS}s'
        mark('tor_c_descriptor_uploaded_s')
        at = c.wait(f'{onion} advertisable (', DESCRIPTOR_SECS)
        mark('fipsd_onion_advertisable_s')
        timings['fipsd_advertisable_reason'] = bytes(c.data[at:at + 80]).decode(errors='replace').split(')')[0]
        relay.wait(f'EVENT kind=37195 pubkey={pubkey} ', 60)
        mark('advert_at_relay_s')
        events = relay_events(port, {'kinds': [37195], 'authors': [pubkey]})
        assert len(events) == 1, events
        want = ('{"identifier":"fips-overlay-v1","version":1,"endpoints":[{"transport":"tor","addr":"'
                + onion + ':443"}]}')
        assert events[0]['content'] == want, events[0]['content']
        tags = {t[0]: t[1:] for t in events[0]['tags']}
        assert tags['d'] == ['fips-overlay-v1'] and tags['protocol'] == ['fips-overlay-v1'] and tags['version'] == ['1'], tags
        assert int(tags['expiration'][0]) > events[0]['created_at'], tags
        print('advert on relay:', events[0]['content'])
        # The Rust node: no address for fipsd, only its npub and Nostr.
        phase = r_tor['control'].command('GETINFO status/bootstrap-phase')
        if not any('PROGRESS=100' in l for l in phase):
            assert r_tor['control'].wait_event(lambda l: 'BOOTSTRAP PROGRESS=100' in l, ti.BOOTSTRAP_SECS), 'tor-rust bootstrap'
        mark('tor_rust_bootstrapped_s')
        (root / 'rust.yaml').write_text(f'''node:
  identity: {{nsec: '{'2' * 64}', persistent: false}}
  control: {{enabled: true, socket_path: '{r_sock}'}}
  native_api: {{enabled: false}}
  rendezvous:
{nostr}      advertise: false
      policy: configured_only
tun: {{enabled: false}}
dns: {{enabled: false}}
transports:
  tor:
    mode: socks5
    socks5_addr: '127.0.0.1:{r_tor['socks']}'
peers:
  - npub: '{npub}'
    alias: fipsd
    via_nostr: true
    connect_policy: auto_connect
''')
        t0 = time.monotonic()
        rust = ti.start([args.rust, '-c', str(root / 'rust.yaml')], 'rust', root, procs)
        rust.wait('peer promoted to active', LINK_SECS)
        timings['rust_discover_and_link_s'] = round(time.monotonic() - t0, 1)
        c.wait('link established with', 60)
        mark('c_link_s')
        deadline = time.monotonic() + 30
        while not any(p.get('npub') == npub for p in ti.query(r_sock, 'show_peers')['peers']):
            assert time.monotonic() < deadline, 'Rust control never reports the fipsd peer'
        row = next(t for t in ti.query(c_sock, 'show_transports')['transports'] if t['type'] == 'tor')
        assert row['stats']['connections_accepted'] >= 1 and row['stats']['pool_inbound'] >= 1, row
        rrow = next(t for t in ti.query(r_sock, 'show_transports')['transports'] if t['type'] == 'tor')
        timings['rust_dial_socks5_errors'] = rrow['stats']['socks5_errors']
        timings['rust_dial_connect_timeouts'] = rrow['stats']['connect_timeouts']
        # One endpoint set, one publish: more HSDir uploads changed nothing.
        published = relay.tail(1 << 30).count(f'EVENT kind=37195 pubkey={pubkey} ')
        timings['fipsd_advert_publishes'] = published
        assert published == 1, f'fipsd published its advert {published} times'
        timings['total_s'] = round(time.monotonic() - began, 1)
        print('timings:', json.dumps(timings, sort_keys=True))
        rc = 0
    except Exception as e:  # noqa: BLE001 - report what was observed, then fail
        print('FAIL:', e)
        print('observed timings:', json.dumps(timings, sort_keys=True))
        for name, _, tap in procs:
            print(f'--- {name} log tail ---\n{tap.tail(2500)}')
    finally:
        for entry in reversed(procs):
            if not entry[0].startswith('tor-'):
                ti.stop(entry, failures)
        for t in [x for x in (locals().get('c_tor'), locals().get('r_tor')) if x]:
            t['control'].close()
        for entry in reversed([p for p in procs if p[0].startswith('tor-')]):
            ti.stop(entry, failures)
    if failures:
        print('teardown:', '; '.join(failures))
    return 1 if failures else rc


if __name__ == '__main__':
    raise SystemExit(main())
