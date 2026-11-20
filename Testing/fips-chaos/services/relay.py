#!/usr/bin/env python3
"""Minimal NIP-01 relay with a NIP-11 document for the netns chaos lab.

Standard library only, so it runs unchanged under `ip netns exec` on a CI
runner. It serves the FIPS rendezvous traffic the daemons need: replaceable
kind-37195 overlay adverts (parameterized by `d`), ephemeral kind-21059
traversal signals (never stored, delivered live to `#p` subscriptions) and
the NIP-11 document fipsd fetches (`Accept: application/nostr+json`) before
it connects. Filters honour ids/authors prefixes, kinds, `#tag`, since,
until and limit (0 = history-free); every EVENT gets an OK, every REQ an
EOSE after its history. Event ids are recomputed and verified; signatures
are not (no secp256k1 in the standard library), which the lab accepts
because both daemons verify what they receive.

Every accepted event is logged as one line the harness reads back as stage
evidence: `EVENT kind=<k> pubkey=<hex> id=<hex> p=<hex,...>`.
"""
import argparse
import asyncio
import base64
import hashlib
import json
import struct
import sys
import time

WS_GUID = b'258EAFA5-E914-47DA-95CA-C5AB0DC85B11'
MAX_MESSAGE = 1 << 20
NIP11 = {
    'name': 'fips-chaos-relay',
    'description': 'netns chaos lab relay (Testing/fips-chaos)',
    'supported_nips': [1, 11, 40],
    'software': 'Testing/fips-chaos/services/relay.py',
    'limitation': {'max_filters': 16, 'max_subscriptions': 32, 'max_limit': 500,
                   'auth_required': False, 'payment_required': False},
}


def log(line):
    print(f'{time.strftime("%H:%M:%S")} relay: {line}', flush=True)


def event_id(ev):
    serial = json.dumps([0, ev['pubkey'], ev['created_at'], ev['kind'], ev['tags'], ev['content']],
                        separators=(',', ':'), ensure_ascii=False)
    return hashlib.sha256(serial.encode()).hexdigest()


def is_hex(s, n):
    return isinstance(s, str) and len(s) == n and all(c in '0123456789abcdef' for c in s)


def validate(ev):
    if not isinstance(ev, dict):
        return 'invalid: event is not an object'
    for key in ('id', 'pubkey', 'created_at', 'kind', 'tags', 'content', 'sig'):
        if key not in ev:
            return f'invalid: missing {key}'
    if not is_hex(ev['id'], 64) or not is_hex(ev['pubkey'], 64) or not is_hex(ev['sig'], 128):
        return 'invalid: malformed id/pubkey/sig'
    if not isinstance(ev['created_at'], int) or not isinstance(ev['kind'], int) or not isinstance(ev['content'], str):
        return 'invalid: malformed created_at/kind/content'
    if not isinstance(ev['tags'], list) or not all(isinstance(t, list) and all(isinstance(x, str) for x in t) for t in ev['tags']):
        return 'invalid: malformed tags'
    if event_id(ev) != ev['id']:
        return 'invalid: event id does not match the serialized event'
    return None


def tag_values(ev, name):
    return [t[1] for t in ev['tags'] if len(t) >= 2 and t[0] == name]


def matches(f, ev):
    if 'ids' in f and not any(ev['id'].startswith(p) for p in f['ids']):
        return False
    if 'authors' in f and not any(ev['pubkey'].startswith(p) for p in f['authors']):
        return False
    if 'kinds' in f and ev['kind'] not in f['kinds']:
        return False
    if 'since' in f and ev['created_at'] < f['since']:
        return False
    if 'until' in f and ev['created_at'] > f['until']:
        return False
    for key, wanted in f.items():
        if key.startswith('#') and len(key) == 2:
            if not set(tag_values(ev, key[1])) & set(wanted):
                return False
    return True


