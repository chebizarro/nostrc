#!/usr/bin/env python3
"""Real fipsd entrypoint against the read-only pinned Rust fipsctl oracle."""
import argparse
import json
import os
from pathlib import Path
import shutil
import signal
import socket
import subprocess
import sys
import tempfile


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--fipsd', required=True)
    ap.add_argument('--upstream', default='')
    ap.add_argument('--cargo-target', required=True)
    ap.add_argument('--temp-root', required=True)
    ap.add_argument('--fipsctl', default='')
    args = ap.parse_args()
    client = args.fipsctl
    env = dict(os.environ, FIPSD_WORKERS='2')
    if not client:
        cargo = shutil.which('cargo')
        if not cargo:
            print('SKIP: cargo unavailable')
            return 77
        manifest = Path(args.upstream) / 'Cargo.toml'
        if not args.upstream or not manifest.is_file():
            print('SKIP: set FIPSD_UPSTREAM_CONTROL_DIR to the read-only upstream checkout')
            return 77
        env['CARGO_TARGET_DIR'] = args.cargo_target
        subprocess.run([cargo, 'build', '--locked', '--release', '--bin', 'fipsctl',
                        '--manifest-path', str(manifest)], env=env, cwd=args.temp_root, check=True)
        client = str(Path(args.cargo_target) / 'release' / 'fipsctl')
    with tempfile.TemporaryDirectory(prefix='.ctl-', dir=args.temp_root) as temp:
        root = Path(temp)
        path = root / 'control.sock'
        config = root / 'config.yaml'
        # Persistent is parsed, but no node or identity is constructed by the
        # control subsystem. It makes this an intentional non-empty config.
        config.write_text(f'node:\n  identity:\n    persistent: true\n  control:\n    socket_path: {path}\n')
        proc = subprocess.Popen([args.fipsd, '-c', str(config)], env=env,
                                stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True)
        try:
            for line in proc.stderr:
                if 'fipsd running' in line:
                    break
            else:
                raise AssertionError('daemon never reached startup readiness')
            cases = [(['show', 'status'], 1, 'node not yet initialized'),
                     (['show', 'peers'], 0, 'peers'),
                     (['show', 'transports'], 0, 'transports'),
                     (['stats', 'list'], 0, 'mesh_size')]
            for command, expected_rc, expected in cases:
                result = subprocess.run([client, '-s', str(path), *command], env=env,
                                        text=True, capture_output=True, timeout=15)
                assert result.returncode == expected_rc, (command, result)
                assert expected in result.stdout + result.stderr, (command, result)
                print(f'fipsctl {" ".join(command)}: exit {result.returncode}; {expected}')
            # Verify the direct consumer error envelope and unknown aliases.
            for command, message in [('show_status', 'node not yet initialized'),
                                     ('reload', 'unknown command: reload')]:
                with socket.socket(socket.AF_UNIX) as conn:
                    conn.settimeout(10)
                    conn.connect(str(path))
                    conn.sendall(json.dumps({'command': command}).encode() + b'\n')
                    with conn.makefile('rb') as stream:
                        answer = json.loads(stream.readline())
                        assert stream.read() == b''
                assert answer == {'status': 'error', 'message': message}, answer
        finally:
            if proc.poll() is None:
                proc.send_signal(signal.SIGTERM)
            _, stderr = proc.communicate(timeout=15)
            assert proc.returncode == 0, stderr
        assert not path.exists(), 'control socket was not removed at shutdown'
        # Real startup gating: disabled control must neither bind nor modify
        # an existing file at its configured socket path.
        path.write_text('do not touch')
        config.write_text(f'node:\n  identity:\n    persistent: true\n  control:\n    enabled: false\n    socket_path: {path}\n')
        proc = subprocess.Popen([args.fipsd, '-c', str(config)], env=env,
                                stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True)
        try:
            for line in proc.stderr:
                if 'fipsd running' in line:
                    break
            else:
                raise AssertionError('disabled-control daemon did not start')
        finally:
            if proc.poll() is None:
                proc.send_signal(signal.SIGTERM)
            _, stderr = proc.communicate(timeout=15)
            assert proc.returncode == 0, stderr
        assert path.read_text() == 'do not touch'
        print('real daemon: custom socket, disabled-control gating and shutdown cleanup passed')
    return 0


if __name__ == '__main__':
    sys.exit(main())
