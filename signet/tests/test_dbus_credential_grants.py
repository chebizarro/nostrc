# SPDX-License-Identifier: MIT
"""Exercise the stock daemon on a private Unix D-Bus (no test hooks)."""
import json
import os
from pathlib import Path
import queue
import re
import select
import signal
import socket
import subprocess
import sys
import tempfile
import threading
import time


BUS = 'net.signet.Signer'
BASE = ['gdbus', 'call', '--system', '--dest', BUS, '--object-path', '/net/signet/Signer']
TOKEN = 'credential.get_token'
SESSION = 'credential.get_session'
DENIED = 'CapabilityDenied'


def exercise(daemon, fixture, root, address, agent, methods, deny=False):
    root.mkdir()
    env = {k: v for k, v in os.environ.items() if not k.startswith('SIGNET_')}
    env.update(DBUS_SYSTEM_BUS_ADDRESS=address,
               SIGNET_DB_KEY='0123456789abcdef' * 4,
               SIGNET_REQUIRE_ENCRYPTED_DB='false')
    bunker = root / 'bunker'
    bunker.write_text('01' * 32 + '\n')
    bunker.chmod(0o600)
    env['SIGNET_BUNKER_NSEC_FILE'] = str(bunker)
    db = root / 'signet.db'
    policy = root / 'policy.conf'
    policy.touch(mode=0o600)
    cred_id = subprocess.check_output([fixture, db, agent, 'deny' if deny else 'allow'],
                                      env=env, text=True, timeout=10).strip()

    def set_policy(allow, *, default='deny', identity=agent):
        subprocess.run([fixture, 'set-policy', policy, identity,
                        json.dumps({'default': default, 'allow_methods': allow})],
                       check=True, env=env, timeout=10)

    if methods is not None:
        set_policy(methods, default='allow' if methods == ['*'] else 'deny')
    if agent == 'ungranted':
        set_policy([TOKEN, SESSION], identity='*')  # a wildcard identity is not a grant

    cfg = root / 'signet.conf'
    with socket.socket() as probe:
        probe.bind(('127.0.0.1', 0))
        port = probe.getsockname()[1]
    cfg.write_text(f'[server]\nhealth_port={port}\n[store]\ndb_path={db}\n'
                   '[nostr]\nrelays=wss://127.0.0.1:1\n[audit]\nstdout=true\n'
                   f'[policy_defaults]\npolicy_file={policy}\n[dbus]\nunix_enabled=true\n'
                   f'[uid_map]\n{os.getuid()}={agent}\n')
    p = subprocess.Popen([daemon, '-c', cfg], env=env, text=True,
                         stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    log = []
    lines = queue.Queue()

    def read_log():
        for line in p.stdout:
            log.append(line)
            lines.put(line)
        lines.put(None)

    reader = threading.Thread(target=read_log, daemon=True)
    reader.start()

    def wait_for_audit(**fields):
        deadline = time.monotonic() + 10
        while True:
            line = lines.get(timeout=max(0, deadline - time.monotonic()))
            assert line is not None and 'config_reload_rejected' not in line, line
            if line.startswith('{'):
                data = json.loads(line)['data']
                if all(data.get(key) == value for key, value in fields.items()):
                    return

    def reload_policy():
        p.send_signal(signal.SIGHUP)
        wait_for_audit(action='config_reload')

    def call(method, arg, expected_error=None):
        result = subprocess.run(BASE + ['--method', f'net.signet.Credentials.{method}', arg],
                                env=env, text=True, capture_output=True, timeout=10)
        if expected_error:
            assert result.returncode and f'net.signet.Error.{expected_error}:' in result.stderr, (
                agent, method, result.stderr, result.returncode)
        else:
            assert result.returncode == 0, (agent, method, result.stderr)
            token = '746f6b656e' if method == 'GetToken' else '[0-9a-f]{64}'
            value = re.fullmatch(rf"\('{token}', (?:int64 )?(\d+)\)", result.stdout.strip())
            assert value and int(value[1]) > time.time(), (agent, method, 'invalid token or expiry')

    try:
        subprocess.run(['gdbus', 'wait', '--system', '--timeout', '10', BUS],
                       check=True, env=env, timeout=12)
        if agent == 'granted':
            call('GetToken', cred_id)
            call('GetSession', 'https://service.invalid')
            set_policy([SESSION])
            reload_policy()
            call('GetToken', cred_id, DENIED)
            call('GetSession', 'https://service.invalid')
            set_policy([])
            reload_policy()
            call('GetSession', 'https://service.invalid', DENIED)
        else:
            call('GetToken', cred_id, 'NotFound' if deny else DENIED)
            call('GetSession', 'https://service.invalid', DENIED)
            if deny:
                wait_for_audit(method='GetSession', decision='deny', reason='deny_listed')
    except Exception:
        print(''.join(log), file=sys.stderr)
        raise
    finally:
        p.terminate()
        try:
            p.wait(timeout=8)
        except subprocess.TimeoutExpired:
            p.kill()
            p.wait()
        reader.join(timeout=2)
        p.stdout.close()


def main():
    daemon, fixture = map(os.path.abspath, sys.argv[1:3])
    # Keep the socket path short enough for sockaddr_un, in a mode-0700 directory.
    with tempfile.TemporaryDirectory(prefix='signet-dbus-grants-', dir='/tmp') as tmp:
        root = Path(tmp)
        bus_cfg = root / 'bus.conf'
        # Do not use the platform's session config (it can request launchd
        # check-in on macOS). This listener belongs only to the test.
        bus_cfg.write_text(f'''<busconfig>
          <type>session</type><listen>unix:path={root / 'bus.sock'}</listen><auth>EXTERNAL</auth>
          <policy context="default"><allow send_destination="*"/>
            <allow receive_sender="*"/><allow own="*"/></policy>
        </busconfig>''')
        with subprocess.Popen(['dbus-daemon', '--nofork', '--print-address=1',
                               f'--config-file={bus_cfg}'], stdout=subprocess.PIPE,
                              text=True) as bus:
            try:
                assert select.select([bus.stdout], [], [], 10)[0], 'D-Bus startup timed out'
                address = bus.stdout.readline().strip()
                assert address.startswith('unix:'), address
                for agent, methods, deny in [('granted', [TOKEN, SESSION], False),
                                             ('ungranted', None, False),
                                             ('wildcard', ['*'], False),
                                             ('deny-listed', [TOKEN, SESSION], True)]:
                    exercise(daemon, fixture, root / agent, address, agent, methods, deny)
                    print(f'{agent}: PASS')
            finally:
                bus.terminate()


if __name__ == '__main__':
    main()
