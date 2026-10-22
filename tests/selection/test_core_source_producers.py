"""Exact root-owned producer repairs; fragment checks are not profile acceptance."""
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile
import unittest
import shutil

from realization_support import Project

from closeout_support import (before_closeout_contract, before_core_producer_source,
                              core_source_witness, CORE_SOURCE_WITNESS)
from selector_support import ROOT, change, s

FIXTURE = CORE_SOURCE_WITNESS


def between(text, start, end):
    return text[text.index(start):text.index(end, text.index(start))]


def fragments():
    text = {p: (ROOT / p).read_text() for p in (
        'apps/gnostr/CMakeLists.txt', 'apps/grotto/CMakeLists.txt',
        'libnostr/CMakeLists.txt', 'nostr-gtk/CMakeLists.txt',
        'components/nostrdb/CMakeLists.txt')}
    gn = text['apps/gnostr/CMakeLists.txt']
    lib = text['libnostr/CMakeLists.txt']
    return {
        'gnostr_config': between(gn, 'set(_NOSTR_CONFIG_DIR', 'execute_process('),
        'gnostr_build_info': between(gn, 'configure_file(\n  ${CMAKE_CURRENT_SOURCE_DIR}/src/gnostr-build-info.h.in', 'set(gnostr_VERSION'),
        'libnostr_config': between(lib, 'configure_file(${CMAKE_CURRENT_SOURCE_DIR}/include/nostr-config.h.in', 'set(NOSTR_PUBLIC_VERSION_HEADER'),
        'libnostr_talstr': between(lib, '    if(NOT EXISTS "${LIBNOSTR_NDB_VENDOR_PATH}/src/bolt11/talstr.h")', '    add_library(nostrdb STATIC'),
        'libnostr_hex': between(lib, '    set(NDB_HEX_COMPAT', '    # Force-include'),
        'vendored_compat': between(text['components/nostrdb/CMakeLists.txt'], '  set(_nostrdb_compat', '  nostrc_add_vendored_nostrdb('),
        'grotto_xml': between(text['apps/grotto/CMakeLists.txt'], 'set(GROTTO_CONTROL_XML ', 'install(FILES "${GROTTO_CONTROL_XML}"'),
        'gtk_version': between(text['nostr-gtk/CMakeLists.txt'], 'set(NOSTR_GTK_PUBLIC_VERSION_HEADER', '# ── Blueprint compilation'),
    }


