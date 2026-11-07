#!/usr/bin/env python3
"""Source contracts + refusal-unit tests; --compose invokes the real Compose parser."""
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
DEPLOY = ROOT / 'signet/deploy'
spec = importlib.util.spec_from_file_location('signet_ops', DEPLOY / 'ops.py')
ops = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ops)
try:
    ops.require_jsonschema()
except RuntimeError as error:
    print(f'deployment_contract: missing capability: {error}', file=sys.stderr)
    sys.exit(1)
COMPOSE = '--compose' in sys.argv
if COMPOSE:
    sys.argv.remove('--compose')


class DeploymentContracts(unittest.TestCase):
    def test_ops_help_without_jsonschema(self):
        result = subprocess.run([sys.executable, '-S', str(DEPLOY / 'ops.py'), '--help'],
                                text=True, capture_output=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('usage:', result.stdout)

    def test_deployment_capability_failure_without_jsonschema(self):
        result = subprocess.run([sys.executable, '-S', str(Path(__file__).resolve())],
                                text=True, capture_output=True)
        self.assertEqual(result.returncode, 1)
        self.assertEqual(result.stderr, 'deployment_contract: missing capability: '
                         'missing python3 module jsonschema; '
                         'install python3-jsonschema or pip install jsonschema\n')

    def test_missing_jsonschema_refused(self):
        with patch.dict(sys.modules, {'jsonschema': None}):
            with self.assertRaisesRegex(RuntimeError, '^missing python3 module jsonschema; '
                                        'install python3-jsonschema or pip install jsonschema$'):
                ops.bundle()

    def test_jsonschema_dependency_error_not_masked(self):
        with patch('builtins.__import__', side_effect=ModuleNotFoundError(name='referencing')):
            with self.assertRaises(ModuleNotFoundError):
                ops.require_jsonschema()

    def test_docker_security_options(self):
        d = (ROOT / 'signet/Dockerfile').read_text()
        for required in ('SIGNET_PASSKEYS=OFF', '-DSIGNET_USE_SQLCIPHER=ON',
                         '-DSIGNET_ENABLE_TEST_HOOKS=OFF', 'AS test',
                         'build-meson-sqlite', 'build-meson-sqlcipher',
                         'ldd /usr/local/bin/signetd', 'ldd /usr/local/bin/signetctl',
                         'USER 10001:10001', '"/etc/signet/signet.conf"'):
            self.assertIn(required, d)
        self.assertNotIn('setcap', d)
        self.assertNotIn('ENV SIGNET_DB_KEY', d)

    def test_secret_environment_refused(self):
        with tempfile.TemporaryDirectory() as t:
            p = Path(t) / 'deploy.env'
            for value in ('SIGNET_DB_KEY=secret', 'COMPOSE_FILE=evil.yml', 'SIGNET_HEALTH_PORT=$(id)',
                          'SIGNET_HEALTH_PORT=8080\nSIGNET_HEALTH_PORT=8081'):
                p.write_text(value)
                with self.assertRaises(RuntimeError):
                    ops.env_file(p, {'SIGNET_HEALTH_PORT'})
            p.write_text('SIGNET_HEALTH_PORT=8080\n')
            self.assertEqual(ops.env_file(p, {'SIGNET_HEALTH_PORT'}), {'SIGNET_HEALTH_PORT': '8080'})

    def test_stop_quarantine_and_missing_receipt(self):
        with tempfile.TemporaryDirectory() as t:
            volume = Path(t)
            (volume / 'state').mkdir()
            stop = volume / 'last-stop.json'
            original = Path.read_text
            def read(path, *a, **kw):
                if str(path) == '/proc/sys/kernel/random/boot_id':
                    return 'boot-one'
                return original(path, *a, **kw)
            with patch.object(ops, 'VOLUME', volume), patch.object(ops, 'STOP', stop), patch.object(Path, 'read_text', read):
                ops.wait_rule()  # genuinely fresh store
                (volume / 'state/signet.db').touch()
                with self.assertRaises(RuntimeError):
                    ops.wait_rule()
                for elapsed, passes in [(150, False), (269, False), (271, True), (-10, False)]:
                    stop.write_text(json.dumps({'wall_time': time.time() - elapsed,
                                               'monotonic': time.monotonic() - elapsed, 'boot_id': 'boot-one'}))
                    if passes:
                        ops.wait_rule()
                    else:
                        with self.assertRaises(RuntimeError):
                            ops.wait_rule()

    def test_volume_absence_precedes_state_creation(self):
        with patch.object(ops, 'run', side_effect=subprocess.CalledProcessError(1, ['findmnt'])):
            with self.assertRaises(subprocess.CalledProcessError):
                ops.storage({})

    def test_wrong_volume_uuid_refused(self):
        output = subprocess.CompletedProcess([], 0, json.dumps({'filesystems': [
            {'target': '/srv/signet', 'source': '/dev/sdz', 'uuid': 'wrong', 'fstype': 'ext4'}]}))
        with patch.object(ops, 'run', return_value=output):
            with self.assertRaisesRegex(RuntimeError, 'UUID'):
                ops.storage({'volume': {'filesystem_uuid': 'expected'}})

    def test_secret_symlink_refused(self):
        with tempfile.TemporaryDirectory() as t:
            p = Path(t) / 'secret'
            p.write_text('fixture-not-a-key')
            p.chmod(0o400)
            link = Path(t) / 'link'
            link.symlink_to(p)
            with self.assertRaises(RuntimeError):
                ops.owned(link, os.getuid(), 0o400)

    def test_uncertain_or_running_stop_refused(self):
        result = subprocess.CompletedProcess([], 0, 'container-id\n')
        with patch.object(ops, 'compose', return_value=result):
            for state in ({'Running': True, 'Restarting': False}, {'Running': False, 'Restarting': True}):
                inspected = subprocess.CompletedProcess([], 0, json.dumps([{'State': state}]))
                with patch.object(ops, 'run', return_value=inspected):
                    with self.assertRaises(RuntimeError):
                        ops.confirmed_stopped(Path('/unused'), {'profiles': []})
            with patch.object(ops, 'run', side_effect=subprocess.CalledProcessError(1, ['docker'])):
                with self.assertRaises(subprocess.CalledProcessError):
                    ops.confirmed_stopped(Path('/unused'), {'profiles': []})

    def test_upgrade_requires_candidate_preflight_even_offline(self):
        from types import SimpleNamespace
        current, candidate = Path('/current'), Path('/candidate')
        manifest = {'volume': {}, 'profiles': []}
        for offline in (False, True):
            with self.subTest(offline=offline):
                args = SimpleNamespace(action='upgrade', bundle=str(candidate), offline=offline)
                with patch.object(ops, 'preflight', side_effect=[
                        (current, manifest, {}), RuntimeError('local image digest mismatch')]) as preflight, \
                     patch.object(ops, 'bundle', return_value=(candidate, manifest, {})), \
                     patch.object(ops, 'compose') as compose, patch.object(ops, 'stop') as stop:
                    with self.assertRaisesRegex(RuntimeError, 'local image digest mismatch'):
                        ops.release(args)
                    preflight.assert_called_with(candidate, starting=False)
                    stop.assert_not_called()
                    if offline:
                        compose.assert_not_called()
                    else:
                        compose.assert_called_once_with(candidate, manifest, 'pull', 'signetd')

    def test_schema_changed_rollback_refused_before_stop(self):
        from types import SimpleNamespace
        previous = Path('/previous')
        manifest = {'volume': {}, 'upgrade': {'previous': str(previous),
                    'before': {'schema_version': 1}, 'after': {'schema_version': 2}}}
        args = SimpleNamespace(action='rollback-image', bundle=str(previous), confirm=None)
        with patch.object(ops, 'preflight', return_value=(Path('/current'), manifest, {})), \
             patch.object(ops, 'bundle', return_value=(previous, {'volume': {}}, {})), \
             patch.object(ops, 'inspect_store', return_value={'schema_version': 2}), \
             patch.object(ops, 'stop') as stop:
            with self.assertRaisesRegex(RuntimeError, 'schema changed'):
                ops.release(args)
            stop.assert_not_called()

    def test_proxy_contract(self):
        c = (DEPLOY / 'Caddyfile').read_text()
        for required in ('admin off', 'auto_https disable_redirects',
                         'path /bootstrap /challenge /auth', 'header_up X-Forwarded-Proto https',
                         'max_size 8KB', 'disable_http_challenge'):
            self.assertIn(required, c)
        self.assertNotIn('/health', c)

    def test_compose_profiles(self):
        with tempfile.TemporaryDirectory() as t:
            env = Path(t) / 'release.env'
            env.write_text('SIGNET_IMAGE=ghcr.io/test/signet@sha256:' + '1'*64 + '\n'
                           'CADDY_IMAGE=caddy@sha256:' + '2'*64 + '\n'
                           'SIGNET_HEALTH_PORT=18080\nSIGNET_ENROLLMENT_HOST=signet.example.org\nSIGNET_UHID_GID=123\n')
            for profiles in ([], ['passkeys'], ['bootstrap'], ['passkeys', 'ctap'], ['passkeys', 'ctap', 'bootstrap']):
                command = ['docker', 'compose', '--env-file', str(env), '-f', str(ROOT / 'signet/docker-compose.yml')]
                for profile in profiles:
                    command += ['-f', str(DEPLOY / ('compose.' + profile + '.yml'))]
                command += ['--profile', '*', 'config', '--format', 'json']
                config = json.loads(subprocess.check_output(command, text=True,
                                  env={k: v for k, v in os.environ.items() if not k.startswith(('SIGNET_', 'COMPOSE_', 'CADDY_'))}))
                daemon = config['services']['signetd']
                self.assertEqual(daemon['network_mode'], 'host')
                self.assertEqual(daemon['restart'], 'no')
                self.assertEqual(daemon['cap_drop'], ['ALL'])
                self.assertTrue(daemon['read_only'])
                self.assertFalse(set(daemon) & {'build', 'ports', 'cap_add', 'privileged'})
                self.assertNotIn('SIGNET_PROVISIONER_PUBKEYS', daemon['environment'])
                # Compose's JSON serializer omits zero-valued soft/hard fields.
                self.assertEqual(daemon['ulimits']['core'].get('hard', 0), 0)
                self.assertEqual(daemon['ulimits']['core'].get('soft', 0), 0)
                for mount in daemon['volumes']:
                    self.assertFalse(mount['bind']['create_host_path'])
                conf = next(v for v in daemon['volumes'] if v['target'] == '/etc/signet')
                self.assertFalse(conf.get('read_only', False))
                service = config['services']['signet-ops']
                self.assertEqual(service['network_mode'], 'none')
                self.assertFalse(any('bunker' in v['source'] for v in service['volumes']))
                self.assertTrue(service['healthcheck']['disable'])


if not COMPOSE:
    delattr(DeploymentContracts, 'test_compose_profiles')

if __name__ == '__main__':
    unittest.main()
