# SPDX-License-Identifier: MIT
"""Exercise the stock daemon on a private Unix D-Bus (no test hooks)."""
import hashlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
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
UNBOUND = 'SessionEndpointNotBound'
EXCHANGE_FAILED = 'SessionExchangeFailed'
SERVICE_TOKEN = 'fixture-service-token'
EXPIRY = 2100000000
RESPONSES = {
    '/ttl': {'token': SERVICE_TOKEN, 'expires_in': 600},
    '/default-expiry': {'session_token': SERVICE_TOKEN},
    '/malformed': {'session_token': 42},
    '/nul': {'session_token': SERVICE_TOKEN + '\0suffix'},
    '/expired': {'session_token': SERVICE_TOKEN, 'expires_at': 1},
    '/bad-expiry': {'session_token': SERVICE_TOKEN, 'expires_at': '2100000000'},
    '/overflow': {'session_token': SERVICE_TOKEN, 'expires_in': 2**63 - 1},
}
SLOW_PATHS = ('/slow-first', '/slow-stop')
FAILURE_PATHS = ('/failed', '/redirect', '/oversize', '/malformed', '/nul',
                 '/expired', '/bad-expiry', '/overflow')


class ExchangeHandler(BaseHTTPRequestHandler):
    def log_message(self, *_):
        pass

    def do_POST(self):
        body = self.rfile.read(int(self.headers['Content-Length']))
        self.server.requests.append((self.path, self.headers['Content-Type'],
                                     self.headers['Accept'], json.loads(body)))
        status, headers = 200, {}
        payload = json.dumps({'session_token': SERVICE_TOKEN, 'expires_at': EXPIRY}).encode()
        if self.path == '/failed':
            status = 503
        elif self.path == '/redirect':
            status, headers = 307, {'Location': '/redirect-target'}
        elif self.path == '/oversize':
            payload += b' ' * (64 * 1024)  # valid JSON if read without the limit
        elif self.path in RESPONSES:
            payload = json.dumps(RESPONSES[self.path]).encode()
        if self.path in SLOW_PATHS:
            self.server.entered.put(self.path)
            if not self.server.release[self.path].wait(timeout=25):
                return
        self.send_response(status)
        self.send_header('Content-Length', str(len(payload)))
        for name, value in headers.items():
            self.send_header(name, value)
        try:
            self.end_headers()
            self.wfile.write(payload)
        except (BrokenPipeError, ConnectionResetError):
            if self.path != '/slow-stop':
                raise