class CoreSourceProducers(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.data = core_source_witness()

    def run_fragment(self, name, fragment, build, *, enabled, commit):
        source = {'gnostr_config': 'apps/gnostr', 'gnostr_build_info': 'apps/gnostr',
                  'grotto_xml': 'apps/grotto', 'gtk_version': 'nostr-gtk',
                  'vendored_compat': 'components/nostrdb'}.get(name, 'libnostr')
        # CMake -P has no directory project context: make the one existing
        # relative configure call absolute, without changing its variables.
        fragment = fragment.replace('configure_file(daemon/grotto_control_xml.h.in grotto_control_xml.h @ONLY)',
                                    'configure_file("${CMAKE_CURRENT_SOURCE_DIR}/daemon/grotto_control_xml.h.in" '
                                    '"${CMAKE_CURRENT_BINARY_DIR}/grotto_control_xml.h" @ONLY)')
        fragment = fragment.replace(
            'get_filename_component(_nostrc_cmake_dir "${CMAKE_CURRENT_LIST_DIR}/../../cmake" ABSOLUTE)',
            'set(_nostrc_cmake_dir [==[' + str(ROOT / 'cmake') + ']==])')
        script = build / 'producer.cmake'
        variables = {
            'CMAKE_CURRENT_SOURCE_DIR': str(ROOT / source),
            'CMAKE_CURRENT_LIST_DIR': str(ROOT / source),
            'CMAKE_CURRENT_BINARY_DIR': str(build), 'PROJECT_ROOT': str(ROOT),
            '_nostrc_cmake_dir': str(ROOT / 'cmake'),
            '_NOSTR_CONFIG_DIR': str(build / 'generated'),
            'NDB_COMPAT_DIR': str(build / 'nostrdb_compat'),
            'NDB_TALSTR_HEADER': str(build / 'nostrdb_compat/talstr.h'),
            'LIBNOSTR_NDB_VENDOR_PATH': str(ROOT / 'third_party/nostrdb'),
            'NDB_VENDOR_PATH': str(ROOT / 'third_party/nostrdb'),
            'vendor_path': str(ROOT / 'third_party/nostrdb'),
            'target': 'nostrdb_vendored', 'NOSTR_HAVE_GLIB': enabled,
            'NOSTR_HAVE_LIBGO': enabled, 'GNOSTR_VERSION': '0.1.0',
            'GNOSTR_GIT_COMMIT': commit, 'NOSTR_GTK_MAJOR_VERSION': '1',
            'NOSTR_GTK_MINOR_VERSION': '2', 'NOSTR_GTK_MICRO_VERSION': '0',
            'NOSTR_GTK_VERSION': '1.2.0',
        }
        (build / 'nostrdb_compat').mkdir(exist_ok=True)
        script.write_text('\n'.join('set(' + k + ' [==[' + v + ']==])' for k, v in variables.items()) +
                          '\ninclude([==[' + str(ROOT / 'cmake/NostrcConfigureProvenance.cmake') + ']==])\n' + fragment)
        result = subprocess.run(['cmake', '-P', str(script)], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return {str(p.relative_to(build)): p.read_bytes() for p in build.rglob('*.h')}

    def test_all_nine_generated_paths_preserve_bytes_and_configuration_inputs(self):
        after = fragments()
        observed = set()
        for enabled, commit in [('OFF', '3233857b7'), ('ON', 'unknown')]:
            for name, old in self.data['before_fragments'].items():
                with self.subTest(name=name, enabled=enabled), tempfile.TemporaryDirectory() as directory:
                    outputs = []
                    for label, fragment in [('before', old), ('after', after[name])]:
                        build = Path(directory) / label
                        build.mkdir()
                        outputs.append(self.run_fragment(name, fragment, build, enabled=enabled, commit=commit))
                    self.assertEqual(outputs[0], outputs[1])
                    self.assertTrue(outputs[1])
                    source = {'gnostr_config': 'apps/gnostr', 'gnostr_build_info': 'apps/gnostr',
                              'grotto_xml': 'apps/grotto', 'gtk_version': 'nostr-gtk',
                              'vendored_compat': 'components/nostrdb'}.get(name, 'libnostr')
                    for path, raw in outputs[1].items():
                        full = source + '/' + path
                        observed.add(full)
                        if enabled == 'OFF':
                            self.assertEqual(hashlib.sha256(raw).hexdigest(),
                                             self.data['before_generated'][full]['sha256'], full)
        self.assertEqual(observed, set(self.data['before_generated']))

    def test_gnostr_config_is_refreshed_from_template_without_rewriting_equal_bytes(self):
        with tempfile.TemporaryDirectory() as directory:
            build = Path(directory)
            fragment = fragments()['gnostr_config']
            self.run_fragment('gnostr_config', fragment, build, enabled='OFF', commit='unknown')
            header = build / 'generated/nostr-config.h'
            first_bytes, first_mtime = header.read_bytes(), header.stat().st_mtime_ns
            self.run_fragment('gnostr_config', fragment, build, enabled='OFF', commit='unknown')
            self.assertEqual((header.read_bytes(), header.stat().st_mtime_ns), (first_bytes, first_mtime))
            header.write_bytes(b'/* stale unproven generated bytes */\n')
            self.run_fragment('gnostr_config', fragment, build, enabled='OFF', commit='unknown')
            self.assertEqual(header.read_bytes(), first_bytes)

    def test_source_projection_is_byte_exact_and_rejects_unrelated_changes(self):
        for path, record in self.data['source_edits'].items():
            current = (ROOT / path).read_bytes()
            previous = before_core_producer_source(path, current)
            self.assertEqual(hashlib.sha256(previous).hexdigest(), record['before_sha256'])
            self.assertEqual(before_core_producer_source(path, previous), previous)
            with self.assertRaises(s.ContractError):
                before_core_producer_source(path, current + b'\n')
            with self.assertRaises(s.ContractError):
                before_core_producer_source(path, previous + b'\n')

    def test_exact_owner_edges_select_consumers_without_reassigning_sources(self):
        policy = s.load_inventory(ROOT / 'tests/selection/inventory.json')
        paths = {'apps/relayd/include/relayd_async_storage.h': 'relayd.async_storage_api',
                 'libmarmot/tests/vectors/mdk-0.11/adopted_commits_fixture.h': 'marmot.vector_inputs',
                 'libmarmot/tests/vectors/mdk-0.11/adopted_fixture.h': 'marmot.vector_inputs',
                 'libmarmot/tests/vectors/mdk-0.11/convergence_forks_fixture.h': 'marmot.vector_inputs'}
        for path, node in paths.items():
            self.assertEqual(s.classify(policy, path)['nodes'], [node])
            self.assertIn(node, policy['checks']['core.default_build']['nodes'])
            self.assertIn('core.default_build', s.make_plan(policy, [change(path)], head='a' * 40)['selected_checks'])
        self.assertIn('libmarmot', policy['checks']['supported.apps.gnostr.plugins.mls.groups']['nodes'])
        for profile in ('groundhog.linux', 'grotto.linux'):
            contract = policy['diagnostic_nonmember_subsets'][profile]
            previous = before_closeout_contract(contract, profile)
            self.assertEqual(len(contract['review']['source_inputs']), 193)
            self.assertEqual(set(contract['review']['source_inputs']), set(previous['review']['source_inputs']))
            self.assertEqual(contract['registrations'], previous['registrations'])
            for path, old in self.data['before_controls'].items():
                self.assertEqual(previous['review']['source_inputs'][path], old)
                self.assertEqual(contract['review']['source_inputs'][path], hashlib.sha256((ROOT / path).read_bytes()).hexdigest())


class CoreConfigureWitnesses(Project):
    def prepare_producers(self, omit_grotto_witness=False):
        for path in ('cmake/NostrcConfigureProvenance.cmake',
                     'cmake/nostrdb-talstr.h.in', 'cmake/nostrdb-hex-compat.h.in',
                     'apps/gnostr/src/nostr-config.h.in', 'apps/gnostr/src/gnostr-build-info.h.in',
                     'apps/grotto/daemon/grotto_control_xml.h.in',
                     'apps/grotto/data/org.nostr.Grotto.Control1.xml',
                     'libnostr/include/nostr-config.h.in',
                     'nostr-gtk/include/nostr-gtk-1.0/nostr-gtk-version.h.in'):
            destination = self.root / path
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(ROOT / path, destination)
        source = fragments()
        blocks = {
            'apps/gnostr': 'set(GNOSTR_VERSION 0.1.0)\nset(GNOSTR_GIT_COMMIT fixed-input)\n' + source['gnostr_config'] + source['gnostr_build_info'],
            'apps/grotto': source['grotto_xml'],
            'nostr-gtk': 'set(NOSTR_GTK_MAJOR_VERSION 1)\nset(NOSTR_GTK_MINOR_VERSION 2)\nset(NOSTR_GTK_MICRO_VERSION 0)\nset(NOSTR_GTK_VERSION 1.2.0)\n' + source['gtk_version'],
            'libnostr': 'set(NDB_COMPAT_DIR "${CMAKE_CURRENT_BINARY_DIR}/nostrdb_compat")\n'
                        'file(MAKE_DIRECTORY "${NDB_COMPAT_DIR}")\n'
                        'set(NDB_TALSTR_HEADER "${NDB_COMPAT_DIR}/talstr.h")\n'
                        'set(LIBNOSTR_NDB_VENDOR_PATH "${PROJECT_ROOT}/third_party/nostrdb")\n' +
                        source['libnostr_config'] + source['libnostr_talstr'] + source['libnostr_hex'],
            'components/nostrdb': 'set(NDB_VENDOR_PATH "${PROJECT_ROOT}/third_party/nostrdb")\n' + source['vendored_compat'],
        }
        for directory, block in blocks.items():
            if directory == 'apps/grotto' and omit_grotto_witness:
                block = block[:block.index('nostrc_record_configure_output(')]
            path = self.root / directory / 'CMakeLists.txt'
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text('set(PROJECT_ROOT "${CMAKE_SOURCE_DIR}")\n'
                            'set(_nostrc_cmake_dir "${CMAKE_SOURCE_DIR}/cmake")\n' + block)
        cmake = self.root / 'CMakeLists.txt'
        cmake.write_text(cmake.read_text() + '\ninclude(cmake/NostrcConfigureProvenance.cmake)\n' +
                         '\n'.join('add_subdirectory(' + p + ')' for p in blocks) + '\n')

    def test_actual_direct_producer_fragments_admit_all_nine_outputs(self):
        self.prepare_producers()
        realized = self.configure()
        expected = set(json.loads((ROOT / FIXTURE).read_text())['before_generated'])
        self.assertEqual({str(Path(p).relative_to(self.build)) for p in realized['configure_outputs']}, expected)
        for output, record in realized['configure_outputs'].items():
            self.assertEqual(record['producer'], str(self.root / Path(output).relative_to(self.build).parent.parent / 'CMakeLists.txt')
                             if '/generated/' in output or '/nostrdb_' in output else
                             str(self.root / Path(output).relative_to(self.build).parent / 'CMakeLists.txt'))

    def test_two_configures_same_build_keep_all_nine_fresh_witnesses(self):
        self.prepare_producers()
        first = self.configure()
        header = self.build / 'apps/gnostr/generated/nostr-config.h'
        original = header.read_bytes()
        second = self.configure()
        self.assertEqual(set(first['configure_outputs']), set(second['configure_outputs']))
        self.assertEqual(len(second['configure_outputs']), 9)
        self.assertEqual(header.read_bytes(), original)
        self.assertNotEqual(first['generation_token'], second['generation_token'])

    def test_matching_grotto_final_bytes_without_immediate_witness_are_not_admitted(self):
        self.prepare_producers(omit_grotto_witness=True)
        realized = self.configure()
        output = self.build / 'apps/grotto/grotto_control_xml.h'
        self.assertTrue(output.is_file())
        self.assertNotIn(str(output), realized['configure_outputs'])
        self.assertEqual(len(realized['configure_outputs']), 8)


if __name__ == '__main__':
    unittest.main()
