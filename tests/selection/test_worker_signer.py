"""Admission negatives, not fabricated service/profile acceptance."""
import copy
import hashlib
import importlib.util
import os
from pathlib import Path
import sys
import shutil
import subprocess
import tempfile
import unittest
import unittest.mock
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'scripts'))
import test_selection_services as services
import test_selection_realization as realization

spec = importlib.util.spec_from_file_location('provisioned_nip5f', ROOT / 'tests/runtime/test_provisioned_nip5f.py')
fixture = importlib.util.module_from_spec(spec)
spec.loader.exec_module(fixture)


class WorkerSignerAdmission(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.build = Path(temporary.name)
        self.profile = {'platform': 'linux', 'architecture': os.uname().machine,
            'capabilities': ['signer_endpoint'],
            'configure_args': ['-DENABLE_TCP_IPC=ON', '-DNIP55L_TEST_TRUST_ENV=ON', '-DNIP55L_SECRET_BACKEND=libsecret'],
            'environment': {'NOSTRC_TEST_NIP5F_APPROVAL': services.SIGNER_CONSENT}}
        self.record = {'profile': self.profile, 'repo_root': str(ROOT), 'build_dir': str(self.build),
                       'source_identity': {'kind': 'committed', 'sha': 'a' * 40}}
        self.cache = '\n'.join(['ENABLE_TCP_IPC:BOOL=ON', 'NIP55L_TEST_TRUST_ENV:BOOL=ON',
                               'NIP55L_SECRET_BACKEND:STRING=libsecret', 'CMAKE_HOME_DIRECTORY:INTERNAL=' + str(ROOT), ''])
        (self.build / 'CMakeCache.txt').write_text(self.cache)
        self.platform = patch.object(services.sys, 'platform', 'linux')
        self.platform.start(); self.addCleanup(self.platform.stop)

    def test_contract_is_not_readiness_and_never_launches(self):
        with patch.object(services.subprocess, 'Popen') as launch:
            proof = services.signer_contract(self.record)
        launch.assert_not_called()
        self.assertFalse(proof['readiness'])
        self.assertFalse(proof['complete'])
        self.assertFalse(proof['complete_gate_eligible'])
        self.assertFalse(proof['remember'])
        self.assertEqual(len(proof['inputs']), 4)

    def test_environment_claim_cannot_replace_authored_consent(self):
        self.profile['environment'] = {}
        with patch.dict(os.environ, NOSTRC_TEST_NIP5F_APPROVAL=services.SIGNER_CONSENT):
            with self.assertRaisesRegex(services.ServiceError, 'authored one-shot'):
                services.signer_contract(self.record)

    def test_missing_or_different_consent_refuses(self):
        for value in ('', 'allow', 'all', True):
            with self.subTest(value=value):
                self.profile['environment']['NOSTRC_TEST_NIP5F_APPROVAL'] = value
                with self.assertRaisesRegex(services.ServiceError, 'authored one-shot'):
                    services.signer_contract(self.record)

    def test_configuration_must_be_authored_not_just_cached(self):
        self.profile['configure_args'].pop(0)
        with self.assertRaisesRegex(services.ServiceError, 'explicit authored configuration'):
            services.signer_contract(self.record)

    def test_duplicate_or_conflicting_authored_flag_refuses(self):
        self.profile['configure_args'].append('-DENABLE_TCP_IPC=OFF')
        with self.assertRaisesRegex(services.ServiceError, 'explicit authored configuration'):
            services.signer_contract(self.record)

    def test_realized_configuration_mismatch_refuses(self):
        (self.build / 'CMakeCache.txt').write_text(self.cache.replace('ENABLE_TCP_IPC:BOOL=ON', 'ENABLE_TCP_IPC:BOOL=OFF'))
        with self.assertRaisesRegex(services.ServiceError, 'realized configuration mismatch'):
            services.signer_contract(self.record)

    def test_other_source_build_refuses(self):
        (self.build / 'CMakeCache.txt').write_text(self.cache.replace(str(ROOT), '/unrelated'))
        with self.assertRaisesRegex(services.ServiceError, 'source root mismatch'):
            services.signer_contract(self.record)

    def test_no_architecture_alias_or_host_promotion(self):
        self.profile['architecture'] = 'unsupported-worker-alias'
        with self.assertRaisesRegex(services.ServiceError, 'exact native Linux'):
            services.signer_contract(self.record)
        self.profile['architecture'] = os.uname().machine
        with patch.object(services.sys, 'platform', 'darwin'):
            with self.assertRaisesRegex(services.ServiceError, 'exact native Linux'):
                services.signer_contract(self.record)

    def test_realization_keeps_profile_declaration_guard(self):
        self.profile['capabilities'] = []
        with self.assertRaisesRegex(realization.RealizationError, 'not declared'):
            realization._runtime_capability_evidence(self.record, ['signer_endpoint'])

    def test_realization_records_provision_contract_not_setup(self):
        proof = realization._runtime_capability_evidence(self.record, ['signer_endpoint'])
        self.assertEqual(proof['signer_endpoint'], services.signer_contract(self.record))
        self.assertFalse(proof['signer_endpoint']['readiness'])

    def test_no_passing_or_partial_audit_can_launch_services(self):
        proof = services.signer_contract(self.record)
        resolved = {'runtime_capability_evidence': {'signer_endpoint': proof}}
        for audit in ({}, {'passed': False, 'after_build': True}, {'passed': True, 'after_build': False}):
            with self.subTest(audit=audit), patch.object(services.subprocess, 'Popen') as launch:
                with self.assertRaisesRegex(services.ServiceError, 'complete post-build input audit'):
                    with services.provisioned_signer(self.record, resolved, audit, self.build / 'evidence'):
                        self.fail('unaudited signer admitted')
                launch.assert_not_called()

    def test_passing_booleans_do_not_replace_full_audit_input_bindings(self):
        proof = services.signer_contract(self.record)
        self.record['descriptors'] = [{'name': 'test_nip5f_tcp', 'requires': ['signer_endpoint']}]
        resolved = {'runtime_capability_evidence': {'signer_endpoint': proof},
                    'tests': ['test_nip5f_tcp'], 'command_executions': {}}
        with patch.object(services.subprocess, 'Popen') as launch:
            with self.assertRaisesRegex(services.ServiceError, 'exact full-audit binding'):
                with services.provisioned_signer(self.record, resolved,
                    {'passed': True, 'after_build': True, 'input_hashes': {}}, self.build / 'evidence'):
                    self.fail('passing capability/audit booleans admitted')
            launch.assert_not_called()

    def test_test_phase_cannot_silently_change_authored_consent(self):
        with self.assertRaisesRegex(services.ServiceError, 'test-phase consent differs'):
            services.signer_contract(self.record, dict(os.environ, NOSTRC_TEST_NIP5F_APPROVAL='all'))

    def test_inherited_unauthored_sanitizer_variable_refuses(self):
        # Runs for ordinary and (inherited) sanitizer variants alike.
        with self.assertRaisesRegex(services.ServiceError, 'sanitizer environment differs'):
            services.signer_contract(self.record, dict(getattr(self, 'env', os.environ),
                                                       NOSTRC_TEST_NIP5F_APPROVAL=services.SIGNER_CONSENT,
                                                       LSAN_OPTIONS='suppressions=/tmp/hide-leaks'))

    def test_changed_contract_refuses_before_launch(self):
        proof = copy.deepcopy(services.signer_contract(self.record))
        proof['remember'] = True
        with patch.object(services.subprocess, 'Popen') as launch:
            with self.assertRaisesRegex(services.ServiceError, 'contract mismatch'):
                with services.provisioned_signer(self.record, {'runtime_capability_evidence': {'signer_endpoint': proof}}, {}, self.build / 'evidence'):
                    self.fail('changed consent admitted')
            launch.assert_not_called()

    def test_fixture_cannot_start_without_explicit_consent(self):
        with self.assertRaisesRegex(RuntimeError, 'explicit unidentified'):
            fixture.prepare_fixture(ROOT, self.build, self.build / 'evidence', consent=False)
        self.assertFalse((self.build / 'evidence').exists())

    def test_prebuilt_route_cannot_fallback_to_compilation(self):
        with patch.object(fixture.platform, 'system', return_value='Linux'), patch.object(fixture.subprocess, 'run') as run:
            with self.assertRaisesRegex(RuntimeError, 'required built fixture executable'):
                fixture.prepare_fixture(ROOT, self.build, self.build / 'evidence', consent=True, prebuilt=True)
            run.assert_not_called()


    def test_tools_are_independent_bound_executables(self):
        proof = services.signer_contract(self.record)
        self.assertEqual(set(proof['executables']), {'dbus-daemon', 'gdbus', 'python'})
        for value in proof['executables'].values():
            self.assertTrue(Path(value['path']).is_absolute())
            self.assertEqual(len(value['sha256']), 64)
        self.assertNotIn('gnome_keyring', proof['executables'])
        self.assertNotIn('nak', proof['executables'])

    def test_generated_artifacts_require_the_declared_producer(self):
        self.record['targets'] = {'approver': {'name': 'test_nip5f_approver', 'artifacts': [], 'sources': []}}
        value = str(self.build / 'nips/nip5f/test_nip5f_approver')
        desc = {'requires': ['signer_endpoint']}
        with self.assertRaisesRegex(realization.RealizationError, 'declared producer closure'):
            realization._signer_generated_input(self.record, 'test_nip5f_tcp', desc, ['approver'], value)
        self.record['targets']['approver']['artifacts'] = [value]
        self.assertTrue(realization._signer_generated_input(self.record, 'test_nip5f_tcp', desc, ['approver'], value))
        self.assertFalse(realization._signer_generated_input(self.record, 'unrelated', desc, ['approver'], value))
        self.assertFalse(realization._signer_generated_input(self.record, 'test_nip5f_tcp', desc, ['approver'], str(self.build / 'unknown')))

    def test_schema_is_not_inferred_from_an_arbitrary_utility(self):
        schema = str(self.build / 'apps/grotto/gschemas.compiled')
        self.record['targets'] = {'schema': {'name': 'other', 'artifacts': [],
                                             'sources': [{'path': schema + '.rule', 'generated': True}]}}
        desc = {'requires': ['signer_endpoint']}
        with self.assertRaisesRegex(realization.RealizationError, 'declared producer closure'):
            realization._signer_generated_input(self.record, 'test_nip5f_tcp', desc, ['schema'], schema)
        self.record['targets']['schema']['name'] = 'grotto-schemas'
        self.assertTrue(realization._signer_generated_input(self.record, 'test_nip5f_tcp', desc, ['schema'], schema))



def synthetic_elf(path, symbols):
    """Minimal little-endian ELF64 with one SHT_SYMTAB and its string table."""
    import struct
    strtab = b'\0' + b''.join(name.encode() + b'\0' for name in symbols)
    entries, offset = [b'\0' * 24], 1
    for name in symbols:
        entries.append(struct.pack('<IBBHQQ', offset, 0x12, 0, 0, 0, 0)); offset += len(name) + 1
    symtab = b''.join(entries)
    strtab_off, symtab_off = 64, 64 + len(strtab)
    shoff = symtab_off + len(symtab)
    header = b'\x7fELF\x02\x01\x01' + b'\0' * 9 + struct.pack(
        '<HHIQQQIHHHHHH', 2, 62, 1, 0, 0, shoff, 0, 64, 0, 0, 64, 3, 0)
    sections = (struct.pack('<IIQQQQIIQQ', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0) +
                struct.pack('<IIQQQQIIQQ', 0, 3, 0, 0, strtab_off, len(strtab), 0, 0, 1, 0) +
                struct.pack('<IIQQQQIIQQ', 0, 2, 0, 0, symtab_off, len(symtab), 1, 1, 8, 24))
    Path(path).parent.mkdir(parents=True, exist_ok=True)
    Path(path).write_bytes(header + strtab + symtab + sections)


class SanitizerSignerAdmission(WorkerSignerAdmission):
    """Authored sanitizer variants: instrumentation is bound, never assumed."""
    def setUp(self):
        super().setUp()
        # Canonical build root: the engine refuses indirect (symlinked) inputs.
        self.build = self.build.resolve(strict=True)
        self.record['build_dir'] = str(self.build)
        self.profile.update(instrumentation='asan_ubsan')
        self.profile['configure_args'] += ['-DCMAKE_BUILD_TYPE=Debug', '-DGNOSTR_ENABLE_ASAN=ON']
        self.profile['environment'].update(ASAN_OPTIONS='detect_leaks=1:halt_on_error=1', G_SLICE='always-malloc')
        self.cache += 'GO_ENABLE_ASAN:BOOL=ON\nCMAKE_BUILD_TYPE:STRING=Debug\n'
        (self.build / 'CMakeCache.txt').write_text(self.cache)
        self.env = dict(os.environ, NOSTRC_TEST_NIP5F_APPROVAL=services.SIGNER_CONSENT,
                        ASAN_OPTIONS='detect_leaks=1:halt_on_error=1', G_SLICE='always-malloc')

    def contract(self):
        return services.signer_contract(self.record, self.env)

    # Inherited cases run with the sanitizer phase environment.
    def test_contract_is_not_readiness_and_never_launches(self):
        proof = self.contract()
        self.assertEqual(proof['instrumentation'], 'asan_ubsan')
        self.assertEqual(proof['sanitizer_environment'],
                         {'ASAN_OPTIONS': 'detect_leaks=1:halt_on_error=1', 'G_SLICE': 'always-malloc'})
        self.assertFalse(proof['readiness'])
        self.assertFalse(proof['complete'])

    def test_realization_records_provision_contract_not_setup(self):
        with patch.object(realization, 'phase_environment', return_value=self.env):
            proof = realization._runtime_capability_evidence(self.record, ['signer_endpoint'])
        self.assertEqual(proof['signer_endpoint'], self.contract())

    def test_tools_are_independent_bound_executables(self):
        self.assertEqual(set(self.contract()['executables']), {'dbus-daemon', 'gdbus', 'python'})

    def test_unknown_instrumentation_has_no_contract(self):
        for value in ('tsan', 'msan', 'asan', None):
            with self.subTest(value=value):
                self.profile['instrumentation'] = value
                with self.assertRaisesRegex(services.ServiceError, 'no runtime contract for instrumentation'):
                    self.contract()

    def test_sanitizer_requires_authored_debug_asan(self):
        for flag in ('-DCMAKE_BUILD_TYPE=Debug', '-DGNOSTR_ENABLE_ASAN=ON'):
            with self.subTest(flag=flag):
                args = list(self.profile['configure_args'])
                self.profile['configure_args'].remove(flag)
                with self.assertRaisesRegex(services.ServiceError, 'authored Debug ASan'):
                    self.contract()
                self.profile['configure_args'] = args

    def test_realized_cache_must_be_instrumented(self):
        for old, new in (('GO_ENABLE_ASAN:BOOL=ON', 'GO_ENABLE_ASAN:BOOL=OFF'),
                         ('CMAKE_BUILD_TYPE:STRING=Debug', 'CMAKE_BUILD_TYPE:STRING=Release')):
            with self.subTest(old=old):
                (self.build / 'CMakeCache.txt').write_text(self.cache.replace(old, new))
                with self.assertRaisesRegex(services.ServiceError, 'realized instrumentation mismatch'):
                    self.contract()

    def test_ordinary_contract_refuses_instrumented_cache(self):
        self.profile['instrumentation'] = 'none'
        with self.assertRaisesRegex(services.ServiceError, 'realized instrumentation mismatch: GO_ENABLE_ASAN'):
            self.contract()

    def test_phase_sanitizer_environment_must_match_authored(self):
        for key, value in (('ASAN_OPTIONS', 'detect_leaks=0'), ('G_SLICE', None)):
            with self.subTest(key=key):
                env = dict(self.env)
                if value is None:
                    env.pop(key)
                else:
                    env[key] = value
                with self.assertRaisesRegex(services.ServiceError, 'sanitizer environment differs'):
                    services.signer_contract(self.record, env)

    def test_elf_symbols_are_parsed_not_grepped(self):
        synthetic_elf(self.build / 'asan', ['main', '__asan_init'])
        synthetic_elf(self.build / 'plain', ['main', 'not__asan_init_suffix'])
        self.assertIn('__asan_init', services.elf_symbol_names((self.build / 'asan').read_bytes()))
        self.assertNotIn('__asan_init', services.elf_symbol_names((self.build / 'plain').read_bytes()))
        with self.assertRaisesRegex(services.ServiceError, 'not a little-endian ELF64'):
            services.elf_symbol_names(b'#!/bin/sh\n# __asan_init\n')
        data = (self.build / 'asan').read_bytes()
        for broken in (data[:70], data[:-40]):  # truncated header / section table
            with self.subTest(size=len(broken)):
                with self.assertRaisesRegex(services.ServiceError, 'malformed ELF symbol table'):
                    services.elf_symbol_names(broken)

    def audited_inputs(self, asan):
        hashes = {}
        for relative in services.SIGNER_INPUTS:
            path = ROOT / relative
            hashes[str(path)] = hashlib.sha256(path.read_bytes()).hexdigest()
        for relative in services.SIGNER_BUILD_INPUTS:
            path = self.build / relative
            if relative in services.SIGNER_EXECUTABLES:
                synthetic_elf(path, ['main'] + (['__asan_init'] if asan[relative] else []))
            else:
                path.write_bytes(b'schema')
            hashes[str(path)] = hashlib.sha256(path.read_bytes()).hexdigest()
        return {'passed': True, 'after_build': True, 'input_hashes': hashes}

    def test_uninstrumented_fixture_executable_refuses_before_launch(self):
        proof = self.contract()
        self.record['descriptors'] = [{'name': 'test_nip5f_tcp', 'requires': ['signer_endpoint']}]
        resolved = {'runtime_capability_evidence': {'signer_endpoint': proof},
                    'tests': ['test_nip5f_tcp'], 'command_executions': {}}
        for plain in services.SIGNER_EXECUTABLES:
            with self.subTest(plain=plain):
                audit = self.audited_inputs({name: name != plain for name in services.SIGNER_EXECUTABLES})
                with patch.object(services.subprocess, 'Popen') as launch, \
                        patch.object(services, '_namespace_resources') as resources:
                    with self.assertRaisesRegex(services.ServiceError, 'instrumentation differs.*' + plain):
                        with services.provisioned_signer(self.record, resolved, audit, self.build / 'evidence',
                                                         environment=self.env):
                            self.fail('uninstrumented sanitizer fixture admitted')
                    launch.assert_not_called()
                    resources.assert_not_called()

    def test_instrumented_fixture_evidence_is_recorded(self):
        audit = self.audited_inputs({name: True for name in services.SIGNER_EXECUTABLES})
        evidence = services.signer_instrumentation_evidence(self.build, 'asan_ubsan', audit['input_hashes'])
        self.assertEqual(set(evidence), {str(self.build / name) for name in services.SIGNER_EXECUTABLES})
        for path, value in evidence.items():
            self.assertTrue(value['asan'])
            self.assertFalse(value['ubsan_observed'])
            self.assertEqual(value['sha256'], audit['input_hashes'][path])
        with self.assertRaisesRegex(services.ServiceError, 'differs from authored none'):
            services.signer_instrumentation_evidence(self.build, 'none')
        changed = dict(audit['input_hashes'], **{str(self.build / services.SIGNER_EXECUTABLES[0]): '0' * 64})
        with self.assertRaisesRegex(services.ServiceError, 'differs from its full-audit binding'):
            services.signer_instrumentation_evidence(self.build, 'asan_ubsan', changed)

    def test_test_phase_overlay_is_the_authored_sanitizer_environment(self):
        self.profile['phase_environments'] = {'configure': {}, 'build': {},
                                              'test': {'LSAN_OPTIONS': 'suppressions={source_root}/README.md'}}
        expected = 'suppressions=' + str(ROOT) + '/README.md'
        with self.assertRaisesRegex(services.ServiceError, 'sanitizer environment differs'):
            services.signer_contract(self.record, self.env)
        proof = services.signer_contract(self.record, dict(self.env, LSAN_OPTIONS=expected))
        self.assertEqual(proof['sanitizer_environment']['LSAN_OPTIONS'], expected)


class FixtureOwnedProcessChecks(unittest.TestCase):
    """Fixture-level attestation/sanitizer checks on owned processes."""
    def fake_signer(self, directory, outputs):
        signer = fixture.Signer.__new__(fixture.Signer)
        signer.out = Path(directory); signer.private = Path(directory) / 'private'
        signer.private.mkdir(); signer.finished = False
        signer.result = {'cleanup': {}}
        signer.processes = []
        for name, (exit_code, output) in outputs.items():
            (signer.out / (name + '.log')).write_bytes(output)
            stopped = {'terminated_by_cleanup': True, 'forced_kill': False, 'exit_code': exit_code}
            signer.processes.append((name, unittest.mock.Mock(stop=unittest.mock.Mock(return_value=stopped))))
        return signer

    def test_sanitizer_reports_from_owned_processes_fail_cleanup(self):
        for name in fixture.INSTRUMENTED:
            for marker in fixture.SANITIZER_REPORTS:
                with self.subTest(name=name, marker=marker), tempfile.TemporaryDirectory() as directory:
                    outputs = {'bus': (-15, b''), 'daemon': (0, b'ok\n'), 'approver': (0, b'ok\n')}
                    outputs[name] = (0, b'x\n==1==' + marker + b' detail\n')
                    signer = self.fake_signer(directory, outputs)
                    with self.assertRaisesRegex(RuntimeError, name + ' emitted sanitizer reports'):
                        signer.finish()
                    self.assertTrue(signer.result['private_namespace_absent'])

    def test_clean_owned_processes_pass_and_record_no_reports(self):
        with tempfile.TemporaryDirectory() as directory:
            signer = self.fake_signer(directory, {'bus': (-15, b''), 'daemon': (0, b'a\n'), 'approver': (0, b'b\n')})
            signer.finish()
            self.assertEqual(signer.result['cleanup']['daemon']['sanitizer_reports'], [])
            self.assertNotIn('sanitizer_reports', signer.result['cleanup']['bus'])

    def test_approver_and_daemon_must_exit_gracefully(self):
        for name in ('daemon', 'approver'):
            with self.subTest(name=name), tempfile.TemporaryDirectory() as directory:
                outputs = {'daemon': (0, b''), 'approver': (0, b'')}
                outputs[name] = (-15, b'')
                with self.assertRaisesRegex(RuntimeError, name + ' did not complete graceful'):
                    self.fake_signer(directory, outputs).finish()

    @unittest.skipUnless(sys.platform == 'linux', 'Linux /proc caller attestation')
    def test_caller_attestation_is_exact_path_and_inode(self):
        with tempfile.TemporaryDirectory() as directory:
            binary = Path(directory) / 'owned'
            shutil.copy2(shutil.which('sleep'), binary)
            other = Path(directory) / 'other'
            shutil.copy2(shutil.which('sleep'), other)
            process = subprocess.Popen([str(binary), '30'])
            try:
                owned = unittest.mock.Mock(process=process)
                record = fixture.caller_attestation('approver', owned, binary)
                self.assertTrue(record['same_inode'])
                self.assertEqual(record['observed'], str(binary))
                with self.assertRaisesRegex(RuntimeError, 'caller_executable_identity_unavailable'):
                    fixture.caller_attestation('approver', owned, other)
            finally:
                process.kill(); process.wait()

from realization_support import Project, r as fixture_realization


class WorkerNonSignerSelection(Project):
    def test_non_signer_subset_of_fixture_profile_does_not_provision_or_promote(self):
        self.profile['capabilities'].append('signer_endpoint')
        realized = self.configure()
        self.build_all()
        resolved = fixture_realization.resolve_checks(self.inventory, self.plan, realized, 'synthetic')
        audit = self.audit(realized)
        self.assertTrue(audit['passed'], audit['gaps'])
        self.assertNotIn('signer_endpoint', resolved['required_capabilities'])
        self.assertFalse(resolved['complete_gate_eligible'])
        self.assertFalse(audit['complete_gate_eligible'])
        with patch.object(services, 'provisioned_signer') as provision:
            receipt = fixture_realization.run_resolved(resolved, self.build, self.evidence,
                                                       input_audit=audit, build=False)
            provision.assert_not_called()
        self.assertEqual(receipt['outcome'], 'passed')
        self.assertFalse(receipt['complete'])
        self.assertFalse(receipt['complete_gate_eligible'])


if __name__ == '__main__':
    unittest.main()
