#!/usr/bin/env python3
"""Execute release workflow Bash with simulated GHCR/docker/metadata responses.

Run: python3 signet/tests/test_release_public_policy.py
Requires Bash, jq and GNU sha256sum (gsha256sum on macOS). No registry requests or container execution.
"""
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import textwrap
import unittest

ROOT = Path(__file__).resolve().parents[2]
WORKFLOW = (ROOT / '.github/workflows/signet-release.yml').read_text()
MANIFEST = b'{"schemaVersion":2,"mediaType":"application/vnd.oci.image.index.v1+json","manifests":[]}'
DIGEST = 'sha256:' + hashlib.sha256(MANIFEST).hexdigest()


def step_script(name):
    step = WORKFLOW.split('      - name: ' + name + '\n', 1)[1]
    step = step.split('\n      - ', 1)[0]
    return textwrap.dedent(step.split('        run: |\n', 1)[1])


PRE = step_script('Check public package publication preconditions')
POST = step_script('Push exactly the scanned image')

# The earlier 44 simulations are replaced by these public-policy cases, with
# additional tag/digest and token-denial coverage for both release variants.
CASES = [
    ('pre_public', 'pre', {}, True),
    *[(f'pre_manifest_{s}', 'pre', {'STATUS': s}, True) for s in ('401', '403', '404')],
    *[(f'pre_unexpected_{s}', 'pre', {'STATUS': s}, False) for s in ('302', '500')],
    ('pre_token_transport', 'pre', {'FAIL': 'token'}, False),
    ('pre_manifest_transport', 'pre', {'FAIL': 'manifest'}, False),
    *[(f'pre_token_{n}', 'pre', {'TOKEN_BODY': b}, False)
      for n, b in [('malformed', '{'), ('missing', '{}'), ('empty', '{"token":""}')]],
    *[(f'pre_token_denied_{s}', 'pre', {'TOKEN_STATUS': s, 'STATUS': '401'}, True)
      for s in ('401', '403', '404')],
    *[(f'pre_metadata_{v}', 'pre', {'GH_TOKEN': 'fake-read-token', 'VISIBILITY': v}, True)
      for v in ('public', 'private', 'unavailable')],
    *[(f'post_manifest_{s}', 'post', {'STATUS': s}, s == '200')
      for s in ('200', '401', '403', '404', '500')],
    ('post_token_transport', 'post', {'FAIL': 'token'}, False),
    ('post_manifest_transport', 'post', {'FAIL': 'manifest'}, False),
    ('post_malformed_token', 'post', {'TOKEN_BODY': '{'}, False),
    ('post_unexpected_token', 'post', {'TOKEN_STATUS': '500'}, False),
    ('post_digest_mismatch', 'post', {'MANIFEST': '{}'}, False),
    ('post_missing_manifest', 'post', {'MISSING_MANIFEST': '1'}, False),
    *[(f'post_token_denied_{s}', 'post', {'TOKEN_STATUS': s}, True)
      for s in ('401', '403', '404')],
]

FAKE = r'''
import json, os, pathlib, sys
args = sys.argv[1:]
command = pathlib.Path(sys.argv[0]).name
with open('calls.jsonl', 'a') as log:
    log.write(json.dumps([command, *args]) + '\n')
if command == 'curl':
    assert args[0] == '-q'
    assert '--connect-timeout' in args and '--max-time' in args
    token_request = args[-1].startswith('https://ghcr.io/token?scope=repository:')
    kind = 'token' if token_request else 'manifest'
    if os.environ.get('FAIL') == kind:
        sys.exit(28)
    if token_request:
        print(os.environ.get('TOKEN_BODY', '{"token":"anonymous-token"}'))
        print(os.environ.get('TOKEN_STATUS', '200'), end='')
    else:
        assert args[-1] == 'https://ghcr.io/v2/chebizarro/nostrc-signet/manifests/' + os.environ['IMAGE'].split(':')[-1]
        authorization = ('Authorization: Bearer anonymous-token'
                         if os.environ.get('TOKEN_STATUS', '200') == '200' else 'Authorization:')
        assert authorization in args
        target = args[args.index('-o') + 1]
        if target != '/dev/null' and not os.environ.get('MISSING_MANIFEST'):
            pathlib.Path(target).write_bytes(os.environ['MANIFEST'].encode())
        print(os.environ.get('STATUS', '200'), end='')
elif command == 'gh':
    assert os.environ.get('GH_TOKEN') == 'fake-read-token'
    visibility = os.environ['VISIBILITY']
    if visibility == 'unavailable':
        sys.exit(1)
    print(visibility)
elif command == 'docker':
    if args[:2] == ['image', 'inspect']:
        print('sha256:local-image' if args[-1] == '{{.Id}}' else
              os.environ['IMAGE'].split(':')[0] + '@' + os.environ['DIGEST'])
    else:
        assert args[0] in ('load', 'push')
else:
    raise AssertionError(command)
'''