class Store:
    def __init__(self):
        self.events = {}       # id -> event
        self.replaceable = {}  # (pubkey, kind[, d]) -> id

    def admit(self, ev):
        """Returns (ok, message, deliver_live)."""
        kind = ev['kind']
        if ev['id'] in self.events:
            return True, 'duplicate: already have this event', False
        if 20000 <= kind < 30000:
            return True, '', True
        key = None
        if kind in (0, 3) or 10000 <= kind < 20000:
            key = (ev['pubkey'], kind)
        elif 30000 <= kind < 40000:
            d = tag_values(ev, 'd')
            key = (ev['pubkey'], kind, d[0] if d else '')
        if key is not None:
            old = self.events.get(self.replaceable.get(key))
            if old and (old['created_at'], -int(old['id'], 16)) >= (ev['created_at'], -int(ev['id'], 16)):
                return True, 'duplicate: a newer or equal replaceable event is stored', False
            if old:
                del self.events[old['id']]
            self.replaceable[key] = ev['id']
        self.events[ev['id']] = ev
        return True, '', True

    def query(self, filters):
        out = {}
        for f in filters:
            rows = sorted((e for e in self.events.values() if matches(f, e)),
                          key=lambda e: (-e['created_at'], e['id']))
            if 'limit' in f:
                rows = rows[:max(0, int(f['limit']))]
            for e in rows:
                out[e['id']] = e
        return sorted(out.values(), key=lambda e: (-e['created_at'], e['id']))


class Conn:
    def __init__(self, writer, peer):
        self.writer = writer
        self.peer = peer
        self.subs = {}

    def send(self, message):
        send_frame(self.writer, 0x1, json.dumps(message, separators=(',', ':')).encode())


def send_frame(writer, opcode, payload):
    head = bytes([0x80 | opcode])
    n = len(payload)
    if n < 126:
        head += bytes([n])
    elif n < 65536:
        head += bytes([126]) + struct.pack('!H', n)
    else:
        head += bytes([127]) + struct.pack('!Q', n)
    writer.write(head + payload)


async def read_frame(reader):
    b1, b2 = await reader.readexactly(2)
    fin, opcode, masked, n = b1 & 0x80, b1 & 0x0f, b2 & 0x80, b2 & 0x7f
    if n == 126:
        n = struct.unpack('!H', await reader.readexactly(2))[0]
    elif n == 127:
        n = struct.unpack('!Q', await reader.readexactly(8))[0]
    if n > MAX_MESSAGE:
        raise ValueError(f'frame of {n} bytes exceeds the {MAX_MESSAGE} byte bound')
    mask = await reader.readexactly(4) if masked else None
    payload = await reader.readexactly(n)
    if mask:
        payload = bytes(b ^ mask[i & 3] for i, b in enumerate(payload))
    return bool(fin), opcode, payload


async def read_message(reader):
    """One complete text message, handling ping/pong/close and continuation."""
    buf, opcode0 = b'', None
    while True:
        fin, opcode, payload = await read_frame(reader)
        if opcode == 0x8:
            return None
        if opcode == 0x9:
            return ('ping', payload)
        if opcode == 0xA:
            continue
        if opcode in (0x1, 0x2):
            buf, opcode0 = payload, opcode
        elif opcode == 0x0:
            buf += payload
        else:
            raise ValueError(f'unsupported opcode {opcode}')
        if len(buf) > MAX_MESSAGE:
            raise ValueError('message exceeds the size bound')
        if fin:
            return ('text', buf) if opcode0 == 0x1 else ('binary', buf)


