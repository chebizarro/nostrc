#!/usr/bin/env python3
"""Local wire fixtures for real relay-pool and signetctl entrypoints (stdlib only)."""
import base64
import hashlib
import json
import os
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import threading


class Relay:
    def __init__(self, handler, on_open=None):
        self.handler = handler
        self.on_open = on_open
        self.listener = socket.socket()
        self.listener.bind(('127.0.0.1', 0))
        self.listener.listen(1)
        self.listener.settimeout(20)
        self.url = 'ws://127.0.0.1:%d' % self.listener.getsockname()[1]
        self.sock = None
        self.lock = threading.Lock()
        self.error = None
        self.closing = False
        self.thread = threading.Thread(target=self.run, daemon=True)
        self.thread.start()

    def exact(self, n):
        data = b''
        while len(data) < n:
            chunk = self.sock.recv(n - len(data))
            if not chunk:
                raise EOFError()
            data += chunk
        return data

    def send(self, obj):
        data = json.dumps(obj, separators=(',', ':')).encode()
        header = bytes([0x81, len(data)]) if len(data) < 126 else b'\x81\x7e' + struct.pack('!H', len(data))
        with self.lock:
            self.sock.sendall(header + data)

    def run(self):
        while not self.closing and not self.error:
            self.connection()
            if self.sock:
                self.sock.close()
                self.sock = None

    def connection(self):
        try:
            self.sock, _ = self.listener.accept()
            self.sock.settimeout(20)
            head = b''
            while not head.endswith(b'\r\n\r\n'):
                head += self.exact(1)
            key = next(line.split(b':', 1)[1].strip() for line in head.split(b'\r\n')
                       if line.lower().startswith(b'sec-websocket-key:'))
            accept = base64.b64encode(hashlib.sha1(key + b'258EAFA5-E914-47DA-95CA-C5AB0DC85B11').digest())
            self.sock.sendall(b'HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: ' + accept + b'\r\n\r\n')
            if self.on_open:
                self.on_open(self)
            while True:
                first, second = self.exact(2)
                size = second & 127
                if size == 126:
                    size = struct.unpack('!H', self.exact(2))[0]
                elif size == 127:
                    size = struct.unpack('!Q', self.exact(8))[0]
                mask = self.exact(4) if second & 128 else None
                data = self.exact(size)
                if mask:
                    data = bytes(v ^ mask[i % 4] for i, v in enumerate(data))
                if first & 15 == 8:
                    break
                if first & 15 == 1:
                    self.handler(self, json.loads(data))
        except (EOFError, ConnectionResetError, BrokenPipeError):
            pass
        except Exception as exc:
            if not self.closing:
                self.error = exc

    def close(self):
        self.closing = True
        sock = self.sock
        if sock:
            try:
                sock.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
            sock.close()
        # Closing a listening FD on another thread need not wake accept().
        # A local EOF connection explicitly wakes it without a timer/sleep.
        try:
            with socket.create_connection(self.listener.getsockname(), timeout=1):
                pass
        except OSError:
            pass
        self.thread.join(2)
        self.listener.close()
        assert not self.thread.is_alive(), 'fixture acceptor failed to stop'
        if self.error:
            raise self.error


def pool_wire(binary, timeout=False):
    shared = {'a_sid': None, 'old_sid': None, 'signed': None, 'events': 0}
    b_subscribed = threading.Event()
    a = None

    def primary(relay, frame):
        if frame[0] == 'REQ':
            sid, filt = frame[1:3]
            if filt['kinds'] == [12346]:
                relay.send(['CLOSED', sid, 'blocked: fixture-complete'])
                return
            assert filt['kinds'] == [12345]
            assert filt['#p'] == [('a' if filt['since'] == 313131 else 'b') * 64]
            shared['old_sid'], shared['a_sid'] = shared['a_sid'], sid
            if filt['since'] == 424242:
                assert b_subscribed.wait(10), 'secondary never subscribed'
                if shared['old_sid']:
                    relay.send(['EOSE', shared['old_sid']])
            relay.send(['EOSE', sid])
        elif frame[0] == 'EVENT':
            shared['events'] += 1
            shared['signed'] = frame[1]
            if not timeout:
                relay.send(['OK', frame[1]['id'], False, 'blocked: fixture'])

    def secondary(relay, frame):
        if frame[0] == 'REQ':
            shared['b_sid'] = frame[1]
            b_subscribed.set()
        elif frame[0] == 'EVENT':
            shared['events'] += 1
            assert frame[1] == shared['signed'], 'failover changed already-signed event'
            relay.send(['OK', frame[1]['id'], True, ''])
            # Same connection orders OK before EVENT. The driver sends a
            # barrier REQ only after observing acceptance; primary CLOSED is
            # sent in response to that barrier, not to a cross-socket send.
            relay.send(['EVENT', shared['b_sid'], frame[1]])

    a = Relay(primary)
    b = Relay(secondary)
    try:
        result = subprocess.run([binary, '--wire-timeout' if timeout else '--wire', a.url, b.url], capture_output=True, text=True, timeout=25)
        assert result.returncode == 0, result.stdout + result.stderr
        assert shared['events'] == 2, shared
        print(result.stdout.strip())
    finally:
        a.close()
        b.close()


