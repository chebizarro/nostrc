"""Finite runtime diagnostics: source enrollment and actual pre-resource refusals.

These are not live Docker/TCP acceptance or a new selector capability. The live
standalone runners require the separately retained operator/provisioning proof.
"""
import ast
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
import xml.etree.ElementTree as ET

from selector_support import ROOT, s


class RuntimeDiagnosticInputs(unittest.TestCase):
    def test_missing_tcp_approval_refuses_before_build_or_service_access(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            evidence = root / 'must-not-exist'
            result = subprocess.run([sys.executable,
                str(ROOT / 'tests/runtime/test_provisioned_nip5f.py'),
                '--source-root', str(ROOT), '--build', str(root / 'absent-build'),
                '--evidence-dir', str(evidence)], capture_output=True)
            self.assertEqual(result.returncode, 2, result.stderr)
            self.assertIn(b'--approve-unidentified-get-public-key', result.stderr)
            self.assertFalse(evidence.exists())

    def test_cache_untrusted_receipt_refuses_before_docker_or_output_creation(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            candidate = root / 'candidate.json'
            candidate.write_text(json.dumps({'cleanup_complete': False}))
            evidence = root / 'must-not-exist'
            result = subprocess.run([sys.executable, str(ROOT / 'scripts/mdk011-cache-build.py'),
                '--source-root', str(ROOT), '--fetch-receipt', str(candidate),
                '--evidence-dir', str(evidence)], capture_output=True)
            self.assertEqual(result.returncode, 1, result.stderr)
            self.assertIn(b'not a successful current guarded image build', result.stderr)
            self.assertFalse(evidence.exists())

    def test_changed_fetch_recipe_refuses_before_daemon_access(self):
        # Deliberately untrusted candidate, never a successful producer attestation.
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            producer = root / 'producer'
            producer.mkdir()
            original = ROOT / 'tests/interop/mdk/driver-0.11'
            for name in ('Cargo.toml', 'Cargo.lock', 'rust-toolchain.toml'):
                shutil.copyfile(original / name, producer / name)
            recipe = ROOT / 'tests/runtime/mdk011-fetch.Dockerfile'
            (producer / 'Dockerfile').write_bytes(recipe.read_bytes() + b'\nRUN true\n')
            import importlib.util
            spec = importlib.util.spec_from_file_location('diagnostic_gate', ROOT / 'scripts/mdk-gate.py')
            gate = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(gate)
            candidate = root / 'candidate.json'
            candidate.write_text(json.dumps({'status': 'docker_lifecycle_succeeded',
                'cleanup_complete': True, 'complete': False,
                'helper': {'sha256': hashlib.sha256((ROOT / 'scripts/mdk-gate.py').read_bytes()).hexdigest()},
                'image_results': [{'version': '011', 'build_complete': True,
                    'context': str(producer), 'digest': gate.digest(producer)}]}))
            evidence = root / 'must-not-exist'
            result = subprocess.run([sys.executable, str(ROOT / 'scripts/mdk011-cache-build.py'),
                '--source-root', str(ROOT), '--fetch-receipt', str(candidate),
                '--evidence-dir', str(evidence)], capture_output=True)
            self.assertEqual(result.returncode, 1, result.stderr)
            self.assertIn(b'unreviewed Cargo fetch producer recipe', result.stderr)
            self.assertFalse(evidence.exists())

    def test_runtime_diagnostics_do_not_admit_raw_receipts_or_capabilities(self):
        policy = s.load_inventory(ROOT / 'tests/selection/inventory.json')
        for profile in policy['profiles'].values():
            self.assertNotIn('signer_endpoint', profile.get('capabilities', []))
        schema = ROOT / 'apps/grotto/data/org.nostr.Grotto.gschema.xml'
        self.assertTrue(ET.parse(schema).getroot().findall('schema'))
        for runner in ('test_provisioned_nip5f.py', 'test_mdk011_cache_contract.py',
                       'test_mdk011_export_custody.py'):
            path = ROOT / 'tests/runtime' / runner
            ast.parse(path.read_bytes(), filename=str(path))
        for path, owner in [('apps/grotto/data/org.nostr.Grotto.gschema.xml', 'grotto.gui'),
                            ('tests/interop/mdk/driver-0.11/src/main.rs', 'mdk011.peer_inputs')]:
            self.assertEqual(set(s.classify(policy, path)['nodes']), {owner, 'selection.contract'})
        self.assertNotIn('selection.contract', s.classify(policy, 'apps/grotto/src/main.c')['nodes'])
        # Their CLI runners are intentionally not auto-discovered as selector tests.
        check = policy['checks']['selection.contract']
        self.assertNotIn('tests/runtime', check['command'])
        for path in ('tests/runtime/test_provisioned_nip5f.py',
                     'tests/runtime/test_mdk011_cache_contract.py',
                     'tests/runtime/nip5f_approver.c', 'scripts/mdk011-cache-build.py',
                     'tests/runtime/mdk011-fetch.Dockerfile'):
            self.assertIn('{source}/' + path, check['runtime_inputs'])
