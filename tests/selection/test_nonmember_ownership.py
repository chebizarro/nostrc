"""Frozen production witnesses and real CMake metadata API regressions, not gate acceptance."""
import copy
import json
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

from selector_support import ROOT, change, s


FIXTURE = ROOT / 'tests/selection/fixtures/nonmember_audit_edges.json'


class NonmemberOwnership(unittest.TestCase):
    def setUp(self):
        self.policy = s.load_inventory(ROOT / 'tests/selection/inventory.json')
        self.evidence = json.loads(FIXTURE.read_text())

    def closure(self, nodes):
        result = set(nodes)
        pending = list(nodes)
        while pending:
            for node in self.policy['nodes'][pending.pop()].get('depends_on', []):
                if node not in result:
                    result.add(node)
                    pending.append(node)
        return result

    def declarations(self):
        found = {}
        for path in self.evidence['descriptor_files']:
            text = (ROOT / path).read_text()
            self.assertTrue(text.startswith('include_guard(DIRECTORY)'))
            for name, owners in re.findall(r'nostrc_add_test_owners\("([^"]+)" OWNER ([^)]+)\)', text):
                self.assertNotIn(name, found)
                found[name] = owners.split()
        return found

    def test_all_1923_frozen_relationships_have_registration_local_coverage(self):
        evidence = self.evidence
        self.assertEqual(evidence['source_sha'], 'ab5cdbf053f67508cb0da219bbd75a11b5d6719b')
        self.assertEqual(evidence['audit_file_sha256'], '010f0a424c7c4715f3fc30c8c1873790022122ee1adfcc6ea0aad00a26543680')
        self.assertEqual(evidence['original_findings'], 49158)
        self.assertEqual(evidence['missing_impact_findings'], 48811)
        self.assertEqual(len(evidence['records']), 261)
        self.assertEqual(sum(len(r['witnesses']) for r in evidence['records']), 1923)
        declarations = self.declarations()
        self.assertEqual(set(declarations), {r['test'] for r in evidence['records'] if r['additional_owners']})
        plans = {}
        for record in evidence['records']:
            self.assertEqual(declarations.get(record['test'], []), record['additional_owners'])
            covered = self.closure(record['original_owners'] + record['additional_owners'])
            for witness in record['witnesses']:
                with self.subTest(test=record['test'], input=witness['input']):
                    self.assertIn(witness['node'], covered)
                    self.assertTrue(set(s.classify(self.policy, witness['input'])['nodes']) <= covered)
                    for cid, check in self.policy['checks'].items():
                        if not any(record['test'] in names for names in check.get('ctest_tests', {}).values()):
                            continue
                        self.assertTrue(covered <= self.closure(check['nodes']), cid)
                        path = witness['input']
                        if path not in plans:
                            plans[path] = s.make_plan(self.policy, [change(path)], head='a' * 40, base='b' * 40)
                        self.assertIn(cid, plans[path]['selected_checks'])

    def test_nonmembers_are_still_exactly_unexecuted_and_not_gate_ready(self):
        gap = next(g for g in self.policy['coverage_gaps'] if g['id'] == 'groundhog_sanitizer_execution_contract')
        self.assertEqual(gap['not_selected_status'], 'not_executed')
        self.assertEqual(len(gap['not_selected_registrations']), 347)
        self.assertEqual(set(gap['not_selected_registrations']), set(self.evidence['unclaimed_registrations']))
        selected = {name for check in self.policy['checks'].values()
                    for name in check.get('ctest_tests', {}).get('groundhog.linux.sanitizers', [])}
        self.assertTrue(selected.isdisjoint(gap['not_selected_registrations']))
        self.assertFalse(self.policy['enforcement_ready'])
        self.assertEqual(self.policy['nodes']['groundhog.unclassified']['depends_on'], [])
        self.assertEqual(self.policy['nodes']['shared.tests']['depends_on'], ['libnostr'])

    def test_component_links_do_not_import_test_fixtures_or_hanami_test_linkage(self):
        for component, prerequisites in self.evidence['production_prerequisites'].items():
            self.assertTrue(set(prerequisites) <= set(self.policy['nodes'][component]['depends_on']))
        for node, spec in self.policy['nodes'].items():
            if node.endswith('.test_inputs') or node == 'marmot.vector_inputs':
                continue
            self.assertFalse(any(n.endswith('.test_inputs') or n == 'marmot.vector_inputs'
                                 for n in spec.get('depends_on', [])), node)
        self.assertNotIn('nip34', self.closure(['libhanami']))
        record = next(r for r in self.evidence['records'] if r['test'] == 'test-hanami-blossom-client')
        owners = record['original_owners'] + record['additional_owners']
        self.assertIn('nip34', self.closure(owners))
        without = [n for n in owners if n != 'nip34']
        self.assertNotIn('nip34', self.closure(without))

    def test_test_input_changes_do_not_select_production_consumers(self):
        def selected(path):
            plan = s.make_plan(self.policy, [change(path)], head='a' * 40, base='b' * 40, context='ci')
            self.assertFalse(plan['broadening_reasons'])
            return set(plan['selected_checks'])
        changed = selected('nips/nip46/tests/test_pending_lifetime.c')
        self.assertIn('root.asan.nip46', changed)
        self.assertNotIn('root.asan.nip47', changed)
        self.assertNotIn('sanitizer.groundhog_account', changed)
        changed = selected('nips/nip44/src/core/nip44.c')
        self.assertIn('root.asan.nip46', changed)
        self.assertIn('root.asan.nip47', changed)
        changed = selected('libmarmot/tests/vectors/mdk/crypto-basics.json')
        self.assertIn('root.asan.libmarmot', changed)
        self.assertNotIn('root.asan.nip46', changed)

    def test_new_metadata_and_homed_mapping_are_explicit_inputs(self):
        inputs = self.policy['checks']['selection.contract']['runtime_inputs']
        for path in self.evidence['descriptor_files']:
            self.assertIn('{source}/' + path, inputs)
        self.assertIn('{source}/tests/selection/fixtures/nonmember_audit_edges.json', inputs)
        self.assertIn('{source}/tests/selection/test_nonmember_ownership.py', inputs)
        text = (ROOT / 'gnome/nostr-homed/TestInventory.cmake').read_text()
        self.assertEqual(len(self.evidence['homed_registrations']), 5)
        for row in self.evidence['homed_registrations']:
            self.assertIn('nostrc_describe_test(' + row['test'] + ' OWNER gnome.nostr_homed gnome.nostr_homed.test_inputs TARGETS ' + row['target'], text)
            self.assertTrue((ROOT / row['source']).is_file())
            self.assertTrue((ROOT / row['common_source']).is_file())
        self.assertFalse(any('gnome.nostr_homed.test_inputs' in n.get('depends_on', [])
                             for n in self.policy['nodes'].values()))


