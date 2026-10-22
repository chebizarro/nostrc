"""Production source dependency/template repairs; miniatures are not profile proof."""
import copy
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest
import xml.etree.ElementTree as ET

from closeout_support import witness
import test_command_execution as execution
from realization_support import Project, ROOT, r
from selector_support import change, s

CMAKE = ROOT / 'gnome/groundhog/CMakeLists.txt'
TEMPLATE = 'gnome/groundhog/src/app/gh-background-autostart.h.in'
DESKTOP = 'gnome/groundhog/data/org.nostr.Groundhog-autostart.desktop.in'


def autostart_fragment(text):
    start = text.index('set(GROUNDHOG_AUTOSTART_DESKTOP ')
    return text[start:text.index('add_library(groundhog-background', start)]


def resource_dependencies(text):
    block = text.split('set(GROUNDHOG_RESOURCE_C ', 1)[1].split('VERBATIM)', 1)[0]
    return {'gnome/groundhog/data/' + p for p in
            re.findall(r'\$\{GROUNDHOG_DATA\}/([^\s)]+)', block.split('DEPENDS', 1)[1])}


class ProductionSourceRepairs(unittest.TestCase):
    def test_all_static_resource_reads_are_declared_without_changing_generated_ui_precedence(self):
        data = witness()['source_producer_repair']
        text = CMAKE.read_text()
        self.assertEqual(hashlib.sha256(CMAKE.read_bytes()).hexdigest(), data['after_cmake_sha256'])
        xml = ET.parse(ROOT / 'gnome/groundhog/data/groundhog.gresource.xml')
        reads = {'gnome/groundhog/data/' + item.text.strip() for item in xml.findall('.//file')
                 if not item.text.strip().startswith('ui/')}
        self.assertFalse(reads - resource_dependencies(text))
        for path in data['missing_static_inputs']:
            self.assertIn(path, reads)
            literal = '${GROUNDHOG_DATA}/' + path.removeprefix('gnome/groundhog/data/')
            counterfactual = text.replace('          ' + literal + '\n', '')
            self.assertEqual(reads - resource_dependencies(counterfactual), {path})
        self.assertIn('${GROUNDHOG_UI_SOURCEDIR} --sourcedir ${GROUNDHOG_DATA}', text)
        self.assertIn('${GROUNDHOG_UI_FILES}', text)

    def test_generated_autostart_header_bytes_are_unchanged_including_escaping(self):
        data = witness()['source_producer_repair']
        current = autostart_fragment(CMAKE.read_text())
        for prefix in ('/usr/local/bin', '/tmp/path with "quotes" and \\slashes'):
            with self.subTest(prefix=prefix), tempfile.TemporaryDirectory() as directory:
                outputs = []
                for name, fragment in [('before', data['before_autostart_fragment']),
                                       ('after', current)]:
                    build = Path(directory) / name
                    build.mkdir()
                    script = Path(directory) / (name + '.cmake')
                    script.write_text(
                        'set(CMAKE_CURRENT_SOURCE_DIR [==[' + str(ROOT / 'gnome/groundhog') + ']==])\n'
                        'set(CMAKE_CURRENT_BINARY_DIR [==[' + str(build) + ']==])\n'
                        'set(GROUNDHOG_DATA [==[' + str(ROOT / 'gnome/groundhog/data') + ']==])\n'
                        'set(GROUNDHOG_BINDIR [==[' + prefix + ']==])\n'
                        'include([==[' + str(ROOT / 'cmake/NostrcConfigureProvenance.cmake') + ']==])\n'
                        + fragment)
                    result = subprocess.run(['cmake', '-P', str(script)], capture_output=True, text=True)
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                    outputs.append((build / 'background/gh-background-autostart.h').read_bytes())
                self.assertEqual(outputs[0], outputs[1])
                if prefix == '/usr/local/bin':
                    self.assertEqual(hashlib.sha256(outputs[1]).hexdigest(), data['before_header_sha256'])

    def test_candidate_hash_adds_only_exact_derived_metadata_to_immutable_contract(self):
        metadata = witness()['source_producer_repair']['candidate_target_metadata']
        fixture = json.loads((ROOT / metadata['packed_fixture']).read_text())
        packed, legacy = fixture['candidate_contract'], {}
        for name, record in packed['registrations'].items():
            legacy[name] = {k: copy.deepcopy(v) for k, v in record.items()
                            if k not in ('target_names', 'input_names')}
            legacy[name]['targets'] = [copy.deepcopy(packed['targets'][n])
                                       for n in record['target_names']]
            legacy[name]['input_hashes'] = {n: packed['source_hashes'][n]
                                            for n in record['input_names']}
        self.assertEqual(s.digest(legacy), metadata['before_hash'])
        current, count = copy.deepcopy(legacy), 0
        for entry in current.values():
            for target in entry['targets']:
                self.assertNotIn(metadata['field'], target)
                target[metadata['field']] = metadata['value']
                count += 1
        self.assertEqual(count, metadata['entries'])
        self.assertEqual(s.digest(current), metadata['after_hash'])
        recovered = copy.deepcopy(current)
        for entry in recovered.values():
            for target in entry['targets']:
                self.assertEqual(target.pop(metadata['field']), metadata['value'])
        self.assertEqual(recovered, legacy)
        self.assertEqual(s.digest(recovered), metadata['before_hash'])
        next(iter(current.values()))['targets'][0][metadata['field']] = '{build}/unreviewed'
        self.assertNotEqual(s.digest(current), metadata['after_hash'])

    def test_benchmark_configure_input_selects_each_actual_root_default_check(self):
        policy = s.load_inventory(ROOT / 'tests/selection/inventory.json')
        plan = s.make_plan(policy, [change('benchmark/CMakeLists.txt')], head='a' * 40)
        for ident in ('core.default_build', 'groundhog.default_build', 'grotto.default_build'):
            self.assertIn('benchmarks', policy['checks'][ident]['nodes'])
            self.assertIn(ident, plan['selected_checks'])
        self.assertEqual(s.classify(policy, TEMPLATE)['nodes'],
                         s.classify(policy, 'gnome/groundhog/src/app/gh-background.c')['nodes'])


