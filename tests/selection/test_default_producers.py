"""Source/native/tool-bound generators; real no-target builds and full audits."""
import json
import shutil
from pathlib import Path
from unittest import mock

import test_command_execution as execution
from realization_support import Project, ROOT, r
import test_selection_producers as producers


class DefaultProducers(Project):
    default_contract = execution.DefaultBuildContracts.default_contract
    configured_default = execution.DefaultBuildContracts.configured_default

    def add_input(self, name, text):
        path = self.root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text)
        self.inventory['path_rules'].append({'exact': name, 'nodes': ['application']})
        return path

    def copy_contract(self, command='${CMAKE_COMMAND} -E copy', extra=''):
        self.add_input('input.h', '#define GENERATED 0\n')
        self.default_contract('''
add_custom_command(OUTPUT "${CMAKE_BINARY_DIR}/generated.h"
  COMMAND ''' + command + ''' "${CMAKE_SOURCE_DIR}/input.h" "${CMAKE_BINARY_DIR}/generated.h"
  DEPENDS "${CMAKE_SOURCE_DIR}/input.h" VERBATIM)
add_custom_target(header ALL DEPENDS "${CMAKE_BINARY_DIR}/generated.h")
add_custom_target(bundle ALL)
add_dependencies(bundle header)
add_dependencies(app bundle)
target_include_directories(app PRIVATE "${CMAKE_BINARY_DIR}")
''' + extra)
        (self.root / 'app.c').write_text('#include "generated.h"\nint main(void) { return GENERATED; }\n')

    def build_audit(self):
        realized = self.configured_default()
        self.assertFalse(realized['default_build_binding']['gaps'], realized['default_build_binding']['gaps'])
        resolved = r.resolve_checks(self.inventory, self.plan, realized, 'synthetic')
        pre = r.check_inputs(self.inventory, realized)
        self.assertTrue(pre['passed'], pre['gaps'])
        receipt = r.build_resolved(resolved, self.build)
        self.assertNotIn('--target', receipt['commands'][0])
        audit = self.audit(realized)
        self.assertTrue(audit['passed'], audit['gaps'])
        return realized, audit

    def test_public_engine_holds_build_lock_and_releases_on_failure(self):
        import fcntl
        import hashlib
        import importlib.util
        import os
        from types import SimpleNamespace
        spec = importlib.util.spec_from_file_location('freshness_cli', ROOT / 'scripts/test-plan.py')
        cli = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(cli)
        key = hashlib.sha256(os.fsencode(self.build.resolve())).hexdigest()
        lock_path = Path('/tmp') / f'nostrc-test-gate-{os.getuid()}' / ('build-' + key + '.lock')
        def owned(*args, **kwargs):
            with lock_path.open('a') as probe:
                with self.assertRaises(BlockingIOError):
                    fcntl.flock(probe, fcntl.LOCK_EX | fcntl.LOCK_NB)
            raise r.RealizationError('owned transaction failure')
        with mock.patch.object(cli, '_realization_command_owned', side_effect=owned):
            with self.assertRaisesRegex(r.RealizationError, 'owned transaction failure'):
                cli._realization_command(SimpleNamespace(build_dir=self.build), {}, r)
        with lock_path.open('a') as probe:
            fcntl.flock(probe, fcntl.LOCK_EX | fcntl.LOCK_NB)

    def glob_ready(self):
        self.default_contract('file(GLOB fixture_sources CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/*.c")\n')
        realized, audit = self.build_audit()
        control = realized['default_build_binding']['snapshot']['generation_control']
        self.assertIn(control['verifier'], realized['generation_files'])
        self.assertEqual(control['files'][control['verifier']]['sha256'],
                         realized['generation_files'][control['verifier']])
        return realized, audit

    def test_glob_default_and_named_freshness_real_execution(self):
        realized, audit = self.glob_ready()
        # The ordinary manifest prepass is not a payload-freshness proof.
        raw = r._command(['ninja', '-n'], cwd=self.build)
        self.assertIn('Re-checking globbed directories', raw)
        for targets in ([], ['app']):
            proof = r._ninja_freshness(realized, targets)
            self.assertEqual(proof['output'], 'ninja: no work to do.\n')
            self.assertFalse(proof['physical_coverage'])
            self.assertEqual(proof['argv'][0], realized['default_build_binding']['snapshot']['build_tools']['ninja']['path'])
        resolved = r.resolve_checks(self.inventory, self.plan, realized, 'synthetic')
        receipt = r.run_resolved(resolved, self.build, self.evidence, input_audit=audit, build=False)
        self.assertEqual(receipt['outcome'], 'passed')
        self.assertFalse(list(self.build.glob('nostrc-ninja-query-*')))

    def test_glob_missing_executable_is_not_hidden_by_manifest_prepass(self):
        realized, _ = self.glob_ready()
        (self.build / 'app').unlink()
        for targets in ([], ['app']):
            with self.subTest(targets=targets), self.assertRaisesRegex(r.RealizationError, 'not up to date'):
                r._ninja_freshness(realized, targets)

    def test_glob_stale_source_same_bytes_refuses(self):
        import os
        import time
        realized, _ = self.glob_ready()
        stamp = max(time.time_ns(), (self.build / 'app').stat().st_mtime_ns + 1000000000)
        os.utime(self.root / 'app.c', ns=(stamp, stamp))
        with self.assertRaisesRegex(r.RealizationError, 'not up to date'):
            r._ninja_freshness(realized, [])

    def test_glob_new_ignored_member_refuses_without_stamp_dependency(self):
        realized, _ = self.glob_ready()
        with (self.root / '.git/info/exclude').open('a') as stream:
            stream.write('\nnew-untracked.c\n')
        (self.root / 'new-untracked.c').write_text('int new_member;\n')
        with self.assertRaisesRegex(r.RealizationError, 'glob membership changed'):
            r._ninja_freshness(realized, [])

    def test_glob_changed_verifier_refuses_before_execution(self):
        realized, _ = self.glob_ready()
        verifier = Path(realized['default_build_binding']['snapshot']['generation_control']['verifier'])
        marker = self.build / 'must-not-execute'
        with verifier.open('a') as stream:
            stream.write('file(WRITE "' + str(marker) + '" "executed")\n')
        with self.assertRaisesRegex(r.RealizationError, 'stale realization'):
            r._ninja_freshness(realized, [])
        self.assertFalse(marker.exists())

    def test_glob_changed_manifest_refuses(self):
        realized, _ = self.glob_ready()
        with (self.build / 'build.ninja').open('a') as stream:
            stream.write('\n# changed generation\n')
        with self.assertRaisesRegex(r.RealizationError, 'stale realization'):
            r._ninja_freshness(realized, [])

    def test_glob_touched_configure_input_requires_regeneration(self):
        import os
        realized, _ = self.glob_ready()
        source = self.root / 'CMakeLists.txt'
        stamp = source.stat().st_mtime_ns + 1000000000
        os.utime(source, ns=(stamp, stamp))
        with self.assertRaisesRegex(r.RealizationError, 'CMake regeneration needed'):
            r._ninja_freshness(realized, [])

    def test_glob_unknown_control_command_refuses_fresh_capture(self):
        realized, _ = self.glob_ready()
        rules = self.build / 'CMakeFiles/rules.ninja'
        rules.write_text(rules.read_text().replace(' -P ', ' --unknown-control -P '))
        with self.assertRaisesRegex(r.RealizationError, 'unsupported CMake regeneration control command'):
            r._default_snapshot(realized)

    def test_glob_verifier_output_refuses_even_without_stamp_change(self):
        realized, _ = self.glob_ready()
        original = r._command
        def command(argv, *args, **kwargs):
            if '-P' in argv:
                return '-- GLOB mismatch!\n'
            return original(argv, *args, **kwargs)
        with mock.patch.object(r, '_command', side_effect=command):
            with self.assertRaisesRegex(r.RealizationError, 'glob membership changed'):
                r._ninja_freshness(realized, [])

    def test_glob_generation_changed_during_query_refuses(self):
        realized, _ = self.glob_ready()
        original = r._command
        def command(argv, *args, **kwargs):
            result = original(argv, *args, **kwargs)
            if '-f' in argv and '-n' in argv:
                with (self.build / 'build.ninja').open('a') as stream:
                    stream.write('\n# concurrent generation\n')
            return result
        with mock.patch.object(r, '_command', side_effect=command):
            with self.assertRaisesRegex(r.RealizationError, 'stale realization'):
                r._ninja_freshness(realized, [])

    def test_real_copy_header_command_free_target_dependencies_full_audit(self):
        self.copy_contract()
        realized, audit = self.build_audit()
        generated = realized['default_build_binding']['generated_producers']
        self.assertEqual(len(generated['producers']), 1)
        self.assertEqual(len(generated['utilities']), 2)
        self.assertIn(str(self.root / 'input.h'), audit['input_hashes'])
        self.assertIn(str(self.build / 'generated.h'), audit['input_hashes'])
        self.assertEqual((self.build / 'generated.h').read_bytes(), (self.root / 'input.h').read_bytes())

    def test_build_directory_with_spaces_preserves_native_output_aliases(self):
        self.build = self.build.parent / 'actual build with spaces'
        self.copy_contract()
        self.build_audit()
        self.assertTrue((self.build / 'generated.h').is_file())

    def test_shell_redirection_run_cannot_truncate_source_before_admission(self):
        self.add_input('input.h', '#define GENERATED 0\n')
        self.add_input('&>', 'tracked punctuation filename\n')
        victim = self.add_input('victim', 'must remain unchanged\n')
        self.default_contract('''
add_custom_command(OUTPUT "${CMAKE_BINARY_DIR}/copied/input.h"
 COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/copied"
 COMMAND ${CMAKE_COMMAND} -E copy input.h &> victim "${CMAKE_BINARY_DIR}/copied"
 DEPENDS "${CMAKE_SOURCE_DIR}/input.h" "${CMAKE_SOURCE_DIR}/&>" "${CMAKE_SOURCE_DIR}/victim"
 WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
add_custom_target(unsafe ALL DEPENDS "${CMAKE_BINARY_DIR}/copied/input.h")
''')
        realized = self.configured_default()
        self.assertTrue(any('shell operator' in g.get('reason', '') for g in realized['default_build_binding']['gaps']))
        with self.assertRaisesRegex(r.RealizationError, 'unsupported default'):
            r.resolve_checks(self.inventory, self.plan, realized, 'synthetic')
        self.assertEqual(victim.read_text(), 'must remain unchanged\n')
        self.assertFalse((self.build / 'copied/input.h').exists())

    def test_python_valid_bytecode_cache_is_part_of_tool_fingerprint(self):
        import importlib.util
        import importlib.machinery
        import os
        import py_compile
        package = (Path(self.temp.name) / 'external-tool-package').resolve()
        package.mkdir()
        module = package / 'module.py'
        module.write_text('VALUE = 1\n')
        os.utime(module, (1000000000, 1000000000))
        cache = Path(py_compile.compile(str(module), doraise=True))
        before = producers.tree_fingerprints(package)
        original = module.read_bytes()
        # Compile equal-length changed code with the same source timestamp;
        # the cache remains a valid executable module for the original header.
        module.write_text('VALUE = 2\n')
        os.utime(module, (1000000000, 1000000000))
        py_compile.compile(str(module), doraise=True)
        module.write_bytes(original)
        os.utime(module, (1000000000, 1000000000))
        namespace = {}
        exec(importlib.machinery.SourceFileLoader('module', str(module)).get_code('module'), namespace)
        self.assertEqual(namespace['VALUE'], 2)
        after = producers.tree_fingerprints(package)
        self.assertEqual(before[str(module)], after[str(module)])
        self.assertNotEqual(before[str(cache)], after[str(cache)])
        self.assertTrue(cache.read_bytes().startswith(importlib.util.MAGIC_NUMBER))

    def test_actual_blueprint_to_resource_producer_chain(self):
        blueprint = shutil.which('blueprint-compiler')
        if not blueprint or not shutil.which('glib-compile-resources'):
            self.skipTest('Blueprint and GResource tools required')
        if Path('/usr/bin/blueprint-compiler').is_file():
            # Exercise the distribution launcher's explicit module_path, not
            # just a newer site-packages installation shadowing it in PATH.
            blueprint = '/usr/bin/blueprint-compiler'
            self.profile['environment']['PATH'] = '/usr/bin:/bin'
        self.add_input('label.blp', 'using Gtk 4.0;\nGtk.Label { label: "Source bound"; }\n')
        self.add_input('resource.xml', '<gresources><gresource prefix="/fixture"><file>label.ui</file></gresource></gresources>\n')
        self.default_contract('''
add_custom_command(OUTPUT "${CMAKE_BINARY_DIR}/label.ui"
 COMMAND "''' + blueprint + '''" compile --output "${CMAKE_BINARY_DIR}/label.ui" "${CMAKE_SOURCE_DIR}/label.blp"
 DEPENDS "${CMAKE_SOURCE_DIR}/label.blp" VERBATIM)
add_custom_command(OUTPUT "${CMAKE_BINARY_DIR}/resources.c"
 COMMAND glib-compile-resources --generate-source --target "${CMAKE_BINARY_DIR}/resources.c" --sourcedir "${CMAKE_BINARY_DIR}" "${CMAKE_SOURCE_DIR}/resource.xml"
 DEPENDS "${CMAKE_SOURCE_DIR}/resource.xml" "${CMAKE_BINARY_DIR}/label.ui" VERBATIM)
add_custom_target(resources ALL DEPENDS "${CMAKE_BINARY_DIR}/resources.c")
''')
        realized, audit = self.build_audit()
        self.assertIn(str(self.root / 'label.blp'), audit['input_hashes'])
        self.assertIn(str(self.build / 'label.ui'), audit['input_hashes'])
        self.assertEqual(len(realized['default_build_binding']['generated_producers']['producers']), 2)

    def test_unknown_operation_remains_refused_before_execution(self):
        self.copy_contract('${CMAKE_COMMAND} -E copy_if_different')
        realized = self.configured_default()
        self.assertTrue(any('unsupported cmake custom operation' in g.get('reason', '') for g in realized['default_build_binding']['gaps']))
        with self.assertRaisesRegex(r.RealizationError, 'unsupported default'):
            r.resolve_checks(self.inventory, self.plan, realized, 'synthetic')
        self.assertFalse((self.build / 'generated.h').exists())

    def test_undeclared_copy_input_is_not_laundered_by_source_ownership(self):
        self.copy_contract()
        cmake = self.root / 'CMakeLists.txt'
        cmake.write_text(cmake.read_text().replace('DEPENDS "${CMAKE_SOURCE_DIR}/input.h" VERBATIM', 'VERBATIM'))
        realized = self.configured_default()
        self.assertTrue(any('reads undeclared input' in g.get('reason', '') for g in realized['default_build_binding']['gaps']))
        self.assertFalse((self.build / 'generated.h').exists())

    def test_input_ownership_still_blocks_admitted_producer(self):
        self.copy_contract()
        self.inventory['path_rules'][-3]['nodes'] = ['other']  # input.h, before app.c/app.h
        realized = self.configured_default()
        self.assertFalse(realized['default_build_binding']['gaps'])
        audit = r.check_inputs(self.inventory, realized)
        self.assertFalse(audit['passed'])
        self.assertTrue(any(g.get('input') == 'input.h' for g in audit['gaps']), audit)

    def test_custom_source_output_is_never_admitted(self):
        self.copy_contract()
        cmake = self.root / 'CMakeLists.txt'
        cmake.write_text(cmake.read_text().replace('${CMAKE_BINARY_DIR}/generated.h', '${CMAKE_SOURCE_DIR}/generated.h'))
        realized = self.configured_default()
        self.assertTrue(realized['default_build_binding']['gaps'])
        self.assertFalse((self.root / 'generated.h').exists())

    def test_source_trace_and_native_argv_disagreement_refuses(self):
        self.copy_contract()
        realized = self.configured_default()
        trace = self.build / 'nostrc-configure-trace.jsonl'
        lines = [json.loads(line) for line in trace.read_text().splitlines()]
        for event in lines:
            if event.get('cmd') == 'add_custom_command':
                event['args'] = ['copy_if_different' if word == 'copy' else word for word in event['args']]
        trace.write_text('\n'.join(json.dumps(line) for line in lines) + '\n')
        binding, _ = r._attest_default(realized)
        self.assertTrue(any('source/native generator argv' in g.get('reason', '') for g in binding['gaps']))

    def test_native_dependency_and_output_disagreement_refuse(self):
        self.copy_contract()
        realized = self.configured_default()
        snapshot, _ = r._default_snapshot(realized)
        import copy
        def query(argv):
            return r._command(argv, env=query.environment)
        query.environment = r.phase_environment(realized['profile'], realized['repo_root'], 'build')
        key = next(k for k, edge in snapshot['edges'].items() if edge['rule'] == 'CUSTOM_COMMAND')
        mutated = copy.deepcopy(snapshot)
        mutated['edges'][key]['inputs'] = []
        proof = producers.attest(realized, mutated, query)
        self.assertTrue(any('source/native declared dependencies differ' in g.get('reason', '') for g in proof['gaps']))
        self.assertFalse(proof['admitted'])
        trace = self.build / 'nostrc-configure-trace.jsonl'
        lines = [json.loads(line) for line in trace.read_text().splitlines()]
        for event in lines:
            if event.get('cmd') == 'add_custom_command':
                event['args'].insert(2, str(self.build / 'unwritten.h'))
        trace.write_text('\n'.join(json.dumps(line) for line in lines) + '\n')
        proof = producers.attest(realized, snapshot, query)
        self.assertTrue(any('source/native output sets differ' in g.get('reason', '') for g in proof['gaps']))
        self.assertFalse(proof['admitted'])

    def test_bound_source_input_drift_refuses_even_with_matching_native_command(self):
        self.copy_contract()
        realized = self.configured_default()
        with (self.root / 'CMakeLists.txt').open('a') as stream:
            stream.write('\n# source changed after configure\n')
        binding, _ = r._attest_default(realized)
        self.assertTrue(any('configure source input changed' in g.get('reason', '') for g in binding['gaps']))

    def test_unproven_generated_fileapi_source_is_blocking(self):
        self.copy_contract()
        realized = self.configured_default()
        target = next(t for t in realized['targets'].values() if t['name'] == 'app')
        target['sources'].append({'path': str(self.build / 'unknown.h'), 'generated': True, 'compiled': False})
        specs = r._command_specs(self.inventory, realized)
        gaps, _ = r._audit_commands(self.inventory, realized, specs, False)
        self.assertTrue(any(g['kind'] == 'unproven_default_generated_source' for g in gaps), gaps)

    def test_actual_gdbus_and_schema_data_producers(self):
        gdbus, schemas = shutil.which('gdbus-codegen'), shutil.which('glib-compile-schemas')
        if not gdbus or not schemas:
            self.skipTest('gdbus-codegen and glib-compile-schemas required')
        self.add_input('interface.xml', '<node><interface name="org.example.Fixture"><method name="Ping"/></interface></node>\n')
        self.add_input('schema/org.example.Fixture.gschema.xml', '<schemalist><schema id="org.example.Fixture" path="/org/example/fixture/"><key name="enabled" type="b"><default>false</default></key></schema></schemalist>\n')
        self.default_contract('''
add_custom_command(OUTPUT "${CMAKE_BINARY_DIR}/dbus.c" "${CMAKE_BINARY_DIR}/dbus.h"
 COMMAND "''' + gdbus + '''" --generate-c-code "${CMAKE_BINARY_DIR}/dbus" --c-namespace Fixture "${CMAKE_SOURCE_DIR}/interface.xml"
 DEPENDS "${CMAKE_SOURCE_DIR}/interface.xml" VERBATIM)
add_custom_command(OUTPUT "${CMAKE_BINARY_DIR}/schema/gschemas.compiled"
 COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/schema"
 COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_SOURCE_DIR}/schema/org.example.Fixture.gschema.xml" "${CMAKE_BINARY_DIR}/schema"
 COMMAND "''' + schemas + '''" --strict "${CMAKE_BINARY_DIR}/schema"
 DEPENDS "${CMAKE_SOURCE_DIR}/schema/org.example.Fixture.gschema.xml" VERBATIM)
add_custom_target(data ALL DEPENDS "${CMAKE_BINARY_DIR}/dbus.c" "${CMAKE_BINARY_DIR}/schema/gschemas.compiled")
''')
        realized, audit = self.build_audit()
        for name in ('dbus.c', 'dbus.h', 'schema/gschemas.compiled', 'schema/org.example.Fixture.gschema.xml'):
            self.assertIn(str(self.build / name), audit['input_hashes'])
        self.assertTrue(realized['default_build_binding']['generated_producers']['tools'])

    def resource_contract(self, dependency=True):
        if not shutil.which('glib-compile-resources'):
            self.skipTest('glib-compile-resources required')
        self.add_input('resource.xml', '<gresources><gresource prefix="/fixture"><file>payload.txt</file></gresource></gresources>\n')
        self.add_input('payload.txt', 'source-bound payload\n')
        self.default_contract('''
add_custom_command(OUTPUT "${CMAKE_BINARY_DIR}/resources.c"
 COMMAND glib-compile-resources --generate-source --target "${CMAKE_BINARY_DIR}/resources.c" --sourcedir "${CMAKE_SOURCE_DIR}" "${CMAKE_SOURCE_DIR}/resource.xml"
 DEPENDS "${CMAKE_SOURCE_DIR}/resource.xml" ''' + ('"${CMAKE_SOURCE_DIR}/payload.txt"' if dependency else '') + ''' VERBATIM)
add_custom_target(resources ALL DEPENDS "${CMAKE_BINARY_DIR}/resources.c")
''')

    def test_actual_path_resolved_resource_tool_and_xml_read_closure(self):
        self.resource_contract()
        realized, audit = self.build_audit()
        self.assertIn(str(self.root / 'payload.txt'), audit['input_hashes'])
        self.assertIn(str(Path(shutil.which('glib-compile-resources')).resolve()), realized['default_build_binding']['generated_producers']['tools'])

    def test_resource_xml_cannot_hide_an_undeclared_input(self):
        self.resource_contract(dependency=False)
        realized = self.configured_default()
        self.assertTrue(any('reads undeclared input' in g.get('reason', '') and 'payload.txt' in g['reason'] for g in realized['default_build_binding']['gaps']))
        self.assertFalse((self.build / 'resources.c').exists())

    def test_actual_list_expanded_gettext_command_and_pseudo_po(self):
        xgettext, msgfmt = shutil.which('xgettext'), shutil.which('msgfmt')
        if not xgettext or not msgfmt:
            self.skipTest('xgettext and msgfmt required')
        self.add_input('messages.c', 'const char *message = _("Hello");\n')
        self.add_input('cmake/nostrc-pseudo-po.py', (ROOT / 'cmake/nostrc-pseudo-po.py').read_text())
        self.default_contract('''
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(EXTRA_COMMANDS COMMAND "''' + xgettext + '''" --package-name=fixture --from-code=UTF-8 --sort-by-file --directory=${CMAKE_SOURCE_DIR} --output=${CMAKE_BINARY_DIR}/messages.pot --language=C --keyword=_ messages.c)
add_custom_command(OUTPUT "${CMAKE_BINARY_DIR}/messages.pot" ${EXTRA_COMMANDS}
 DEPENDS "${CMAKE_SOURCE_DIR}/messages.c" VERBATIM)
add_custom_command(OUTPUT "${CMAKE_BINARY_DIR}/en@pseudo.po"
 COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/cmake/nostrc-pseudo-po.py" "${CMAKE_BINARY_DIR}/messages.pot" "${CMAKE_BINARY_DIR}/en@pseudo.po" en@pseudo
 DEPENDS "${CMAKE_SOURCE_DIR}/cmake/nostrc-pseudo-po.py" "${CMAKE_BINARY_DIR}/messages.pot" VERBATIM)
add_custom_command(OUTPUT "${CMAKE_BINARY_DIR}/messages.mo"
 COMMAND "''' + msgfmt + '''" --check --output-file=${CMAKE_BINARY_DIR}/messages.mo "${CMAKE_BINARY_DIR}/en@pseudo.po"
 DEPENDS "${CMAKE_BINARY_DIR}/en@pseudo.po" VERBATIM)
add_custom_target(translations ALL DEPENDS "${CMAKE_BINARY_DIR}/messages.mo")
''')
        realized, audit = self.build_audit()
        self.assertIn(str(self.root / 'messages.c'), audit['input_hashes'])
        self.assertIn(str(self.build / 'messages.mo'), audit['input_hashes'])
        self.assertEqual(len(realized['default_build_binding']['generated_producers']['producers']), 3)

    def test_literal_resource_metadata_must_have_exact_expected_bytes(self):
        helper = self.root / 'cmake/NostrcGtkResources.cmake'
        helper.write_text('file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/fixture-resource-inputs.txt" CONTENT "source-bound\\n")\n')
        self.default_contract('include(cmake/NostrcGtkResources.cmake)\n')
        realized = self.configured_default()
        output = self.build / 'fixture-resource-inputs.txt'
        self.assertIn(str(output), realized['default_build_binding']['generated_producers']['literal_outputs'])
        output.write_text('unrelated final bytes\n')
        binding, _ = r._attest_default(realized)
        self.assertTrue(any('literal configure metadata bytes/origin differ' in g.get('reason', '') for g in binding['gaps']))

    def test_schema_directory_cannot_hide_stale_input(self):
        schemas = shutil.which('glib-compile-schemas')
        if not schemas:
            self.skipTest('glib-compile-schemas required')
        self.add_input('schema.xml', '<schemalist/>\n')
        self.default_contract('''
add_custom_command(OUTPUT "${CMAKE_BINARY_DIR}/schema/gschemas.compiled"
 COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/schema"
 COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_SOURCE_DIR}/schema.xml" "${CMAKE_BINARY_DIR}/schema/declared.gschema.xml"
 COMMAND "''' + schemas + '''" "${CMAKE_BINARY_DIR}/schema"
 DEPENDS "${CMAKE_SOURCE_DIR}/schema.xml" VERBATIM)
add_custom_target(data ALL DEPENDS "${CMAKE_BINARY_DIR}/schema/gschemas.compiled")
''')
        self.build.mkdir(exist_ok=True)
        (self.build / 'schema').mkdir(exist_ok=True)
        (self.build / 'schema/unknown.gschema.xml').write_text('<schemalist/>\n')
        realized = self.configured_default()
        self.assertTrue(any('schema directory has unbound inputs' in g.get('reason', '') for g in realized['default_build_binding']['gaps']))

    def test_shell_operators_and_expansion_are_not_custom_argv(self):
        for text in ('cd /tmp && echo x > out', 'cd /tmp && echo $(id)', 'cd /tmp && echo x; echo y', 'cd /tmp && echo `id`', 'cd /tmp && echo *.xml', 'cd /tmp && echo ignored # tail'):
            with self.subTest(command=text), self.assertRaises(producers.ProducerError):
                producers.shell_commands(text)

    def test_tool_fingerprint_change_invalidates_binding(self):
        self.copy_contract()
        realized = self.configured_default()
        actual_hash = producers.hashed
        cmake = str(Path(shutil.which('cmake')).resolve())
        with mock.patch.object(producers, 'hashed', side_effect=lambda path: '0' * 64 if str(path) == cmake else actual_hash(path)):
            with self.assertRaisesRegex(r.RealizationError, 'default'):
                r._validate_default_binding(realized)