class PublicPolicy(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        for command in ('bash', 'jq'):
            if not shutil.which(command):
                raise RuntimeError(f'Required simulation tool missing: {command}')
        cls.sha256sum = shutil.which('gsha256sum') or shutil.which('sha256sum')
        if not cls.sha256sum:
            raise RuntimeError('Required simulation tool missing: GNU sha256sum')
        version = subprocess.run([cls.sha256sum, '--version'], capture_output=True, text=True)
        if version.returncode or 'GNU coreutils' not in version.stdout:
            raise RuntimeError('Simulations require GNU sha256sum; install coreutils on macOS')

    def simulate(self, variant, phase, changes, succeeds):
        with tempfile.TemporaryDirectory(prefix='signet-public-policy-') as directory:
            work = Path(directory)
            (work / 'bin').mkdir()
            (work / 'bin' / 'sha256sum').symlink_to(self.sha256sum)
            for command in ('curl', 'docker', 'gh'):
                path = work / 'bin' / command
                path.write_text(f'#!{sys.executable}\n' + FAKE)
                path.chmod(0o755)
            evidence = work / 'evidence'
            evidence.mkdir()
            hashes = {}
            for name in ('image.tar', 'build.json', 'sbom.spdx.json'):
                content = name.encode()
                (evidence / name).write_bytes(content)
                hashes[name] = hashlib.sha256(content).hexdigest()
            env = dict(os.environ, PATH=str(work / 'bin') + os.pathsep + os.environ['PATH'],
                       GH_TOKEN='', OWNER='chebizarro', DIGEST=DIGEST,
                       IMAGE='ghcr.io/chebizarro/nostrc-signet:0.8.0-rc.1' + variant,
                       MANIFEST=MANIFEST.decode(), TOKEN_BODY='{"token":"anonymous-token"}',
                       TOKEN_STATUS='200', STATUS='200', FAIL='', MISSING_MANIFEST='',
                       GITHUB_OUTPUT=str(work / 'output'),
                       EXPECTED_IDENTITY=json.dumps({'archive_sha256': hashes['image.tar'],
                           'build_sha256': hashes['build.json'], 'image_id': 'sha256:local-image'}),
                       EXPECTED_SBOM=hashes['sbom.spdx.json'])
            env.update(changes)
            result = subprocess.run(['bash', '-c', PRE if phase == 'pre' else POST],
                                    cwd=work, env=env, capture_output=True, text=True, timeout=15)
            self.assertEqual(result.returncode == 0, succeeds, result.stdout + result.stderr)
            calls = [json.loads(line) for line in (work / 'calls.jsonl').read_text().splitlines()]
            self.assertEqual(any(c[0] == 'gh' for c in calls), bool(env['GH_TOKEN']))
            if phase == 'post':
                self.assertIn(['docker', 'push', env['IMAGE']], calls)
                self.assertEqual((evidence / 'digest.txt').read_text(),
                                 f'ghcr.io/chebizarro/nostrc-signet@{DIGEST}\n')
                self.assertEqual((work / 'output').read_text(), f'digest={DIGEST}\n')
                self.assertLess(WORKFLOW.index('manifest_digest='),
                                WORKFLOW.index('- uses: actions/attest-build-provenance'))
            else:
                self.assertFalse(any(c[0] == 'docker' for c in calls))


def make_test(variant, phase, changes, succeeds):
    def test(self):
        self.simulate(variant, phase, changes, succeeds)
    return test


for variant in ('', '-passkeys'):
    for name, phase, changes, succeeds in CASES:
        setattr(PublicPolicy, 'test_' + (variant.strip('-') or 'default') + '_' + name,
                make_test(variant, phase, changes, succeeds))

if __name__ == '__main__':
    unittest.main()
