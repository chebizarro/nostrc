#!/usr/bin/env python3
"""Exercise backup() through the real AWS CLI and a hermetic loopback S3 server.

Usage: python3 test_backup_upload.py /absolute/path/to/aws
No live credentials, containers or external endpoints are used.
"""
import contextlib
import http.server
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import threading
import unittest
from unittest.mock import patch

AWS = sys.argv.pop(1) if len(sys.argv) > 1 and not sys.argv[1].startswith('-') else 'aws'
if os.name != 'posix' or not shutil.which(AWS):
    print('SKIP backup_upload: requires a POSIX host and an executable AWS CLI', file=sys.stderr)
    sys.exit(77)
DEPLOY = Path(__file__).resolve().parents[1] / 'deploy'
spec = importlib.util.spec_from_file_location('signet_ops', DEPLOY / 'ops.py')
ops = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ops)


@contextlib.contextmanager
def fake_s3(fault, object_name):
    objects, requests = {}, []

    class Handler(http.server.BaseHTTPRequestHandler):
        protocol_version = 'HTTP/1.1'

        def log_message(self, *args):
            pass

        def do_PUT(self):
            objects[self.path] = self.rfile.read(int(self.headers['Content-Length']))
            requests.append(('PUT', self.path))
            self.send_response(200)
            self.send_header('Content-Length', '0')
            self.send_header('ETag', '"not-a-content-hash"')
            self.end_headers()

        def do_HEAD(self):
            self.read_object(head=True)

        def do_GET(self):
            self.read_object(head=False)

        def read_object(self, head):
            requests.append(('HEAD' if head else 'GET', self.path))
            value = objects.get(self.path)
            if value is None or (fault == 'missing' and self.path.endswith('/' + object_name)):
                self.send_response(404)
                self.send_header('Content-Length', '0')
                self.end_headers()
                return
            if self.path.endswith('/' + object_name):
                if fault == 'truncated':
                    value = value[:-1]
                elif fault == 'corrupt':
                    value = bytes([value[0] ^ 1]) + value[1:]
            self.send_response(200)
            self.send_header('Content-Length', str(len(value)))
            self.send_header('Last-Modified', 'Sun, 22 Nov 2026 00:00:00 GMT')
            self.send_header('ETag', '"not-a-content-hash"')
            self.end_headers()
            if not head:
                self.wfile.write(value)

    with http.server.ThreadingHTTPServer(('127.0.0.1', 0), Handler) as server:
        thread = threading.Thread(target=server.serve_forever)
        thread.start()
        try:
            yield f'http://127.0.0.1:{server.server_port}', requests
        finally:
            server.shutdown()
            thread.join()


class BackupUpload(unittest.TestCase):
    def exercise(self, fault=None, object_name='store.db', upload=True):
        with tempfile.TemporaryDirectory() as tmp, fake_s3(fault, object_name) as (endpoint, requests):
            root = Path(tmp)
            home, volume, bundle = root / 'home', root / 'volume', root / 'bundle'
            for directory in (home / 'config', volume / 'backups', volume / 'state', bundle):
                directory.mkdir(parents=True)
            for path in (home / 'config/signet.conf', home / 'config/deploy.env',
                         volume / 'state/policies.conf', volume / 'state/audit.jsonl'):
                path.write_text('fixture\n')
            previous = {'last_successful_backup': {'reference': 'previous-verified-backup'}}
            (bundle / 'manifest.json').write_text(json.dumps(previous))
            manifest = json.loads(json.dumps(previous))
            credentials = root / 'credentials'
            credentials.write_text('[default]\naws_access_key_id=fixture\naws_secret_access_key=fixture\n')

            def store(_p, _m, command, destination):
                self.assertEqual(command, 'backup-db')
                (volume / destination.lstrip('/')).write_bytes(b'database fixture')

            def run(command):
                # Only remap the fixed production credential path and validated
                # endpoint to this fixture. Execute the real CLI, not a mock cp.
                self.assertEqual(command[:4], ['env',
                    'AWS_SHARED_CREDENTIALS_FILE=/opt/signet/secrets/spaces.credentials',
                    'AWS_EC2_METADATA_DISABLED=true', 'aws'])
                args = [str(x) for x in command[4:]]
                self.assertEqual(args[:2], ['--endpoint-url', 'https://fixture.digitaloceanspaces.com'])
                args[1] = endpoint
                env = {'PATH': os.environ.get('PATH', os.defpath), 'HOME': str(root),
                       'AWS_ACCESS_KEY_ID': 'fixture', 'AWS_SECRET_ACCESS_KEY': 'fixture',
                       'AWS_SHARED_CREDENTIALS_FILE': str(credentials),
                       'AWS_CONFIG_FILE': str(root / 'no-config'), 'AWS_EC2_METADATA_DISABLED': 'true',
                       'AWS_DEFAULT_REGION': 'us-east-1', 'AWS_MAX_ATTEMPTS': '1',
                       'NO_PROXY': '127.0.0.1', 'AWS_REQUEST_CHECKSUM_CALCULATION': 'when_required'}
                return subprocess.run([AWS, *args], env=env, check=True, capture_output=True)

            with patch.object(ops, 'HOME', home), patch.object(ops, 'VOLUME', volume), \
                 patch.object(ops, 'store', store), patch.object(ops, 'inspect_store'), \
                 patch.object(ops.os, 'chown'), patch.object(ops, 'owned'), \
                 patch.object(ops, 'site', return_value={'SPACES_ENDPOINT': 'https://fixture.digitaloceanspaces.com',
                                                       'SPACES_BUCKET': 'fixture-bucket'}), \
                 patch.object(ops, 'run', run):
                if fault:
                    if fault == 'missing':
                        with self.assertRaises(subprocess.CalledProcessError) as error:
                            ops.backup(bundle, manifest, upload=upload)
                        self.assertIn(b'404', error.exception.stderr)
                    else:
                        message = 'size mismatch' if fault == 'truncated' else 'checksum mismatch'
                        with self.assertRaisesRegex(RuntimeError, message + ': ' + object_name):
                            ops.backup(bundle, manifest, upload=upload)
                    self.assertTrue(any(verb == 'HEAD' and path.endswith('/' + object_name)
                                        for verb, path in requests))
                    self.assertEqual(manifest, previous)
                    self.assertEqual(json.loads((bundle / 'manifest.json').read_text()), previous)
                else:
                    target = Path(ops.backup(bundle, manifest, upload=upload))
                    receipt = json.loads((bundle / 'manifest.json').read_text())['last_successful_backup']
                    self.assertEqual(receipt['off_host'], upload)
                    self.assertEqual(receipt['checksums'], json.loads((target / 'checksums.json').read_text()))
                self.assertFalse(list((volume / 'backups').glob('*/.readback-*')))
                expected = ['store.db', 'config.tar.gz', 'checksums.json']
                if upload and not fault:
                    self.assertEqual([verb for verb, _ in requests], ['PUT', 'HEAD', 'GET'] * 3)
                    self.assertEqual([path.rsplit('/', 1)[-1] for verb, path in requests if verb == 'GET'], expected)
                if not upload:
                    self.assertEqual(requests, [])

    def test_verified_upload(self):
        self.exercise()

    def test_readback_failures_preserve_previous_receipt(self):
        for name in ('store.db', 'config.tar.gz', 'checksums.json'):
            for fault in ('truncated', 'corrupt', 'missing'):
                with self.subTest(object=name, fault=fault):
                    self.exercise(fault, name)

    def test_local_backup_does_not_claim_off_host(self):
        self.exercise(upload=False)


if __name__ == '__main__':
    unittest.main()
