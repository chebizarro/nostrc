#!/usr/bin/env python3
"""Real fipsd entrypoint + upstream adapter fetch tests. No sleeps."""
import argparse
from collections import Counter
import json
import os
from pathlib import Path
import selectors
import socket
import subprocess
import tempfile
import threading
import time

from fipsctl_e2e import start_fipsd, reserve_port
from udp_interop import SANITIZER_MARKERS
from fipstop_golden import TABS


class Fake:
    def __init__(self, path, replies):
        self.path, self.replies = path, replies
        self.counts = Counter()
        self.errors = []
        self.listener = socket.socket(socket.AF_UNIX)
        self.listener.bind(str(path))
        self.listener.listen(16)
        self.reader, self.writer = socket.socketpair()
        self.thread = threading.Thread(target=self.serve)
        self.thread.start()

    def serve(self):
        try:
            with selectors.DefaultSelector() as sel:
                sel.register(self.listener, selectors.EVENT_READ)
                sel.register(self.reader, selectors.EVENT_READ)
                while True:
                    ready = sel.select()
                    if any(k.fileobj is self.reader for k, _ in ready):
                        return
                    conn, _ = self.listener.accept()
                    with conn:
                        conn.settimeout(5)
                        request = json.loads(conn.makefile('rb').readline())
                        cmd = request['command']
                        self.counts[cmd] += 1
                        value = self.replies(cmd) if callable(self.replies) else self.replies.get(cmd, {})
                        if value is not None:
                            conn.sendall(json.dumps({'status': 'ok', 'data': value}).encode() + b'\n')
        except Exception as e:
            self.errors.append(e)

    def close(self):
        self.writer.sendall(b'x')
        self.thread.join(6)
        assert not self.thread.is_alive(), 'fake did not stop'
        self.listener.close()
        self.reader.close()
        self.writer.close()
        self.path.unlink()
        assert not self.errors, self.errors


def run(binary, *args):
    result = subprocess.run([binary, *map(str, args)], capture_output=True, text=True, timeout=20)
    assert result.returncode == 0, (args, result.returncode, result.stderr)
    assert not any(m.decode() in result.stderr for m in SANITIZER_MARKERS), result.stderr
    return result.stdout


def fetch(binary, path, count):
    return [json.loads(s) for s in run(binary, '--fetch', path, count).splitlines()]


def adapter_cases(unit, root):
    control = root / 'node.sock'
    adapter_path = root / 'node-ipv6tun.sock'
    node = Fake(control, {'show_status': {'npub': 'npub1node'}})
    adapter = Fake(adapter_path, {'show_status': {'tun_state': 'active', 'tun_name': 'fips-a',
        'effective_ipv6_mtu': 1323}, 'show_listening_sockets': {'fips0_addr': 'fd00::a', 'sockets': []}})
    try:
        for path in (control, root / 'link.sock'):
            if path != control:
                path.symlink_to(control)
            result = fetch(unit, path, 1)[0]
            assert result['status']['npub'] == 'npub1node'
            assert result['status']['tun_name'] == 'fips-a'
            assert result['status']['effective_ipv6_mtu'] == 1323
            assert result['listening']['fips0_addr'] == 'fd00::a'
    finally:
        node.close()
        adapter.close()
    # An absent adapter is skipped for precisely the next ten fetches.
    appeared = []
    def reply(command):
        if command == 'show_status':
            return {'tun_state': 'disabled', 'tun_name': '-', 'effective_ipv6_mtu': 1203}
        if not appeared:
            appeared.append(Fake(adapter_path, {'show_listening_sockets': {'fips0_addr': 'fd00::d', 'sockets': []}}))
        return {'fips0_addr': 'fd00::c', 'sockets': []}
    node = Fake(control, reply)
    try:
        results = fetch(unit, control, 12)
        assert [x['quiet'] for x in results] == list(range(10, -1, -1)) + [0]
        assert [x['listening']['fips0_addr'] for x in results] == ['fd00::c'] * 11 + ['fd00::d']
        assert appeared[0].counts['show_listening_sockets'] == 1
    finally:
        node.close()
        for server in appeared:
            server.close()
    # A reached adapter which closes the panel request keeps its status;
    # this is not an unreachable socket and must not trigger quiet mode.
    node = Fake(control, {'show_status': {'npub': 'npub1node'}, 'show_listening_sockets': None})
    adapter = Fake(adapter_path, {'show_status': {'tun_state': 'active', 'tun_name': 'fips0',
        'effective_ipv6_mtu': 1323}, 'show_listening_sockets': None})
    try:
        results = fetch(unit, control, 3)
        assert all(x['status']['tun_name'] == 'fips0' and x['listening'] is None and x['quiet'] == 0 for x in results)
        assert sum(adapter.counts.values()) == 6
    finally:
        node.close()
        adapter.close()
    print('upstream adapter cases: sibling/symlink composition, fallback, ten-fetch quiet, failed panel passed')


def main():
    p = argparse.ArgumentParser()
    p.add_argument('--fipsd', required=True)
    p.add_argument('--fipstop', required=True)
    p.add_argument('--unit', required=True)
    args = p.parse_args()
    repo = Path(__file__).resolve().parents[2]
    with tempfile.TemporaryDirectory(prefix='.ft-', dir=repo) as directory:
        root = Path(directory)
        adapter_cases(args.unit, root)
        env = dict(os.environ, FIPSD_WORKERS='2', NO_COLOR='1')
        node = start_fipsd('live', args.fipsd, root, env, reserve_port(socket.AF_INET), '1' * 64)
        try:
            node.daemon.await_log('fipsd running', time.monotonic() + 30)
            status = node.query('show_status')
            for size in ('80x24', '120x40'):
                for tab in TABS:
                    frame = run(args.fipstop, '-s', node.socket, '--render-once', size, '--tab', tab)
                    assert 'Connected' in frame and 'Refresh: 2s' in frame, (tab, frame)
                    assert len(frame.splitlines()) == int(size.split('x')[1])
                    if tab == 'Node':
                        assert status['npub'] in frame, frame
            frame = run(args.fipstop, '-s', root / 'missing.sock', '--render-once', '80x24')
            assert 'Disconnected' in frame
        finally:
            rc = node.daemon.stop(20)
            assert rc == 0 and not node.daemon.sanitizer_report(), node.daemon.text()[-4000:]
    print('fipstop live e2e: ten panes x two sizes, disconnected frame, clean daemon shutdown passed')


if __name__ == '__main__':
    main()