class Relay:
    def __init__(self):
        self.store = Store()
        self.conns = set()

    def deliver(self, ev):
        for conn in list(self.conns):
            for sub, filters in conn.subs.items():
                if any(matches(f, ev) for f in filters):
                    try:
                        conn.send(['EVENT', sub, ev])
                    except Exception as e:  # noqa: BLE001 - a dead client must not stop delivery
                        log(f'deliver to {conn.peer} sub={sub} failed: {e}')

    def handle(self, conn, message):
        if not isinstance(message, list) or not message or not isinstance(message[0], str):
            conn.send(['NOTICE', 'error: malformed message'])
            return
        verb = message[0]
        if verb == 'EVENT' and len(message) == 2:
            ev = message[1]
            problem = validate(ev)
            if problem:
                log(f'EVENT rejected from {conn.peer}: {problem}')
                conn.send(['OK', ev.get('id', '') if isinstance(ev, dict) else '', False, problem])
                return
            ok, reason, live = self.store.admit(ev)
            log(f'EVENT kind={ev["kind"]} pubkey={ev["pubkey"]} id={ev["id"]} '
                f'p={",".join(tag_values(ev, "p"))} d={",".join(tag_values(ev, "d"))} '
                f'created_at={ev["created_at"]} ok={str(ok).lower()}{" " + reason if reason else ""}')
            conn.send(['OK', ev['id'], ok, reason])
            if ok and live:
                self.deliver(ev)
        elif verb == 'REQ' and len(message) >= 3 and isinstance(message[1], str):
            sub, filters = message[1], [f for f in message[2:] if isinstance(f, dict)]
            conn.subs[sub] = filters
            history = self.store.query(filters)
            log(f'REQ from {conn.peer} sub={sub} filters={json.dumps(filters, separators=(",", ":"))} history={len(history)}')
            for ev in history:
                conn.send(['EVENT', sub, ev])
            conn.send(['EOSE', sub])
        elif verb == 'CLOSE' and len(message) == 2:
            conn.subs.pop(message[1], None)
            log(f'CLOSE from {conn.peer} sub={message[1]}')
        else:
            conn.send(['NOTICE', f'error: unsupported message {verb}'])

    async def serve(self, reader, writer):
        peer = '%s:%s' % writer.get_extra_info('peername')[:2]
        try:
            head = await asyncio.wait_for(reader.readuntil(b'\r\n\r\n'), 30)
            lines = head.decode('latin-1').split('\r\n')
            headers = {}
            for line in lines[1:]:
                if ':' in line:
                    k, v = line.split(':', 1)
                    headers[k.strip().lower()] = v.strip()
            if 'websocket' in headers.get('upgrade', '').lower() and 'sec-websocket-key' in headers:
                accept = base64.b64encode(hashlib.sha1(headers['sec-websocket-key'].encode() + WS_GUID).digest()).decode()
                writer.write(('HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n'
                              f'Sec-WebSocket-Accept: {accept}\r\n\r\n').encode())
                await writer.drain()
                await self.websocket(reader, writer, peer)
                return
            if 'application/nostr+json' in headers.get('accept', ''):
                body = json.dumps(NIP11).encode()
                ctype = 'application/nostr+json'
                log(f'NIP-11 served to {peer}')
            else:
                body, ctype = b'fips chaos relay: connect with a websocket client\n', 'text/plain'
            writer.write((f'HTTP/1.1 200 OK\r\nContent-Type: {ctype}\r\nContent-Length: {len(body)}\r\n'
                          'Access-Control-Allow-Origin: *\r\nConnection: close\r\n\r\n').encode() + body)
            await writer.drain()
        except (asyncio.IncompleteReadError, asyncio.TimeoutError, ConnectionError, ValueError) as e:
            log(f'connection {peer} ended: {type(e).__name__}: {e}')
        finally:
            writer.close()

    async def websocket(self, reader, writer, peer):
        conn = Conn(writer, peer)
        self.conns.add(conn)
        log(f'websocket open {peer}')
        try:
            while True:
                item = await read_message(reader)
                if item is None:
                    send_frame(writer, 0x8, b'')
                    await writer.drain()
                    break
                kind, payload = item
                if kind == 'ping':
                    send_frame(writer, 0xA, payload)
                elif kind == 'text':
                    try:
                        message = json.loads(payload)
                    except ValueError:
                        conn.send(['NOTICE', 'error: invalid JSON'])
                        continue
                    self.handle(conn, message)
                await writer.drain()
        except (asyncio.IncompleteReadError, ConnectionError, ValueError) as e:
            log(f'websocket {peer} ended: {type(e).__name__}: {e}')
        finally:
            self.conns.discard(conn)
            log(f'websocket closed {peer}')


async def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--bind', default='127.0.0.1')
    ap.add_argument('--port', type=int, default=7777)
    args = ap.parse_args()
    relay = Relay()
    server = await asyncio.start_server(relay.serve, args.bind, args.port, reuse_address=True)
    log(f'listening on ws://{args.bind}:{args.port}')
    async with server:
        await server.serve_forever()


if __name__ == '__main__':
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        sys.exit(0)