class AutostartConfigureWitness(Project):
    default_contract = execution.DefaultBuildContracts.default_contract
    configured_default = execution.DefaultBuildContracts.configured_default

    def prepare_autostart(self, *, omit_header_witness=False):
        for source, relative in [(ROOT / TEMPLATE, 'src/app/gh-background-autostart.h.in'),
                                 (ROOT / DESKTOP, 'data/org.nostr.Groundhog-autostart.desktop.in'),
                                 (ROOT / 'cmake/NostrcConfigureProvenance.cmake',
                                  'cmake/NostrcConfigureProvenance.cmake')]:
            target = self.root / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(source, target)
        fragment = autostart_fragment(CMAKE.read_text())
        if omit_header_witness:
            call = ('nostrc_record_configure_output("${CMAKE_CURRENT_LIST_FILE}"\n'
                    '  "${CMAKE_CURRENT_SOURCE_DIR}/src/app/gh-background-autostart.h.in"\n'
                    '  "${GROUNDHOG_BACKGROUND_GENERATED}/gh-background-autostart.h")\n')
            self.assertEqual(fragment.count(call), 1)
            fragment = fragment.replace(call, '')
        self.default_contract()
        cmake = self.root / 'CMakeLists.txt'
        cmake.write_text(cmake.read_text() + '\ninclude(cmake/NostrcConfigureProvenance.cmake)\n'
                         'set(GROUNDHOG_DATA "${CMAKE_CURRENT_SOURCE_DIR}/data")\n'
                         'set(GROUNDHOG_BINDIR "/usr/local/bin")\n' + fragment +
                         '\ntarget_include_directories(app PRIVATE "${GROUNDHOG_BACKGROUND_GENERATED}")\n'
                         'target_sources(app PRIVATE "${GROUNDHOG_BACKGROUND_GENERATED}/gh-background-autostart.h")\n')
        main = self.root / 'app.c'
        main.write_text('#include "gh-background-autostart.h"\n' + main.read_text())
        self.inventory['path_rules'] += [
            {'prefix': 'src/app/', 'nodes': ['application']},
            {'prefix': 'data/', 'nodes': ['application']}]
        return self.configured_default()

    def test_exact_production_fragment_has_immediate_source_template_witness_and_full_audit(self):
        realized = self.prepare_autostart()
        header = str(self.build / 'background/gh-background-autostart.h')
        self.assertIn(header, realized['configure_outputs'])
        self.assertFalse(realized['default_build_binding']['gaps'])
        resolved = r.resolve_checks(self.inventory, self.plan, realized, 'synthetic')
        r.build_resolved(resolved, self.build)
        audit = self.audit(realized)
        self.assertTrue(audit['passed'], audit['gaps'])
        self.assertIn(str(self.root / 'src/app/gh-background-autostart.h.in'), audit['input_hashes'])

    def test_missing_header_witness_cannot_be_replaced_by_final_matching_bytes(self):
        realized = self.prepare_autostart(omit_header_witness=True)
        header = str(self.build / 'background/gh-background-autostart.h')
        self.assertNotIn(header, realized['configure_outputs'])
        resolved = r.resolve_checks(self.inventory, self.plan, realized, 'synthetic')
        r.build_resolved(resolved, self.build)
        audit = self.audit(realized)
        self.assertFalse(audit['passed'])
        self.assertTrue(any(finding.get('kind') == 'unproven_default_generated_input'
                            and finding.get('input') == header for finding in audit['gaps']),
                        audit['gaps'])

    def test_source_template_changes_invalidate_existing_generation(self):
        realized = self.prepare_autostart()
        resolved = r.resolve_checks(self.inventory, self.plan, realized, 'synthetic')
        r.build_resolved(resolved, self.build)
        audit = self.audit(realized)
        self.assertTrue(audit['passed'], audit['gaps'])
        template = self.root / 'src/app/gh-background-autostart.h.in'
        template.write_text(template.read_text() + '\n/* changed */\n')
        with self.assertRaises(r.RealizationError):
            r.load_realization(self.build, self.identity)


if __name__ == '__main__':
    unittest.main()