def exercise(daemon, fixture, root, address, agent, methods, service, curl, deny=False):
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
    endpoint = f'http://127.0.0.1:{service.server_port}'
    insecure = f'http://localhost:{service.server_port}/insecure'
    matched = endpoint + '/session'
    extra = ['', insecure, endpoint + '/ambiguous', endpoint + '/ambiguous']
    extra += [endpoint + path for path in (*FAILURE_PATHS, *SLOW_PATHS, '/ttl', '/default-expiry')]
    cred_id = subprocess.check_output(
        [fixture, db, agent, 'deny' if deny else 'allow', matched, *extra],
        env=env, text=True, timeout=10).strip()

    def leases():
        return subprocess.check_output([fixture, 'leases', db], env=env, text=True,
                                       timeout=10).splitlines()

    def session(url, expected_error=None, *, hit=None, ttl=None):
        previous_requests, previous_leases = len(service.requests), leases()
        before = int(time.time())
        expiry = call('GetSession', url, expected_error)
        after = int(time.time())
        expected_requests = [] if hit is None else [
            (hit, 'application/json', 'application/json', {'credential': 'dG9rZW4='})]
        assert service.requests[previous_requests:] == expected_requests
        rows = leases()
        if expected_error:
            assert rows == previous_leases, (url, expected_error, rows)
        else:
            assert rows[:-1] == previous_leases and len(rows) == len(previous_leases) + 1
            secret_id, owner, lease_expiry, metadata = rows[-1].split('\t', 3)
            assert secret_id.startswith('cred_') and owner == agent and int(lease_expiry) == expiry
            if ttl is None:
                assert expiry == EXPIRY and secret_id == cred_id
            else:
                assert before + ttl <= expiry <= after + ttl
            assert json.loads(metadata) == {
                'session_token_hash': hashlib.sha256(SERVICE_TOKEN.encode()).hexdigest()}

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

    def command(method, arg):
        return BASE + ['--method', f'net.signet.Credentials.{method}', arg]

    def call(method, arg, expected_error=None, *, timeout=10):
        result = subprocess.run(command(method, arg), env=env, text=True,
                                capture_output=True, timeout=timeout)
        if expected_error:
            assert result.returncode and f'net.signet.Error.{expected_error}:' in result.stderr, (
                agent, method, result.stderr, result.returncode)
        else:
            assert result.returncode == 0, (agent, method, result.stderr)
            token = '746f6b656e' if method == 'GetToken' else SERVICE_TOKEN
            value = re.fullmatch(rf"\('{token}', (?:int64 )?(\d+)\)", result.stdout.strip())
            assert value and int(value[1]) > time.time(), (agent, method, 'invalid token or expiry')
            return int(value[1])

    def exercise_slow_sessions():
        clients = []
        try:
            for path in SLOW_PATHS:
                client = subprocess.Popen(command('GetSession', endpoint + path), env=env,
                                          text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
                clients.append(client)
                assert service.entered.get(timeout=6) == path
            assert all(client.poll() is None for client in clients)
            # Neither held request may serialize the other or block dispatch.
            call('GetToken', cred_id, timeout=4)
            call('GetSession', matched, 'SessionExchangeBusy', timeout=4)
            service.release[SLOW_PATHS[0]].set()
            stdout, stderr = clients[0].communicate(timeout=6)
            assert clients[0].returncode == 0 and SERVICE_TOKEN in stdout, (stdout, stderr)

            before_stop = leases()
            p.terminate()
            assert p.wait(timeout=8) == 0, 'daemon did not drain a held exchange cleanly'
            _, stderr = clients[1].communicate(timeout=6)
            assert clients[1].returncode != 0, stderr
            assert leases() == before_stop, 'cancelled exchange persisted a lease'
        finally:
            for path in SLOW_PATHS:
                service.release[path].set()
            for client in clients:
                if client.poll() is None:
                    client.kill()
                client.communicate(timeout=6)

    try:
        subprocess.run(['gdbus', 'wait', '--system', '--timeout', '10', BUS],
                       check=True, env=env, timeout=12)
        if agent == 'granted':
            call('GetToken', cred_id)
            session(matched, None if curl else EXCHANGE_FAILED, hit='/session' if curl else None)
            wait_for_audit(method='GetSession', decision='allow' if curl else 'error',
                           reason='ok' if curl else 'exchange_failed')
            for url in ('', endpoint + '/unprovisioned', matched + '/', endpoint + '/ambiguous'):
                session(url, UNBOUND)
            session(insecure, EXCHANGE_FAILED)
            wait_for_audit(method='GetSession', decision='error', reason='exchange_failed')
            for path in FAILURE_PATHS:
                session(endpoint + path, EXCHANGE_FAILED, hit=path if curl else None)
            for path, ttl in (('/ttl', 600), ('/default-expiry', 3600)):
                session(endpoint + path, None if curl else EXCHANGE_FAILED,
                        hit=path if curl else None, ttl=ttl)
            set_policy([SESSION])
            reload_policy()
            call('GetToken', cred_id, DENIED)
            session(matched, None if curl else EXCHANGE_FAILED, hit='/session' if curl else None)
            set_policy([])
            reload_policy()
            session(matched, DENIED)
            if curl:
                set_policy([TOKEN, SESSION])
                reload_policy()
                exercise_slow_sessions()
        else:
            call('GetToken', cred_id, 'NotFound' if deny else DENIED)
            session(matched, DENIED)
            if deny:
                wait_for_audit(method='GetSession', decision='deny', reason='deny_listed')
    except Exception:
        print(''.join(log), file=sys.stderr)
        raise
    finally:
        if p.poll() is None:
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
    assert sys.argv[3] in ('0', '1')
    curl = sys.argv[3] == '1'
    with ThreadingHTTPServer(('127.0.0.1', 0), ExchangeHandler) as service:
        service.requests = []
        service.entered = queue.Queue()
        service.release = {path: threading.Event() for path in SLOW_PATHS}
        thread = threading.Thread(target=service.serve_forever, daemon=True)
        thread.start()
        try:
            exercise_bus(daemon, fixture, service, curl)
        finally:
            service.shutdown()
            thread.join()


def exercise_bus(daemon, fixture, service, curl):
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
                    exercise(daemon, fixture, root / agent, address, agent, methods,
                             service, curl, deny)
                    print(f'{agent}: PASS')
            finally:
                bus.terminate()


if __name__ == '__main__':
    main()