class DeferredMetadataAPI(unittest.TestCase):
    def configure(self, mode):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        root = Path(temporary.name)
        child = root / 'nips/nip46'
        child.mkdir(parents=True)
        (child / 'vectors').mkdir()
        (child / 'vectors/fixture.json').write_text('{}')
        (root / 'CMakeLists.txt').write_text(
            'cmake_minimum_required(VERSION 3.20)\nproject(MetadataProjection NONE)\nenable_testing()\n'
            'include("' + str(ROOT / 'cmake/NostrcTestInventory.cmake') + '")\n'
            'add_subdirectory(nips/nip46)\nnostrc_finalize_test_inventory()\n')
        script = 'add_test(NAME vector COMMAND "${CMAKE_COMMAND}" -E true)\n'
        script += 'nostrc_describe_test(vector OWNER nip46 TARGETS core INPUTS "${CMAKE_CURRENT_SOURCE_DIR}/vectors" REQUIRES private_bus EXPECTED pass)\n'
        if mode == 'late':
            script += 'cmake_language(DEFER CALL add_test NAME later COMMAND "${CMAKE_COMMAND}" -E true)\n'
        if mode == 'manual':
            script += 'include(TestInventory.cmake)\n'
        (child / 'CMakeLists.txt').write_text(script)
        metadata = 'include_guard(DIRECTORY)\nnostrc_add_test_owners(vector OWNER nip46.test_inputs)\n'
        if mode == 'late':
            metadata += 'nostrc_add_test_owners(later OWNER nip46.test_inputs)\n'
        if mode == 'duplicate':
            metadata += 'nostrc_add_test_owners(vector OWNER nip46.test_inputs)\n'
        if mode == 'missing':
            metadata += 'nostrc_add_test_owners(absent OWNER nip46.test_inputs)\n'
        if mode == 'duplicate_descriptor':
            metadata += 'nostrc_describe_test(vector OWNER nip46)\n'
        (child / 'TestInventory.cmake').write_text(metadata)
        build = root / 'build'
        result = subprocess.run(['cmake', '-S', str(root), '-B', str(build), '-G', 'Ninja'], capture_output=True, text=True)
        return result, build, child

    def test_deferred_and_explicit_loading_preserve_target_runtime_and_requirements(self):
        for mode in ('late', 'manual'):
            with self.subTest(mode=mode):
                result, build, child = self.configure(mode)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                descriptors = json.loads(next(build.glob('nostrc-test-descriptors-*.json')).read_text())['tests']
                record = next(d for d in descriptors if d['name'] == 'vector')
                self.assertEqual(record['owner'], ['nip46', 'nip46.test_inputs'])
                self.assertEqual(record['targets'], ['core'])
                self.assertEqual(record['inputs'], [str(child / 'vectors')])
                self.assertEqual(record['requires'], ['private_bus'])
                self.assertEqual(record['expected'], ['pass'])
                if mode == 'late':
                    later = next(d for d in descriptors if d['name'] == 'later')
                    self.assertEqual(later['owner'], ['nip46', 'nip46.test_inputs'])

    def test_duplicate_or_unregistered_authorship_still_fails_closed(self):
        for mode, message in [('duplicate', 'duplicate supplement'), ('missing', 'invalid or unregistered'),
                              ('duplicate_descriptor', 'duplicate descriptor')]:
            with self.subTest(mode=mode):
                result, _, _ = self.configure(mode)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn(message, result.stdout + result.stderr)


if __name__ == '__main__':
    unittest.main()
