"""Pinned sibling reconciliation without renewing historical source authorities."""
import copy
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import textwrap
import tempfile
import unittest
from unittest.mock import patch

import closeout_support as c
from profile_contract_support import source_job_blocks
from realization_support import r
from selector_support import ROOT, s


class CloseoutInterfaces(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.p = s.load_inventory(ROOT / 'tests/selection/inventory.json')
        cls.data = c.witness()

    def test_lossless_exact_policy_projection_and_input_enrollment(self):
        self.assertEqual(s.digest(self.p), self.data['after_policy_hash'])
        previous = c.before_closeout_policy(self.p)
        self.assertEqual(s.digest(previous), self.data['before_policy_hash'])
        self.assertEqual(c.before_closeout_policy(previous), previous)
        self.assertEqual(len(self.data['checks']), 5)
        self.assertEqual(len(self.data['profiles']), 2)
        self.assertEqual(len(self.data['workflow_jobs']), 37)
        for path in self.data['new_inputs']:
            self.assertIn(path, self.p['checks']['selection.contract']['runtime_inputs'])
            self.assertTrue((ROOT / path.removeprefix('{source}/')).is_file())

    def test_source_bound_workflow_records_change_only_reviewed_hash_fields(self):
        observed = source_job_blocks(ROOT)
        for ident, record in self.data['workflow_jobs'].items():
            before, after = record['before'], record['after']
            for field in set(before) | set(after):
                if field not in {'source_sha256', 'workflow_sha256'}:
                    self.assertEqual(before.get(field), after.get(field), (ident, field))
            for field in ('source_sha256', 'workflow_sha256', 'matrix_source'):
                self.assertEqual(after[field], observed[ident][field], (ident, field))

    def test_actual_install_script_pins_module_and_reported_version_in_one_argument(self):
        repair = self.data['workflow_install_repair']
        raw = (ROOT / repair['workflow']).read_bytes()
        self.assertEqual(hashlib.sha256(raw).hexdigest(), repair['after_sha256'])
        self.assertEqual(raw.count(repair['after_command'].encode()), 1)
        prior = raw.replace(repair['after_command'].encode(), repair['before_command'].encode())
        self.assertEqual(hashlib.sha256(prior).hexdigest(), repair['before_sha256'])
        block = raw.decode().split('- name: Install pinned nak for ordinary runtime peers', 1)[1]
        script = textwrap.dedent(block.split('- name:', 1)[0].split('run: |\n', 1)[1])
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / 'bin').mkdir()
            nak = root / 'gopath/bin/nak'
            nak.parent.mkdir(parents=True)
            nak.touch(); nak.chmod(0o755)
            go = root / 'bin/go'
            go.write_text('#!' + sys.executable + '\nimport json, os, sys\n'
                          'with open(os.environ["GO_CAPTURE"], "a") as f: '
                          'f.write(json.dumps(sys.argv[1:]) + "\\n")\n'
                          'if sys.argv[1:] == ["env", "GOPATH"]: '
                          'print(os.environ["TEST_GOPATH"])\n')
            go.chmod(0o755)
            capture = root / 'capture.jsonl'
            env = dict(os.environ, PATH=str(root / 'bin') + os.pathsep + os.environ['PATH'],
                       GO_CAPTURE=str(capture), TEST_GOPATH=str(root / 'gopath'),
                       GITHUB_PATH=str(root / 'github-path'))
            result = subprocess.run(['bash', '-c', script], env=env, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertEqual([json.loads(line) for line in capture.read_text().splitlines()],
                             [['install', '-ldflags=-X main.version=v0.19.7', repair['upstream_module']],
                              ['env', 'GOPATH']])
            self.assertEqual((root / 'github-path').read_text(), str(nak.parent) + '\n')

    def test_executable_capabilities_do_not_attest_signer_or_live_docker_setup(self):
        for profile in self.data['profiles']:
            record = self.data['profiles'][profile]
            self.assertEqual(record['before']['capabilities'], ['display', 'private_bus'])
            self.assertEqual(record['after']['capabilities'],
                             ['display', 'private_bus', 'nak', 'gnome_keyring'])
            before, after = copy.deepcopy(record['before']), copy.deepcopy(record['after'])
            before.pop('capabilities'); after.pop('capabilities')
            self.assertEqual(before, after)
            self.assertFalse({'docker', 'signer_endpoint'} &
                             set(self.p['profiles'][profile]['capabilities']))

    def test_contract_projection_preserves_193_keys_and_exact_33_omissions(self):
        for profile in self.data['contracts']:
            current = self.p['diagnostic_nonmember_subsets'][profile]
            previous = c.before_closeout_contract(current, profile)
            self.assertEqual(set(current['review']['source_inputs']),
                             set(previous['review']['source_inputs']))
            self.assertEqual(len(current['review']['source_inputs']), 193)
            self.assertEqual(current['registrations'], previous['registrations'])
            self.assertEqual(len(current['registrations']), 33)
            record = self.data['contracts'][profile]
            a, b = copy.deepcopy(current), copy.deepcopy(previous)
            for path, values in record.get('source_changes', {}).items():
                self.assertEqual(a['review']['source_inputs'][path], values['after'])
                self.assertEqual(b['review']['source_inputs'][path], values['before'])
                self.assertEqual(hashlib.sha256((ROOT / path).read_bytes()).hexdigest(), values['after'])
                a['review']['source_inputs'][path] = values['before']
            self.assertEqual(a['candidate_contract_hash'], record['after_candidate_contract_hash'])
            a['candidate_contract_hash'] = record['before_candidate_contract_hash']
            a.pop('profile_hash'); b.pop('profile_hash')
            self.assertEqual(a, b)

    def test_default_builds_have_real_consumed_source_owners_not_graph_dependencies(self):
        for ident, inputs in self.data['default_inputs'].items():
            check = self.p['checks'][ident]
            self.assertEqual(check['kind'], 'build_contract')
            self.assertEqual(check['build_scope'], 'default')
            self.assertEqual(check['build_targets'], [])
            self.assertEqual(check['command'],
                             ['cmake', '--build', '{build}', '--config', 'Debug'])
            covered = r._impact_prerequisites(self.p, check['nodes'])
            for index in inputs['source_indexes']:
                path = self.data['default_source_paths'][index]
                self.assertTrue((ROOT / path).is_file(), path)
                classified = s.classify(self.p, path)
                self.assertFalse(classified.get('unclassified'), path)
                if classified['effect'] != 'global':
                    self.assertEqual(classified['effect'], 'impact', path)
                    self.assertTrue(set(classified['nodes']) & covered, (ident, path))
            marker = next(n for n in check['nodes'] if n.startswith('execution.'))
            self.assertEqual(self.p['nodes'][marker]['depends_on'], [])

    def test_golden_changes_are_only_explicit_default_build_memberships(self):
        for name, record in self.data['golden_updates'].items():
            self.assertTrue(set(record['before']) <= set(record['after']), name)
            additions = set(record['after']) - set(record['before'])
            self.assertTrue(additions, name)
            for ident in additions:
                self.assertEqual(self.p['checks'][ident]['build_scope'], 'default')
            self.assertEqual(c.closeout_golden_checks(name, record['before'], self.p),
                             record['after'])
            with self.assertRaises(s.ContractError):
                c.closeout_golden_checks(name, record['before'] + ['unknown'], self.p)

    def test_profiles_job_authority_default_commands_and_inputs_cannot_drift(self):
        mutations = [
            lambda p: p['profiles']['grotto.linux']['capabilities'].remove('gnome_keyring'),
            lambda p: p['profiles']['groundhog.linux'].update(architecture='aarch64'),
            lambda p: p['checks']['core.default_build']['command'].extend(['--target', 'all']),
            lambda p: p['checks']['groundhog.default_build']['nodes'].pop(),
            lambda p: p['checks']['selection.contract']['runtime_inputs'].pop(),
            lambda p: p['checks']['selection.contract']['runtime_inputs'].append('{source}/unknown'),
            lambda p: p['workflow_jobs'][next(iter(self.data['workflow_jobs']))].update(
                profiles=['unreviewed.profile']),
            lambda p: p['path_rules'].remove(self.data['path_rules'][0]),
        ]
        for mutate in mutations:
            bad = copy.deepcopy(self.p)
            mutate(bad)
            with self.assertRaises(s.ContractError):
                c.before_closeout_policy(bad)

    def test_contract_and_unknown_source_authority_mutations_refuse(self):
        for profile in self.data['contracts']:
            for mutate in (
                lambda x: x['registrations'].pop(),
                lambda x: x['review']['source_inputs'].update({'unknown.cmake': '0' * 64}),
                lambda x: x['review']['source_inputs'].update(
                    {'gnome/groundhog/CMakeLists.txt': '0' * 64}),
                lambda x: x['review'].update(architecture='aarch64'),
                lambda x: x['registration_universe'].pop(),
                lambda x: x['required_closure'].pop(),
                lambda x: x.update(toolchain_contract_hash='0' * 64),
                lambda x: x.update(candidate_contract_hash='0' * 64),
                lambda x: x.update(profile_hash='0' * 64),
            ):
                bad = copy.deepcopy(self.p['diagnostic_nonmember_subsets'][profile])
                mutate(bad)
                with self.assertRaises(s.ContractError):
                    c.before_closeout_contract(bad, profile)

    def test_witness_byte_changes_refuse_before_projection(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            path = root / c.WITNESS
            path.parent.mkdir(parents=True)
            path.write_bytes((ROOT / c.WITNESS).read_bytes() + b' ')
            with patch.object(c, 'ROOT', root), self.assertRaises(s.ContractError):
                c.witness()

    def test_projection_does_not_erase_unrelated_unknown_changes(self):
        bad = copy.deepcopy(self.p)
        bad['checks']['libgo.channel']['expected_outcome'] = 'unknown'
        self.assertEqual(c.before_closeout_policy(bad)['checks']['libgo.channel']['expected_outcome'],
                         'unknown')
        with self.assertRaises(s.ContractError):
            s.validate_inventory(c.before_closeout_policy(bad))


if __name__ == '__main__':
    unittest.main()
