# SPDX-License-Identifier: MIT
"""Exercise stock signetd without test hooks or a running relay."""
import os
from pathlib import Path
import select
import socket
import subprocess
import sys
import tempfile
import time
import unittest
from urllib.parse import quote

DAEMON, CLIENT = map(os.path.abspath, sys.argv[1:])


def startup_log(proc):
    deadline = time.monotonic() + 15
    output = bytearray()
    while b'entering event-driven main loop' not in output:
        if not select.select([proc.stdout], [], [], max(0, deadline - time.monotonic()))[0]:
            raise AssertionError(f'signetd readiness deadline: {output.decode(errors="replace")}')
        chunk = os.read(proc.stdout.fileno(), 4096)
        if not chunk:
            raise AssertionError(f'signetd exited: {output.decode(errors="replace")}')
        output.extend(chunk)
    return output.decode(errors='replace')


class DaemonTcp(unittest.TestCase):
    def check_profile(self, host, settings='', host_override=None, opt_in=False):
        with tempfile.TemporaryDirectory(prefix='signet-dbus-tcp-') as tmp:
            root = Path(tmp)
            env = {k: v for k, v in os.environ.items() if not k.startswith(('SIGNET_', 'G_DEBUG'))}
            for name, value, mode in (
                    ('bunker', '01' * 32, 0o600), ('master', '0123456789abcdef' * 4, 0o400)):
                path = root / name
                path.write_text(value + '\n')
                path.chmod(mode)
            env.update(SIGNET_BUNKER_NSEC_FILE=str(root / 'bunker'),
                       SIGNET_DB_KEY_FILE=str(root / 'master'), SIGNET_REQUIRE_ENCRYPTED_DB='false')
            if host_override is not None:
                env['SIGNET_DBUS_TCP_HOST'] = host_override
            db = root / 'db'
            subprocess.run([CLIENT, 'seed', str(db)], env=env, check=True, timeout=12)
            with socket.socket(socket.AF_INET6 if ':' in host else socket.AF_INET) as s:
                s.bind((host, 0))
                port = s.getsockname()[1]
            config = root / 'signet.conf'
            config.write_text(f'[server]\nhealth_port=0\n[store]\ndb_path={db}\n'
                              '[nostr]\nrelays=wss://127.0.0.1:1\n[audit]\nstdout=false\n'
                              f'[dbus]\ntcp_enabled=true\ntcp_port={port}\n{settings}')
            address = f'tcp:host={quote(host, safe="")},port={port}'
            other = '127.0.0.1' if ':' in host else '::1'
            with subprocess.Popen([DAEMON, '-c', str(config)], env=env,
                                  stdout=subprocess.PIPE, stderr=subprocess.STDOUT) as proc:
                try:
                    log = startup_log(proc)
                    with self.subTest(check='wire credentials and signing'):
                        subprocess.run([CLIENT, 'probe', address, 'opt-in' if opt_in else 'default'],
                                       check=True, timeout=15)
                    with self.subTest(check='listener and warning'):
                        self.assertRegex(log, rf'D-Bus TCP transport started on tcp:host=[^,]+,port={port}\b')
                        if host == '127.0.0.1':
                            self.assertIn(f'tcp:host=127.0.0.1,port={port}', log)
                        with self.assertRaises(ConnectionRefusedError):
                            socket.create_connection((other, port), timeout=2).close()
                        self.assertEqual('may send credentials in plaintext' in log, opt_in, log)
                finally:
                    proc.terminate()
                    try:
                        proc.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        proc.kill()
                        proc.wait()

    def test_default(self):
        self.check_profile('127.0.0.1')

    def test_configured_host_and_opt_in(self):
        self.check_profile('::1', 'tcp_host=::1\ntcp_allow_credentials=true\n', opt_in=True)

    def test_environment_overrides_host(self):
        self.check_profile('127.0.0.1', 'tcp_host=::1\ntcp_allow_credentials=false\n',
                           host_override='127.0.0.1')


if __name__ == '__main__':
    unittest.main(argv=[sys.argv[0]], verbosity=2)