def reconnect_wire(binary):
    state = {'connections': 0, 'auth': 0}

    def opened(relay):
        state['connections'] += 1
        if state['connections'] == 1:
            relay.send(['AUTH', 'retired-generation-challenge'])

    def handler(relay, frame):
        if frame[0] == 'AUTH':
            state['auth'] += 1
            assert state['connections'] == 1, 'AUTH leaked into the challenge-free generation'
            relay.sock.shutdown(socket.SHUT_RDWR)  # drop before AUTH OK
        elif frame[0] == 'REQ':
            assert frame[2]['#p'] == ['a' * 64]
            assert frame[2]['since'] == 313131
            if state['connections'] > 1:
                relay.send(['EOSE', frame[1]])
            else:
                relay.send(['CLOSED', frame[1], 'auth-required: fixture'])

    relay = Relay(handler, opened)
    try:
        result = subprocess.run([binary, '--wire-reconnect', relay.url],
                                capture_output=True, text=True, timeout=25)
        assert result.returncode == 0, result.stdout + result.stderr
        assert state['connections'] >= 2 and state['auth'] == 1, state
        print(result.stdout.strip())
    finally:
        relay.close()


def cli_wire(binary, cancel=False):
    subscribed = threading.Event()
    state = {'boundary': False, 'authed': False, 'events': 0}
    challenge = 'full-challenge-' + 'z' * 160

    def handler(relay, frame):
        if frame[0] == 'REQ':
            assert frame[2]['kinds'] == [1059]
            assert len(frame[2]['#p'][0]) == 64
            assert frame[2]['since'] > 0
            subscribed.set()
            if cancel:
                return
            if not state['authed']:
                relay.send(['AUTH', challenge])
            else:
                state['boundary'] = True
                relay.send(['EOSE', frame[1]])
        elif frame[0] == 'AUTH':
            tags = dict(tag[:2] for tag in frame[1]['tags'])
            assert tags['challenge'] == challenge, 'AUTH challenge was truncated'
            state['authed'] = True
            relay.send(['OK', frame[1]['id'], True, ''])
        elif frame[0] == 'EVENT':
            assert state['boundary'], 'CLI published before current EOSE'
            state['events'] += 1
            relay.send(['OK', frame[1]['id'], False, 'blocked: fixture'])

    def unusable(r, frame):
        if frame[0] == 'REQ':
            r.send(['CLOSED', frame[1], 'blocked: unusable-secondary'])
        elif frame[0] == 'EVENT':
            raise AssertionError('CLI published to a relay without reply catch-up')

    relay = Relay(handler, None if cancel else lambda r: r.send(['AUTH', challenge]))
    bad = None if cancel else Relay(unusable)
    try:
        with tempfile.TemporaryDirectory() as directory:
            key = os.path.join(directory, 'provisioner.key')
            config = os.path.join(directory, 'operator.conf')
            with open(key, 'w') as f:
                f.write('0' * 63 + '2')
            os.chmod(key, 0o600)
            with open(config, 'w') as f:
                f.write('[nostr]\nrelays = %s\nbunker_pubkey = %s\n' %
                        (relay.url + (',' + bad.url if bad else ''), '79be667ef9dcbbac55a06295ce870b07029bfcdb2dce28d959f2815b16f81798'))
            env = dict(os.environ, SIGNET_PROVISIONER_NSEC_FILE=key)
            env.pop('NOSTR_TEST_MODE', None)
            process = subprocess.Popen([binary, '-c', config, 'list'], env=env,
                                       stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            if cancel:
                assert subscribed.wait(10), 'CLI never installed reply subscription'
                process.send_signal(signal.SIGTERM)
            out, err = process.communicate(timeout=20)
            assert process.returncode != 0, out + err
            assert state['events'] == (0 if cancel else 1), (state, out, err)
            assert ('cancelled' if cancel else 'relay rejected') in err, err
            print('CLI wire: %s PASS' % ('cancellation without publish' if cancel else 'AUTH/full filter/current EOSE/rejection/no retry'))
    finally:
        relay.close()
        if bad:
            bad.close()


if __name__ == '__main__':
    pool_wire(sys.argv[1])
    pool_wire(sys.argv[1], timeout=True)
    reconnect_wire(sys.argv[1])
    cli_wire(sys.argv[2])
    cli_wire(sys.argv[2], cancel=True)
