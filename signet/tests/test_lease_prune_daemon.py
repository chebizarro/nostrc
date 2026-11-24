#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Verify the scheduled signetd tick against a disposable on-disk store."""
import os
import pathlib
import selectors
import socket
import subprocess
import sys
import tempfile
import time


def main():
    daemon, fixture = map(os.path.abspath, sys.argv[1:3])
    env = {k: v for k, v in os.environ.items() if not k.startswith('SIGNET_')}
    with tempfile.TemporaryDirectory(prefix='signet-lease-prune-') as tmp:
        root = pathlib.Path(tmp)
        db = root / 'signet.db'
        key = '0123456789abcdef' * 4  # Disposable fixture key.
        master = root / 'master'
        master.write_text(key + '\n')
        master.chmod(0o400)
        bunker = root / 'bunker'
        bunker.write_text('01' * 32 + '\n')
        bunker.chmod(0o600)
        env.update(SIGNET_DB_KEY_FILE=str(master), SIGNET_BUNKER_NSEC_FILE=str(bunker))
        subprocess.run([fixture, 'seed', db, key], env=env, check=True, timeout=10)

        cfg = root / 'signet.conf'
        with socket.socket() as probe:
            probe.bind(('127.0.0.1', 0))
            port = probe.getsockname()[1]
        cfg.write_text(f'[server]\nhealth_port={port}\n[store]\ndb_path={db}\n'
                       '[nostr]\nrelays=wss://127.0.0.1:1\n[audit]\nstdout=false\n')
        with subprocess.Popen([daemon, '-c', cfg], env=env, stdout=subprocess.PIPE,
                              stderr=subprocess.STDOUT) as child, selectors.DefaultSelector() as sel:
            sel.register(child.stdout, selectors.EVENT_READ)
            output = bytearray()
            # Only the periodic tick emits this message; startup alone is not evidence.
            tick = b'relay disconnected'
            deadline = time.monotonic() + 10
            try:
                while tick not in output:
                    assert sel.select(max(0, deadline - time.monotonic())), output.decode(errors='replace')
                    chunk = os.read(child.stdout.fileno(), 65536)
                    assert chunk, output.decode(errors='replace')
                    output.extend(chunk)
            finally:
                child.terminate()
                try:
                    tail, _ = child.communicate(timeout=5)
                except subprocess.TimeoutExpired:
                    child.kill()
                    child.communicate()
                    raise
                output.extend(tail)
            assert child.returncode == 0, output.decode(errors='replace')

        subprocess.run([fixture, 'verify', db, key], env=env, check=True, timeout=10)
        assert b'pruned 1 expired leases' in output, output.decode(errors='replace')


if __name__ == '__main__':
    main()
