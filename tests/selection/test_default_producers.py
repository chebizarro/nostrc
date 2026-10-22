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
