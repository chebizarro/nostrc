#!/usr/bin/env python3
"""The shipped frame renderer, from control_golden replies, without a terminal."""
import argparse
import difflib
import os
from pathlib import Path
import subprocess

TABS = ('Node', 'Peers', 'Transports', 'Sessions', 'Tree', 'Filters',
        'Performance', 'Routing', 'Graphs', 'Gateway')


def main():
    p = argparse.ArgumentParser()
    p.add_argument('--fipstop', required=True)
    p.add_argument('--fixtures', type=Path, required=True)
    p.add_argument('--goldens', type=Path, required=True)
    p.add_argument('--update', action='store_true')
    a = p.parse_args()
    failures = []
    for size in ('80x24', '120x40'):
        for tab in TABS:
            result = subprocess.run([a.fipstop, '--fixture-dir', str(a.fixtures),
                '--render-once', size, '--tab', tab], capture_output=True, encoding='utf-8', timeout=10, env=dict(os.environ, LC_ALL="C"))
            if result.returncode or result.stderr:
                raise AssertionError(f'{tab}: {result.returncode}: {result.stderr}')
            w, h = map(int, size.split('x'))
            lines = result.stdout.splitlines()
            assert len(lines) == h and all(len(line) == w for line in lines), (tab, size)
            canonical = '\n'.join(s.rstrip() for s in lines) + '\n'
            path = a.goldens / f'{tab.lower()}-{size}.txt'
            if a.update:
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(canonical, encoding='utf-8')
            else:
                expected = path.read_text(encoding='utf-8')
                if canonical != expected:
                    failures.append(''.join(difflib.unified_diff(expected.splitlines(True),
                        canonical.splitlines(True), fromfile=str(path), tofile='rendered')))
    assert not failures, '\n'.join(failures)
    print('fipstop golden frames: 10 panes x 2 sizes passed' if not a.update else 'updated 20 fipstop golden frames')


if __name__ == '__main__':
    main()