class NamedTargetFreshness(Project):
    """Selected-target lanes (no default attestation) under CONFIGURE_DEPENDS.

    Hosted MDK08/Grotto ASan refused after a passing build: raw ``ninja -n``
    reports CMake's always-dirty VerifyGlobs prepass and exits before any
    target, so it can never prove no-work. The configure-bound entry can.
    """

    def glob_ready(self):
        with (self.root / 'CMakeLists.txt').open('a') as stream:
            stream.write('\nfile(GLOB fixture_sources CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/*.c")\n')
        realized = self.configure()
        self.assertNotIn('default_build_binding', realized)
        binding = realized['ninja_generation_binding']
        control = binding['generation_control']
        self.assertTrue(control['verifier'])
        self.assertIn(control['verifier'], control['files'])
        self.assertIn(str(self.build / 'build.ninja'), binding['manifests']['files'])
        self.assertEqual(binding['generation_control']['tools'], binding['build_tools'])
        for path in (control['verifier'], str(self.build / 'CMakeCache.txt'), binding['build_tools']['ninja']['path'],
                     *binding['manifests']['files']):
            self.assertIn(path, realized['generation_files'])
        resolved = r.resolve_checks(self.inventory, self.plan, realized, 'synthetic')
        self.assertEqual(resolved['build_targets'], ['shared'])
        pre = r.check_inputs(self.inventory, realized)
        self.assertTrue(pre['passed'], pre['gaps'])
        r.build_resolved(resolved, self.build)
        audit = self.audit(realized)
        self.assertTrue(audit['passed'], audit['gaps'])
        return realized, resolved, audit

    def retained(self, resolved):
        return json.loads((r._attempt_directory(resolved, self.evidence, 1) / 'ninja-freshness-query.json').read_text())

    def test_binding_is_configure_time_profile_identity(self):
        realized, _, _ = self.glob_ready()
        self.assertEqual(r._ninja_generation_binding(realized), realized['ninja_generation_binding'])
        unbound = {k: v for k, v in realized.items() if k != 'ninja_generation_binding'}
        self.assertNotEqual(r._profile_fingerprint(unbound), realized['profile_fingerprint'])

    def test_named_target_freshness_real_execution(self):
        realized, resolved, audit = self.glob_ready()
        # Causal witness: the former raw dry run cannot prove no-work here.
        raw = r._command(['ninja', '-C', str(self.build), '-n', 'shared'])
        self.assertIn('Re-checking globbed directories', raw)
        self.assertNotIn('ninja: no work to do.', raw)
        receipt = r.run_resolved(resolved, self.build, self.evidence, input_audit=audit, build=False)
        self.assertEqual(receipt['outcome'], 'passed', receipt)
        [proof] = self.retained(resolved)
        self.assertEqual(proof['output'], 'ninja: no work to do.\n')
        self.assertFalse(proof['physical_coverage'])
        self.assertEqual(proof['argv'][0], realized['ninja_generation_binding']['build_tools']['ninja']['path'])
        self.assertEqual(proof['argv'][-1], 'shared')
        self.assertEqual(proof['generation_control_hash'], r._hash(realized['ninja_generation_binding']['generation_control']))
        self.assertFalse(list(self.build.glob('nostrc-ninja-query-*')))

    def test_missing_output_refuses_and_retains_pending_query(self):
        _, resolved, audit = self.glob_ready()
        (self.build / 'shared').unlink()
        with self.assertRaisesRegex(r.RealizationError, 'selected targets are not up to date'):
            r.run_resolved(resolved, self.build, self.evidence, input_audit=audit, build=False)
        [proof] = self.retained(resolved)
        self.assertEqual(proof['outcome'], 'pending_work')
        self.assertIn('shared', proof['output'])
        self.assertNotIn('ctest', json.dumps(json.loads((r._attempt_directory(resolved, self.evidence, 1) / 'receipt.json').read_text())['commands'][-1:]))

    def test_stale_source_same_bytes_refuses(self):
        import os
        import time
        _, resolved, audit = self.glob_ready()
        stamp = max(time.time_ns(), (self.build / 'shared').stat().st_mtime_ns + 1000000000)
        os.utime(self.root / 'main.c', ns=(stamp, stamp))
        with self.assertRaisesRegex(r.RealizationError, 'not up to date'):
            r.run_resolved(resolved, self.build, self.evidence, input_audit=audit, build=False)
        self.assertEqual(self.retained(resolved)[0]['outcome'], 'pending_work')

    def test_new_glob_member_refuses_and_retains_verifier_output(self):
        _, resolved, audit = self.glob_ready()
        with (self.root / '.git/info/exclude').open('a') as stream:
            stream.write('\nnew-untracked.c\n')
        (self.root / 'new-untracked.c').write_text('int new_member;\n')
        with self.assertRaisesRegex(r.RealizationError, 'glob membership changed'):
            r.run_resolved(resolved, self.build, self.evidence, input_audit=audit, build=False)
        [proof] = self.retained(resolved)
        self.assertEqual((proof['outcome'], proof['stage']), ('refused', 'pre_query_validation'))
        self.assertNotEqual(proof['verifier_runs'][-1]['output'], '')
        self.assertNotIn('argv', proof)

    def test_touched_configure_input_requires_regeneration(self):
        import os
        realized, _, _ = self.glob_ready()
        source = self.root / 'CMakeLists.txt'
        stamp = source.stat().st_mtime_ns + 1000000000
        os.utime(source, ns=(stamp, stamp))
        with self.assertRaisesRegex(r.RealizationError, 'CMake regeneration needed') as caught:
            r._ninja_freshness(realized, ['shared'])
        self.assertEqual(caught.exception.proof['outcome'], 'refused')

    def test_changed_manifest_cache_and_verifier_refuse_before_execution(self):
        for name, message in (('build.ninja', 'stale realization'), ('CMakeFiles/rules.ninja', 'stale realization'),
                              ('CMakeCache.txt', 'stale realization'), ('verifier', 'stale realization')):
            with self.subTest(name=name):
                self.setUp()
                realized, resolved, audit = self.glob_ready()
                path = (Path(realized['ninja_generation_binding']['generation_control']['verifier'])
                        if name == 'verifier' else self.build / name)
                marker = self.build / 'must-not-execute'
                with path.open('a') as stream:
                    stream.write('\nfile(WRITE "' + str(marker) + '" "executed")\n' if name == 'verifier' else '\n# changed\n')
                with self.assertRaisesRegex(r.RealizationError, message) as caught:
                    r._ninja_freshness(realized, ['shared'])
                self.assertEqual(caught.exception.proof['outcome'], 'refused')
                self.assertNotIn('argv', caught.exception.proof)
                self.assertFalse(marker.exists())

    def test_build_tool_bytes_change_refuses_binding(self):
        realized, _, _ = self.glob_ready()
        actual = r._bound_build_tools
        def changed(realization, cache):
            tools = actual(realization, cache)
            tools['ninja'] = dict(tools['ninja'], sha256='0' * 64)
            return tools
        with mock.patch.object(r, '_bound_build_tools', side_effect=changed):
            with self.assertRaisesRegex(r.RealizationError, 'Ninja generation binding changed'):
                r._ninja_freshness(realized, ['shared'])

    def test_build_tool_identity_change_refuses(self):
        realized, _, _ = self.glob_ready()
        fake = Path(self.temp.name) / 'bin/ninja'
        fake.parent.mkdir()
        shutil.copy2(shutil.which('ninja'), fake)
        real = shutil.which
        with mock.patch.object(r.shutil, 'which', side_effect=lambda name, path=None: str(fake) if name == 'ninja' else real(name, path=path)):
            with self.assertRaisesRegex(r.RealizationError, 'build tool identity changed'):
                r._ninja_freshness(realized, ['shared'])

    def test_unknown_regeneration_control_argv_refuses_fresh_binding(self):
        realized, _, _ = self.glob_ready()
        rules = self.build / 'CMakeFiles/rules.ninja'
        rules.write_text(rules.read_text().replace(' -P ', ' --unknown-control -P '))
        with self.assertRaisesRegex(r.RealizationError, 'unsupported CMake regeneration control command'):
            r._ninja_generation_binding(realized)
        with self.assertRaisesRegex(r.RealizationError, 'stale realization'):
            r._ninja_freshness(realized, ['shared'])

    def test_query_output_with_any_pending_text_refuses(self):
        realized, _, _ = self.glob_ready()
        original = r._command
        def command(argv, *args, **kwargs):
            result = original(argv, *args, **kwargs)
            return result + '[1/1] pending\n' if '-f' in argv and '-n' in argv else result
        with mock.patch.object(r, '_command', side_effect=command):
            with self.assertRaisesRegex(r.RealizationError, 'not up to date') as caught:
                r._ninja_freshness(realized, ['shared'])
        self.assertIn('[1/1] pending', caught.exception.proof['output'])

    def test_missing_binding_and_unsupported_generator_refuse(self):
        realized, _, _ = self.glob_ready()
        unbound = {k: v for k, v in realized.items() if k != 'ninja_generation_binding'}
        with self.assertRaisesRegex(r.RealizationError, 'configure-bound Ninja generation control') as caught:
            r._ninja_freshness(unbound, ['shared'])
        self.assertEqual((caught.exception.proof['stage'], caught.exception.proof['outcome']), ('binding', 'refused'))
        shutil.rmtree(self.build)
        self.profile['configure_args'] = ['-G', 'Unix Makefiles']
        makefiles = self.configure()
        self.assertNotIn('ninja_generation_binding', makefiles)
        self.assertNotIn('default_build_binding', makefiles)
        with self.assertRaisesRegex(r.RealizationError, 'configure-bound Ninja generation control'):
            r._ninja_freshness(makefiles, ['shared'])

    def test_cancellation_during_freshness_is_generic_cancelled_receipt(self):
        # Retention applies to RealizationError refusals; cancellation stays generic.
        _, resolved, audit = self.glob_ready()
        with mock.patch.object(r, '_ninja_freshness_query', side_effect=KeyboardInterrupt('cancelled during query')):
            with self.assertRaisesRegex(r.RealizationError, 'execution interrupted/failed; retained .*: cancelled during query'):
                r.run_resolved(resolved, self.build, self.evidence, input_audit=audit, build=False)
        attempt = r._attempt_directory(resolved, self.evidence, 1)
        receipt = json.loads((attempt / 'receipt.json').read_text())
        self.assertEqual((receipt['outcome'], receipt['execution_outcome']), ('cancelled', 'cancelled'))
        self.assertFalse((attempt / 'ninja-freshness-query.json').exists())

