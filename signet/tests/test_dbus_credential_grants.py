# SPDX-License-Identifier: MIT
"""Exercise the stock daemon on a private Unix D-Bus (no test hooks)."""
import ast
import base64
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
PASSKEY_GRANTS = {
    'GetInfo': 'passkey.get_info',
    'MakeCredential': 'passkey.make_credential',
    'GetAssertion': 'passkey.get_assertion',
    'ExportCredential': 'passkey.export',
    'ImportCredential': 'passkey.import',
}


def exercise_passkeys(agent, call, set_policy, reload_policy, wait_for_audit, transfer):
    def request(method, payload=None, error=None):
        args = () if method == 'GetInfo' else (json.dumps(payload or {}),)
        output = call('Passkeys', method, *args, expected_error=error)
        if not error:
            return json.loads(ast.literal_eval(output)[0])

    def grant_only(method):
        set_policy([PASSKEY_GRANTS[method]])
        reload_policy()
        for other in PASSKEY_GRANTS:
            if other != method:
                request(other, error=DENIED)

    def revoke(method):
        set_policy([])
        reload_policy()
        request(method, error=DENIED)

    if agent not in ('granted', 'importer'):
        for method in PASSKEY_GRANTS:
            request(method, error=DENIED)
        if agent == 'deny-listed':
            wait_for_audit(method='GetInfo', decision='deny', reason='deny_listed')
        return

    rp = 'passkey-grants.example'
    assertion = {'rpId': rp, 'clientDataHash': base64.b64encode(b'a' * 32).decode(),
                 'userVerification': 'discouraged'}
    if agent == 'granted':
        grant_only('GetInfo')
        assert request('GetInfo')['versions']
        revoke('GetInfo')
        grant_only('MakeCredential')
        created = request('MakeCredential', {
            **assertion, 'clientDataHash': base64.b64encode(b'r' * 32).decode(),
            'userHandle': base64.b64encode(b'grants-test').decode(),
            'discoverable': True, 'pubKeyCredParams': [-7],
        })
        credential_id = created['credentialId']
        assert credential_id and created['attestationObject']
        revoke('MakeCredential')
        grant_only('GetAssertion')
        assertion['allowCredentials'] = [credential_id]
        signed = request('GetAssertion', assertion)
        assert signed['credentialId'] == credential_id and signed['signature']
        revoke('GetAssertion')
        grant_only('ExportCredential')
        exported = request('ExportCredential', {'credentialId': credential_id})
        assert exported['format'] == 'signet-passkey-export' and exported['formatVersion'] == 1
        assert exported['container']
        revoke('ExportCredential')
        transfer.update(credential_id=credential_id, container=exported['container'])
    else:
        grant_only('ImportCredential')
        imported = request('ImportCredential', {'container': transfer['container']})
        assert imported['credentialId'] == transfer['credential_id'] and imported['rpId'] == rp
        revoke('ImportCredential')
        grant_only('GetAssertion')
        assertion['allowCredentials'] = [transfer['credential_id']]
        signed = request('GetAssertion', assertion)
        assert signed['credentialId'] == transfer['credential_id'] and signed['signature']


def exercise(daemon, fixture, root, address, agent, methods, deny=False, *, transfer=None):
    passkeys = transfer is not None
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
        wildcard_grants = list(PASSKEY_GRANTS.values()) if passkeys else [TOKEN, SESSION]
        set_policy(wildcard_grants, identity='*')  # a wildcard identity is not a grant

    cfg = root / 'signet.conf'
    with socket.socket() as probe:
        probe.bind(('127.0.0.1', 0))
        port = probe.getsockname()[1]
    passkey_config = ('[passkeys]\nenabled=true\nbackend=software-openssl\n'
                      f'sync_key={"ab" * 32}\n') if passkeys else ''
    cfg.write_text(f'[server]\nhealth_port={port}\n[store]\ndb_path={db}\n'
                   '[nostr]\nrelays=wss://127.0.0.1:1\n[audit]\nstdout=true\n'
                   f'[policy_defaults]\npolicy_file={policy}\n[dbus]\nunix_enabled=true\n'
                   f'[uid_map]\n{os.getuid()}={agent}\n' + passkey_config)
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

    def invoke(interface, method, *args, expected_error=None):
        result = subprocess.run(BASE + ['--method', f'net.signet.{interface}.{method}', *args],
                                env=env, text=True, capture_output=True, timeout=10)
        if expected_error:
            assert result.returncode and f'net.signet.Error.{expected_error}:' in result.stderr, (
                agent, method, result.stderr, result.returncode)
        else:
            assert result.returncode == 0, (agent, method, result.stderr)
            return result.stdout.strip()

    def call(method, arg, expected_error=None):
        output = invoke('Credentials', method, arg, expected_error=expected_error)
        if not expected_error:
            token = '746f6b656e' if method == 'GetToken' else '[0-9a-f]{64}'
            value = re.fullmatch(rf"\('{token}', (?:int64 )?(\d+)\)", output)
            assert value and int(value[1]) > time.time(), (agent, method, 'invalid token or expiry')

    try:
        subprocess.run(['gdbus', 'wait', '--system', '--timeout', '10', BUS],
                       check=True, env=env, timeout=12)
        if passkeys:
            exercise_passkeys(agent, invoke, set_policy, reload_policy, wait_for_audit, transfer)
        elif agent == 'granted':
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
    passkeys = sys.argv[3:] == ['--passkeys']
    grants = list(PASSKEY_GRANTS.values()) if passkeys else [TOKEN, SESSION]
    transfer = {} if passkeys else None
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
                cases = [('granted', None if passkeys else grants, False),
                         ('ungranted', None, False), ('wildcard', ['*'], False),
                         ('deny-listed', grants, True)]
                if passkeys:
                    cases.append(('importer', None, False))
                for agent, methods, deny in cases:
                    exercise(daemon, fixture, root / agent, address, agent, methods, deny,
                             transfer=transfer)
                    print(f'{agent}: PASS')
            finally:
                bus.terminate()


if __name__ == '__main__':
    main()
