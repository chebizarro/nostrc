#!/usr/bin/env python3
"""Optional Rust/C screen comparison through tmux, driven by output events."""
import argparse
import difflib
import json
import os
from pathlib import Path
import selectors
import shlex
import shutil
import socket
import subprocess
import tempfile
import time
from fipsctl_e2e import start_fipsd, reserve_port
from fipstop_golden import TABS
from fipstop_e2e import Fake

MARKERS = ('Runtime', 'Peers (', 'Transports (', 'Sessions (', 'Tree Position',
           'Bloom Filter State', 'Link MMP', 'Routing State', 'Graphs — Node', 'Gateway not running')


class Screens:
    def __init__(self, root, w, h, binaries, control):
        self.tmux = shutil.which('tmux')
        assert self.tmux, 'tmux is required when pyte is unavailable'
        self.socket = root / 'tmux.sock'
        self.client = None
        self.selector = None
        self.w, self.h = w, h
        self.call('new-session', '-d', '-s', 'ft', '-n', 'c', '-x', w, '-y', h,
                  shlex.join(['env', 'TERM=xterm-256color', 'LC_ALL=en_US.UTF-8', 'NO_COLOR=1', binaries[0], '-s', str(control)]))
        self.call('set-option', '-g', 'status', 'off')
        self.call('set-option', '-g', 'remain-on-exit', 'on')
        self.call('new-window', '-d', '-t', 'ft', '-n', 'rust',
                  shlex.join(['env', 'TERM=xterm-256color', 'LC_ALL=en_US.UTF-8', 'NO_COLOR=1', binaries[1], '-s', str(control)]))
        for target in ('ft:c', 'ft:rust'):
            self.call('set-window-option', '-t', target, 'window-size', 'manual')
            self.call('resize-window', '-t', target, '-x', w, '-y', h)
        self.client = subprocess.Popen([self.tmux, '-S', str(self.socket), '-C', 'attach-session', '-t', 'ft'],
                                       stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        self.selector = selectors.DefaultSelector()
        self.selector.register(self.client.stdout, selectors.EVENT_READ)

    def call(self, *args):
        return subprocess.check_output([self.tmux, '-S', str(self.socket), *map(str, args)], text=True, timeout=5)

    def screen(self, target):
        text = self.call('capture-pane', '-p', '-t', target)
        lines = text.splitlines()
        return '\n'.join(line.ljust(self.w)[:self.w] for line in lines[:self.h]) + '\n'

    def await_screen(self, target, marker):
        deadline = time.monotonic() + 15
        self.call('select-window', '-t', target)
        while True:
            frame = self.screen(target)
            if marker in frame and 'Refresh:' in frame and len(frame.splitlines()) == self.h:
                return frame
            remaining = deadline - time.monotonic()
            assert remaining > 0, f'{target}: no {marker!r}:\n{frame}'
            events = self.selector.select(remaining)
            assert events, f'{target}: no terminal output: {frame}'
            data = os.read(self.client.stdout.fileno(), 65536)
            assert data, f'tmux control client exited: {self.client.poll()}'

    def close(self):
        if self.client:
            self.client.terminate()
            try:
                self.client.communicate(timeout=5)
            except subprocess.TimeoutExpired:
                self.client.kill()
                self.client.communicate(timeout=5)
        if self.selector:
            self.selector.close()
        self.call('kill-server')


def main():
    p = argparse.ArgumentParser()
    p.add_argument('--fipsd', required=True)
    p.add_argument('--fipstop', required=True)
    p.add_argument('--rust', required=True)
    p.add_argument('--out', type=Path, required=True)
    p.add_argument('--fixtures', type=Path)
    a = p.parse_args()
    a.out.mkdir(parents=True, exist_ok=True)
    repo = Path(__file__).resolve().parents[2]
    report = []
    with tempfile.TemporaryDirectory(prefix='.fp-', dir=repo) as directory:
        root = Path(directory)
        node = start_fipsd('live', a.fipsd, root, dict(os.environ, FIPSD_WORKERS='2'), reserve_port(socket.AF_INET), '1' * 64)
        fixture_server = None
        try:
            node.daemon.await_log('fipsd running', time.monotonic() + 30)
            control = node.socket
            if a.fixtures:
                replies = {path.stem: json.loads(path.read_text()).get('data') for path in a.fixtures.glob('show_*.json')}
                fixture_server = Fake(root / 'fixture.sock', replies)
                control = fixture_server.path
            for w, h in ((80, 24), (120, 40)):
                screens = Screens(root, w, h, (a.fipstop, a.rust), control)
                try:
                    for tab, marker in zip(TABS, MARKERS):
                        pair = []
                        full = []
                        for target, name in (('ft:c', 'c'), ('ft:rust', 'rust')):
                            frame = screens.await_screen(target, marker)
                            (a.out / f'{name}-{tab.lower()}-{w}x{h}.txt').write_text(frame)
                            pair.append(frame.splitlines()[3:-1])
                            full.append(frame.splitlines())
                        difference = sum(x != y for l, r in zip(*pair) for x, y in zip(l, r))
                        (a.out / f'diff-{tab.lower()}-{w}x{h}.txt').write_text(''.join(difflib.unified_diff(
                            [s+'\n' for s in pair[1]], [s+'\n' for s in pair[0]], fromfile='rust', tofile='c')))
                        report.append({'pane': tab, 'size': f'{w}x{h}', 'body_cells_different': difference,
                                       'footer_cells_different': sum(x != y for x, y in zip(full[0][-1], full[1][-1]))})
                        print(f'{w}x{h} {tab}: {difference} body cells differ', flush=True)
                        if tab != TABS[-1]:
                            screens.call('send-keys', '-t', 'ft:c', 'Tab')
                            screens.call('send-keys', '-t', 'ft:rust', 'Tab')
                finally:
                    screens.close()
        finally:
            if fixture_server:
                fixture_server.close()
            rc = node.daemon.stop(20)
            assert rc == 0 and not node.daemon.sanitizer_report(), node.daemon.text()[-4000:]
    (a.out / 'summary.json').write_text(json.dumps(report, indent=2) + '\n')
    print('comparison complete; private tmux server and live fipsd stopped')


if __name__ == '__main__':
    main()