class TargetlessNinjaLane(Project):
    """Command/registration lanes without build targets still carry the binding."""
    command = execution.CommandExecutionTests.command

    def glob(self):
        lists = self.root / 'CMakeLists.txt'
        lists.write_text(lists.read_text().replace('nostrc_finalize_test_inventory()', '''add_test(NAME standalone COMMAND ${CMAKE_COMMAND} -P ${CMAKE_CURRENT_SOURCE_DIR}/noop.cmake)
nostrc_describe_test(standalone OWNER testing INPUTS noop.cmake)
nostrc_finalize_test_inventory()''') + '\nfile(GLOB fixture_sources CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/*.c")\n')

    def lanes(self):
        self.command()
        self.inventory['checks']['standalone.registration'] = {
            'kind': 'ctest', 'profiles': ['synthetic'], 'ctest_tests': {'synthetic': ['standalone']}, 'nodes': ['testing']}
        return (('command', ['command.actual']), ('registration', ['standalone.registration']))

    def test_targetless_lanes_bind_generation_and_run_without_query(self):
        self.glob()
        for name, selected in self.lanes():
            with self.subTest(lane=name):
                self.plan['selected_checks'] = selected
                realized = self.configure()
                binding = realized['ninja_generation_binding']
                self.assertNotIn('default_build_binding', realized)
                self.assertIn(binding['build_tools']['ninja']['path'], realized['generation_files'])
                self.assertEqual(r._ninja_generation_binding(realized), binding)
                self.build_all()
                resolved = r.resolve_checks(self.inventory, self.plan, realized, 'synthetic')
                self.assertEqual(resolved['build_targets'], [])
                audit = self.audit(realized)
                self.assertTrue(audit['passed'], audit['gaps'])
                evidence = self.evidence / name
                receipt = r.run_resolved(resolved, self.build, evidence, input_audit=audit, build=False)
                self.assertEqual(receipt['outcome'], 'passed', receipt)
                self.assertFalse((r._attempt_directory(resolved, evidence, 1) / 'ninja-freshness-query.json').exists())

    def test_targetless_configure_refuses_path_ninja_mismatch(self):
        self.glob()
        self.lanes()
        fake = Path(self.temp.name) / 'bin/ninja'
        fake.parent.mkdir()
        shutil.copy2(shutil.which('ninja'), fake)
        real = shutil.which
        with mock.patch.object(r.shutil, 'which', side_effect=lambda name, path=None: str(fake) if name == 'ninja' else real(name, path=path)):
            with self.assertRaisesRegex(r.RealizationError, 'configured/PATH build tool identity mismatch: ninja'):
                self.configure()
        self.assertFalse((self.build / 'nostrc-test-realization.json').exists())

    def test_targetless_run_refuses_bound_tool_drift(self):
        self.glob()
        self.lanes()
        self.plan['selected_checks'] = ['command.actual']
        realized = self.configure()
        self.build_all()
        resolved = r.resolve_checks(self.inventory, self.plan, realized, 'synthetic')
        audit = self.audit(realized)
        self.assertTrue(audit['passed'], audit['gaps'])
        ninja = realized['ninja_generation_binding']['build_tools']['ninja']['path']
        record = self.build / 'nostrc-test-realization.json'
        stored = json.loads(record.read_text())
        stored['generation_files'][ninja] = '0' * 64  # bound tool bytes no longer match
        record.write_text(json.dumps(stored))
        with self.assertRaisesRegex(r.RealizationError, 'stale realization metadata: ' + ninja):
            r.run_resolved(resolved, self.build, self.evidence, input_audit=audit, build=False)
