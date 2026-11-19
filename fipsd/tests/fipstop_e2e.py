#!/usr/bin/env python3
"""Real fipsd entrypoint + upstream adapter fetch tests. No sleeps."""
import argparse
from collections import Counter
import faulthandler
import json
import fcntl
import pty
import re
import signal
import struct
import termios
import os
from pathlib import Path
import selectors
import socket
import subprocess
import sys
import tempfile
import threading
import time

from fipsctl_e2e import start_fipsd, reserve_port
from udp_interop import SANITIZER_MARKERS, kill_children
from fipstop_golden import TABS

# Below CTest's TIMEOUT for FipsTopE2eTest (180 s, fipsd/CMakeLists.txt); a
# normal run takes seconds. See watchdog().
WATCHDOG_SECONDS = 150
TERMINALS = {}  # live PTY fipstop children -> their output so far


def watchdog(seconds):
    """End a run that stops making progress with a diagnosis, never a silent stall.

    Every wait below has its own deadline; this covers whatever does not (a
    blocked syscall, a wedged thread). On expiry: every thread's stack, the
    tail of each live PTY's output, then SIGKILL the children and exit 1."""
    def expire():
        out = sys.stderr
        out.write(f'fipstop e2e: no result within {seconds} s; thread stacks follow\n')
        out.flush()
        faulthandler.dump_traceback(file=out, all_threads=True)
        for process, output in list(TERMINALS.items()):
            out.write(f'fipstop pid {process.pid} alive={process.poll() is None} PTY tail: '
                      f'{bytes(output[-2000:]).decode(errors="replace")!r}\n')
            if process.poll() is None:
                process.kill()
        kill_children()  # fipsd started through start_fipsd
        out.flush()
        os._exit(1)
    timer = threading.Timer(seconds, expire)
    timer.daemon = True
    timer.start()
    return timer


class Fake:
    """A scripted control socket: one request line, one reply line per connection.

    A failed request is recorded for close() to report, without ending the
    server, so later requests still get answers. The thread is a daemon and
    accept() never blocks, so a fake that is never closed cannot hold the
    interpreter's exit."""

    def __init__(self, path, replies):
        self.path, self.replies = path, replies
        self.counts = Counter()
        self.errors = []
        self.closed = False
        self.listener = socket.socket(socket.AF_UNIX)
        self.listener.bind(str(path))
        self.listener.listen(16)
        self.listener.setblocking(False)  # a client gone before accept() must not wedge serve()
        self.reader, self.writer = socket.socketpair()
        self.thread = threading.Thread(target=self.serve, name=f'fake {path.name}', daemon=True)
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
                    try:
                        conn, _ = self.listener.accept()
                    except BlockingIOError:
                        continue
                    try:
                        self.answer(conn)
                    except Exception as e:
                        self.errors.append(e)
        except Exception as e:
            self.errors.append(e)

    def answer(self, conn):
        with conn:
            conn.settimeout(5)
            request = json.loads(conn.makefile('rb').readline())
            cmd = request['command']
            self.counts[cmd] += 1
            value = self.replies(cmd) if callable(self.replies) else self.replies.get(cmd, {})
            if value is not None:
                conn.sendall(json.dumps({'status': 'ok', 'data': value}).encode() + b'\n')

    def close(self):
        """Stop and release everything first, then report what went wrong."""
        if self.closed:
            return
        self.closed = True
        self.writer.sendall(b'x')
        self.thread.join(6)
        stuck = self.thread.is_alive()
        self.listener.close()
        self.reader.close()
        self.writer.close()
        self.path.unlink(missing_ok=True)
        assert not stuck, f'fake {self.path} did not stop; served {dict(self.counts)}, errors {self.errors}'
        assert not self.errors, (f'fake {self.path} served {dict(self.counts)}', self.errors)


def close_all(fakes):
    """Close every fake, then raise the first failure: one bad close must not skip the rest."""
    failures = []
    for fake in fakes:
        try:
            fake.close()
        except AssertionError as e:
            failures.append(e)
    if failures:
        raise failures[0]


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
        close_all([node, adapter])
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
        close_all([node, *appeared])
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
        close_all([node, adapter])
    print('upstream adapter cases: sibling/symlink composition, fallback, ten-fetch quiet, failed panel passed')


