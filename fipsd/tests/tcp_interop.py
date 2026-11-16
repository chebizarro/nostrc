#!/usr/bin/env python3
"""Optional real C/Rust TCP link smoke; the full interop gate is f66sr.1.

No build or source-checkout mutation here. Supply a binary built from the
libfips-core/UPSTREAM.md pin in a separate CARGO_TARGET_DIR.
"""
import argparse
import json
import os
from pathlib import Path
import selectors
import signal
import socket
import subprocess
import time


def await_log(proc, needle):
    selector = selectors.DefaultSelector()
    selector.register(proc.stdout, selectors.EVENT_READ)
    deadline = time.monotonic() + 25
    output = bytearray()
    try:
        while needle.encode() not in output.lower():
            remaining = deadline - time.monotonic()
            assert remaining > 0, f'timed out waiting for {needle}: {output.decode(errors="replace")}'
            assert selector.select(remaining), f'no log readiness: {output.decode(errors="replace")}'
            chunk = os.read(proc.stdout.fileno(), 65536)
            assert chunk, f'daemon exited: {output.decode(errors="replace")}'
            output.extend(chunk)
        print(output.decode(errors='replace'), end='')
    finally:
        selector.close()


def query(path, command):
    with socket.socket(socket.AF_UNIX) as conn:
        conn.settimeout(10)
        conn.connect(str(path))
        conn.sendall(json.dumps({'command': command}).encode() + b'\n')
        response = json.loads(conn.makefile('rb').readline())
    assert response['status'] == 'ok', response
    return response['data']


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--fipsd', required=True)
    ap.add_argument('--rust', default=os.environ.get('FIPSD_TCP_INTEROP_RUST_BINARY', ''))
    args = ap.parse_args()
    args.rust = args.rust or os.environ.get('FIPSD_TCP_INTEROP_RUST_BINARY', '')
    if not args.rust or not Path(args.rust).is_file():
        print('SKIP: set FIPSD_TCP_INTEROP_RUST_BINARY to a pinned Rust fips daemon')
        return 77
    root = Path(os.environ['FIPSD_TEST_TMPDIR'])
    rust_socket, c_socket = root / 'r.sock', root / 'c.sock'
    # Reserve an address only to select a free port; Rust's ordinary exclusive
    # listener bind is authoritative if another process races the reservation.
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as reservation:
        reservation.bind(('127.0.0.1', 0))
        rust_port = reservation.getsockname()[1]
    rust_config = root / 'rust.yaml'
    rust_config.write_text(f'''
node:
  identity: {{nsec: '{'2' * 64}', persistent: false}}
  control: {{enabled: true, socket_path: '{rust_socket}'}}
  native_api: {{enabled: false}}
tun: {{enabled: false}}
dns: {{enabled: false}}
transports:
  tcp: {{bind_addr: '127.0.0.1:{rust_port}'}}
''')
    processes = []
    env = dict(os.environ, FIPSD_WORKERS='2', TOKIO_WORKER_THREADS='2', NO_COLOR='1')
    try:
        rust = subprocess.Popen([args.rust, '-c', str(rust_config)], cwd=root, env=env,
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        processes.append(rust)
        await_log(rust, 'control socket listening')
        rust_npub = query(rust_socket, 'show_status')['npub']
        config = root / 'c.yaml'
        config.write_text(f'''
node:
  identity: {{nsec: '{'1' * 64}', persistent: false}}
  control: {{enabled: true, socket_path: '{c_socket}'}}
transports:
  tcp: {{bind_addr: '127.0.0.1:0'}}
peers:
  - npub: '{rust_npub}'
    addresses: [{{transport: tcp, addr: '127.0.0.1:{rust_port}'}}]
''')
        c = subprocess.Popen([args.fipsd, '-c', str(config)], cwd=root, env=env,
                             stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        processes.append(c)
        await_log(c, 'link established')
        for name, path in [('Rust', rust_socket), ('C', c_socket)]:
            # Rust's off-loop control view is tick-published, not a synchronous
            # reflection of the promotion log. Observe successive snapshots
            # until the link exists; socket replies provide backpressure. No
            # fixed warm-up sleep and no retry of a failed assertion.
            deadline = time.monotonic() + 10
            links = query(path, 'show_links')['links']
            while not links and time.monotonic() < deadline:
                links = query(path, 'show_links')['links']
            assert links, f'{name} control reports no link within snapshot deadline'
            print(f'{name} control show_links: {len(links)} link(s)')
        return 0
    finally:
        failures = []
        for proc in reversed(processes):
            if proc.poll() is None:
                proc.send_signal(signal.SIGTERM)
            try:
                code = proc.wait(timeout=15)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()
                failures.append('daemon did not join on SIGTERM')
                code = proc.returncode
            if code != 0:
                failures.append(f'daemon exited {code}')
            proc.stdout.close()
        assert not failures, '; '.join(failures)


if __name__ == '__main__':
    raise SystemExit(main())