def terminal_case(binary, root, stop):
    master, slave = pty.openpty()
    original = termios.tcgetattr(slave)
    fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack('HHHH', 24, 80, 0, 0))
    env = dict(os.environ, TERM='xterm-256color', NO_COLOR='1', LC_ALL='en_US.UTF-8')
    # EOF on `exited` is the child's exit (the kernel closes its descriptors).
    exited, held = os.pipe()
    try:
        process = subprocess.Popen([binary, '--fixture-dir', str(root / 'fipsd/tests/control_golden')],
                                   stdin=slave, stdout=slave, stderr=slave, env=env, close_fds=True, pass_fds=(held,))
    finally:
        os.close(held)
    transcript = TERMINALS[process] = bytearray()
    def read():
        chunk = os.read(master, 65536)
        transcript.extend(chunk)
        return chunk
    def until(needle):
        output = bytearray()
        deadline = time.monotonic() + 10
        with selectors.DefaultSelector() as sel:
            sel.register(master, selectors.EVENT_READ)
            while needle not in output:
                remaining = deadline - time.monotonic()
                assert remaining > 0 and sel.select(remaining), ('terminal output missing', needle, output)
                output.extend(read())
        return bytes(output)
    def drain_until_exit():
        # Read like a terminal until the process is gone. ncurses restores the
        # shell mode with TCSADRAIN, which on a PTY waits until the master has
        # read everything written; endwin's last bytes can trail its rmcup in a
        # later read, so a reader that stops at rmcup deadlocks the exit.
        output = bytearray()
        deadline = time.monotonic() + 10
        with selectors.DefaultSelector() as sel:
            sel.register(master, selectors.EVENT_READ)
            sel.register(exited, selectors.EVENT_READ)
            while True:
                remaining = deadline - time.monotonic()
                ready = sel.select(remaining) if remaining > 0 else []
                assert ready, ('fipstop did not exit', output[-2000:])
                if any(key.fileobj == master for key, _ in ready):
                    output.extend(read())
                elif not os.read(exited, 1):
                    break
            sel.unregister(exited)
            while sel.select(0):  # what it wrote last, before exiting
                output.extend(read())
        return bytes(output)
    try:
        output = until(b'Refresh:')
        assert not re.search(rb'\x1b\[(?:3[0-7]|4[0-7]|38;|48;)', output), output
        fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack('HHHH', 40, 120, 0, 0))
        process.send_signal(signal.SIGWINCH)
        until(b'Gateway')
        if stop == 'q':
            os.write(master, b'q')
        else:
            process.send_signal(stop)
        output = drain_until_exit()
        assert b'\x1b[?1049l' in output, ('endwin not observed', output[-2000:])
        assert not any(m in output for m in SANITIZER_MARKERS), output.decode(errors='replace')
        assert process.wait(timeout=10) == 0
        current = termios.tcgetattr(slave)
        # PENDIN describes the kernel's pending-input queue, not a terminal mode;
        # macOS sets it when q is injected. Check every actual setting unchanged.
        pending = getattr(termios, 'PENDIN', 0)
        original[3] &= ~pending
        current[3] &= ~pending
        assert current == original, ('terminal modes were not restored', original, current)
    finally:
        if process.poll() is None:
            process.kill()
        os.close(master)  # hangs up the PTY: nothing can stay blocked draining into it
        process.wait(timeout=5)
        TERMINALS.pop(process, None)
        os.close(slave)
        os.close(exited)
    print(f'real ncurses PTY: initial frame, NO_COLOR, resize, stop={stop}, terminal restoration passed')


def main():
    p = argparse.ArgumentParser()
    p.add_argument('--fipsd', required=True)
    p.add_argument('--fipstop', required=True)
    p.add_argument('--unit', required=True)
    args = p.parse_args()
    watchdog(WATCHDOG_SECONDS)
    repo = Path(__file__).resolve().parents[2]
    fixtures = repo / 'fipsd/tests/control_golden'
    left = run(args.fipstop, '--fixture-dir', fixtures, '--render-once', '80x24')
    right = run(args.fipstop, f'--fixture-dir={fixtures}', '--render-once=80x24', '-r2')
    assert left == right, 'long equals and attached short flags differ'
    for flags in (['--refresh=-1'], ['--render-once=999999999999999999999x24'], ['--render-once=1x0'], ['--bogus']):
        result = subprocess.run([args.fipstop, *flags], capture_output=True, timeout=10)
        assert result.returncode == 1 and b'error:' in result.stderr

    for stop in ('q', signal.SIGTERM, signal.SIGHUP):
        terminal_case(args.fipstop, repo, stop)
    failed = subprocess.run([args.fipstop, '-s', str(repo / '.missing.sock')], stdin=subprocess.DEVNULL, capture_output=True, timeout=10)
    assert failed.returncode == 1 and b'terminal required' in failed.stderr
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
                    frame = run(args.fipstop, '-s', node.socket, '--gateway-socket', root / 'gateway.sock', '--render-once', size, '--tab', tab)
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
