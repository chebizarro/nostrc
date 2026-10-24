"""Provisioned signer credentials reach only the admitted CTest registration.

Real CMake/Ninja configure, the real test_nip5f_tcp.c consumer and real CTest
processes. Only the Linux-only signer service is replaced: a kernel-assigned
listener records what the consumer sends. Its frame stubs always fail, so the
consumer can never pass against it: this proves delivery and isolation, not
signer coverage (that needs the real provisioned fixture on a Linux worker).
"""
import contextlib
import json
import os
from pathlib import Path
import secrets
import shutil
import socket
import stat
import subprocess
import sys
import tempfile
import threading
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'scripts'))
import test_selection_services as services
from realization_support import Project, r

GROUPS = 'nip5f_signer:1'
CONSUMER = 'test_nip5f_tcp'
DUMPERS = ('probe-setup', 'probe-cleanup', 'unrelated', 'forged-label')


class Listener:
    """Accepts every loopback connection and keeps the bytes each one sent."""

    def __init__(self):
        self.socket = socket.socket()
        self.socket.bind(('127.0.0.1', 0))
        self.socket.listen(8)
        self.socket.settimeout(0.2)
        self.endpoint = 'tcp:127.0.0.1:%d' % self.socket.getsockname()[1]
        self.received, self.stop = [], threading.Event()
        self.thread = threading.Thread(target=self.serve, daemon=True)
        self.thread.start()

    def serve(self):
        while not self.stop.is_set():
            try:
                connection, _ = self.socket.accept()
            except socket.timeout:
                continue
            with connection:
                connection.settimeout(10)
                data = b''
                with contextlib.suppress(OSError):
                    while chunk := connection.recv(4096):
                        data += chunk
                self.received.append(data)

    def close(self):
        self.stop.set()
        self.thread.join(timeout=10)
        self.socket.close()


class ScopedDeliveryProject(Project):
    def setUp(self):
        super().setUp()
        self.out = Path(tempfile.mkdtemp(prefix='scope-out-'))
        self.addCleanup(shutil.rmtree, self.out, True)
        self.private = Path(tempfile.mkdtemp(prefix='scope-'))  # mkdtemp: owner-only 0700
        self.addCleanup(shutil.rmtree, self.private, True)
        (self.root / 'nips/nip5f/tests').mkdir(parents=True)
        shutil.copy2(ROOT / 'nips/nip5f/tests/test_nip5f_tcp.c', self.root / 'nips/nip5f/tests')
        (self.root / 'fixture').mkdir()
        (self.root / 'fixture/json.h').write_text(
            'int nostr_json_get_string(const char *json, const char *key, char **out);\n')
        (self.root / 'fixture/stubs.c').write_text(
            '#include <stddef.h>\n'
            'int nip5f_read_frame(int fd, char **j, size_t *n) { (void)fd; (void)j; (void)n; return -1; }\n'
            'int nip5f_write_frame(int fd, const char *j, size_t n) { (void)fd; (void)j; (void)n; return -1; }\n'
            'int nostr_json_get_string(const char *j, const char *k, char **o) { (void)j; (void)k; (void)o; return -1; }\n')
        dump = '\n'.join(
            f'add_test(NAME {name} COMMAND sh -c "env > {self.out}/{name}.env")\n'
            + (f'nostrc_describe_test({name} OWNER testing INPUTS noop.cmake EXPECTED fixture)' if name.startswith('probe-')
               else f'nostrc_describe_test({name} OWNER application INPUTS noop.cmake)') for name in DUMPERS)
        self.cmake = self.root / 'CMakeLists.txt'
        self.cmake.write_text(self.cmake.read_text().replace('nostrc_finalize_test_inventory()', f'''
add_executable(test_nip5f_tcp nips/nip5f/tests/test_nip5f_tcp.c fixture/stubs.c)
target_include_directories(test_nip5f_tcp PRIVATE fixture)
add_test(NAME test_nip5f_tcp COMMAND test_nip5f_tcp)
set_tests_properties(test_nip5f_tcp PROPERTIES SKIP_RETURN_CODE 77 RESOURCE_GROUPS "{GROUPS}"
  FIXTURES_REQUIRED signerprobe)
nostrc_describe_test(test_nip5f_tcp OWNER application TARGETS test_nip5f_tcp
  INPUTS nips/nip5f/tests/test_nip5f_tcp.c REQUIRES signer_endpoint)
add_test(NAME alias COMMAND test_nip5f_tcp)
set_tests_properties(alias PROPERTIES SKIP_RETURN_CODE 77)
nostrc_describe_test(alias OWNER application TARGETS test_nip5f_tcp INPUTS nips/nip5f/tests/test_nip5f_tcp.c)
{dump}
set_tests_properties(probe-setup PROPERTIES FIXTURES_SETUP signerprobe)
set_tests_properties(probe-cleanup PROPERTIES FIXTURES_CLEANUP signerprobe)
set_tests_properties(forged-label PROPERTIES LABELS "signer_endpoint;nip5f_signer;test_nip5f_tcp")
nostrc_finalize_test_inventory()'''))
        (self.root / 'dump.sh').write_text('env > "$1"\n')
        self.inventory['path_rules'] += [
            {'exact': 'dump.sh', 'nodes': ['application'], 'effect': 'impact'},
            {'prefix': 'nips/', 'nodes': ['application'], 'effect': 'impact'},
            {'prefix': 'fixture/', 'nodes': ['application'], 'effect': 'impact'}]
        for name in (CONSUMER, 'alias', 'unrelated', 'forged-label'):
            self.inventory['checks'][name] = {'kind': 'ctest', 'profiles': ['synthetic'],
                                              'ctest_tests': {'synthetic': [name]}, 'nodes': ['application', 'testing']}
        self.inventory['checks'][CONSUMER]['requires'] = ['signer_endpoint']
        self.inventory['checks']['command.env'] = {
            'kind': 'command', 'nodes': ['application'], 'profiles': ['synthetic'],
            'command': ['sh', '{source}/dump.sh', f'{self.out}/command.env'], 'runtime_inputs': ['{source}/dump.sh'],
            'working_directory': '{source}', 'environment': {}, 'requires': [],
            'cost': 'ordinary', 'requirement': 'blocking', 'expected_outcome': 'pass'}
        self.plan['selected_checks'] = [CONSUMER, 'alias', 'unrelated', 'forged-label', 'command.env']
        self.profile['capabilities'].append('signer_endpoint')
        self.contract = {'kind': 'nip5f_one_shot_provision_contract_v1', 'readiness': False,
                         'inputs': {}, 'executables': {}}
        for target, value in ((services, 'signer_contract'),):
            patcher = patch.object(target, value, lambda *a, **k: dict(self.contract))
            patcher.start()
            self.addCleanup(patcher.stop)

    def export(self, realized):
        return {t['name']: t for t in realized['tests']}


class RealCTestDelivery(ScopedDeliveryProject):
    def setUp(self):
        super().setUp()
        self.realized = self.configure()
        self.build_all()
        self.token = secrets.token_hex(32)
        self.listener = Listener()
        self.addCleanup(self.listener.close)

    def ctest(self, spec, *names):
        indices = [str(i + 1) for i, t in enumerate(self.realized['tests']) if t['name'] in names]
        selection = self.out / 'indices.txt'
        selection.write_text('0,0,1,' + ','.join(indices) + '\n')
        junit = self.out / 'results.xml'
        argv = ['ctest', '--test-dir', str(self.build), '-I', str(selection), '--output-on-failure',
                '--output-junit', str(junit), '--parallel', '4']
        if spec is not None:
            argv += ['--resource-spec-file', str(spec)]
        log = self.out / 'ctest.log'
        with log.open('wb') as stream:
            subprocess.run(argv, stdout=stream, stderr=subprocess.STDOUT, timeout=120)
        return log, junit

    def env_of(self, name):
        return (self.out / (name + '.env')).read_text()

    def test_only_the_admitted_registration_receives_the_real_allocation(self):
        names = [CONSUMER, 'alias', *DUMPERS]
        with services.signer_resource_delivery(self.listener.endpoint, self.token, self.private,
                                               (self.out / 'ctest.log', self.out / 'results.xml')) as (spec, record):
            info, parent = spec.stat(), spec.parent.stat()
            self.assertEqual((stat.S_IMODE(info.st_mode), stat.S_IMODE(parent.st_mode)), (0o600, 0o700))
            self.assertFalse(spec.is_relative_to(self.evidence))
            log, junit = self.ctest(spec, *names)
        self.assertTrue(record['removed'])
        self.assertFalse(spec.exists() or spec.parent.exists())
        self.assertNotIn(self.token, json.dumps(record))
        self.assertEqual(record['exposure_checked'], sorted([str(log), str(junit)]))
        # The real consumer parsed its allocation and authenticated with the
        # delivered token on the delivered port: exactly one connection.
        self.assertEqual(self.listener.received[:1], [b'AUTH ' + self.token.encode() + b'\n'])
        self.assertEqual(len(self.listener.received), 1)
        results = junit.read_text()
        # Scheduled without the allocation: a failure, never a skip that could pass.
        self.assertRegex(results, r'name="alias"[^>]*status="fail"')
        self.assertRegex(results, r'name="test_nip5f_tcp"[^>]*status="fail"')  # stubs: never fake success
        for name in DUMPERS:
            env = self.env_of(name)
            self.assertNotIn(self.token, env, name)
            self.assertNotIn('NIP5F_SIGNER', env, name)
            self.assertNotIn('NOSTR_SIGNER_', env, name)
        for path in (log, junit, *self.build.rglob('Testing/Temporary/*')):
            self.assertNotIn(self.token.encode(), path.read_bytes(), str(path))

    def test_without_a_resource_spec_ctest_allocates_nothing_and_the_consumer_skips(self):
        log, junit = self.ctest(None, CONSUMER, 'alias')
        self.assertEqual(self.listener.received, [])
        self.assertRegex(junit.read_text(), r'name="test_nip5f_tcp"[^>]*status="(skipped|notrun)"')

    def test_inherited_credentials_never_reach_a_resource_scheduled_consumer(self):
        spoof = dict(os.environ, NOSTR_SIGNER_ENDPOINT=self.listener.endpoint, NOSTR_SIGNER_TOKEN='f' * 64)
        with services.signer_resource_delivery(self.listener.endpoint, self.token, self.private) as (spec, _):
            with patch.dict(os.environ, spoof):
                self.ctest(spec, 'alias')
        self.assertEqual(self.listener.received, [], 'resource-scheduled alias fell back to inherited env')

    def test_unrelated_resource_requirement_is_starved_by_the_signer_spec(self):
        # Why admission refuses any other selected resource group: under the
        # signer-only spec CTest reports it Not Run instead of executing it.
        self.cmake.write_text(self.cmake.read_text().replace(
            'set_tests_properties(forged-label PROPERTIES', 'set_tests_properties(forged-label PROPERTIES RESOURCE_GROUPS "gpus:1"'))
        self.realized = self.configure()
        with services.signer_resource_delivery(self.listener.endpoint, self.token, self.private) as (spec, _):
            _, junit = self.ctest(spec, 'forged-label')
        self.assertRegex(junit.read_text(), r'name="forged-label"[^>]*status="(notrun|fail)"')
        with self.assertRaisesRegex(services.ServiceError, 'resources the signer spec cannot provide'):
            services.signer_delivery_admission(self.export(self.realized), [CONSUMER, 'forged-label'], {})


class DeliveryAdmission(ScopedDeliveryProject):
    def refuses(self, pattern, selected=(CONSUMER,), environment=None, edit=None):
        if edit:
            self.cmake.write_text(self.cmake.read_text().replace(*edit))
        tests = self.export(self.configure())
        with self.assertRaisesRegex(services.ServiceError, pattern):
            services.signer_delivery_admission(tests, list(selected), environment or {})

    def test_exact_consumer_is_admitted(self):
        tests = self.export(self.configure())
        services.signer_delivery_admission(tests, [CONSUMER, 'alias', *DUMPERS], {'PATH': '/bin'})

    def test_forged_resource_on_unrelated_registration_refuses(self):
        self.refuses('exactly test_nip5f_tcp', edit=('set_tests_properties(forged-label PROPERTIES',
                                                     'set_tests_properties(forged-label PROPERTIES RESOURCE_GROUPS "nip5f_signer:1"'))

    def test_alias_of_the_consumer_binary_cannot_claim_the_resource(self):
        self.refuses('exactly test_nip5f_tcp', edit=('set_tests_properties(alias PROPERTIES SKIP_RETURN_CODE 77)',
                                                     'set_tests_properties(alias PROPERTIES SKIP_RETURN_CODE 77 RESOURCE_GROUPS "nip5f_signer:1")'))

    def test_unselected_holder_still_refuses_over_the_whole_universe(self):
        self.refuses('exactly test_nip5f_tcp', selected=(CONSUMER,),
                     edit=('set_tests_properties(probe-cleanup PROPERTIES', 'set_tests_properties(probe-cleanup PROPERTIES RESOURCE_GROUPS "nip5f_signer:1"'))

    def test_changed_slots_or_missing_route_refuses(self):
        self.refuses('exact nip5f_signer:1', edit=(f'RESOURCE_GROUPS "{GROUPS}"', 'RESOURCE_GROUPS "nip5f_signer:2"'))

    def test_missing_route_refuses(self):
        self.refuses('exactly test_nip5f_tcp', edit=(f'RESOURCE_GROUPS "{GROUPS}"', ''))

    def test_label_or_name_alone_is_not_admission(self):
        tests = self.export(self.configure())
        with self.assertRaisesRegex(services.ServiceError, 'exact nip5f_signer:1'):
            services.signer_delivery_admission(tests, ['forged-label'], {})

    def test_ambient_signer_or_resource_variables_refuse(self):
        tests = self.export(self.configure())
        for key in ('NOSTR_SIGNER_ENDPOINT', 'NOSTR_SIGNER_TOKEN', 'NOSTR_SIGNER_SOCK',
                    'CTEST_RESOURCE_GROUP_0_NIP5F_SIGNER', 'CTEST_RESOURCE_SPEC_FILE'):
            with self.subTest(key=key), self.assertRaisesRegex(services.ServiceError, 'ambient'):
                services.signer_delivery_admission(tests, [CONSUMER], {key: ''})


class DeliveryLifecycle(unittest.TestCase):
    def setUp(self):
        self.root = Path(tempfile.mkdtemp(prefix='scope-'))
        self.addCleanup(shutil.rmtree, self.root, True)
        self.token = secrets.token_hex(32)

    def test_cancellation_removes_the_private_spec(self):
        for error in (KeyboardInterrupt, SystemExit, RuntimeError):
            with self.subTest(error=error.__name__):
                with self.assertRaises(error):
                    with services.signer_resource_delivery('tcp:127.0.0.1:9', self.token, self.root) as (spec, record):
                        self.assertIn(self.token, spec.read_text())
                        raise error()
                self.assertTrue(record['removed'])
                # Scanned on every exit, before any output is retained or uploaded.
                self.assertEqual((record['exposure_checked'], record['exposed']), ([], []))
                self.assertEqual(list(self.root.iterdir()), [])

    def test_exposure_on_failure_or_cancellation_is_recorded_without_masking(self):
        leak = self.root / 'ctest.log'
        for error in (KeyboardInterrupt, SystemExit, RuntimeError):
            with self.subTest(error=error.__name__):
                with self.assertRaises(error):
                    with services.signer_resource_delivery('tcp:127.0.0.1:9', self.token, self.root, (leak,)) as (_, record):
                        leak.write_text('AUTH ' + self.token)
                        raise error()
                self.assertTrue(record['removed'])
                self.assertEqual(record['exposed'], [str(leak)])  # never rewritten, never the token itself
                self.assertNotIn(self.token, json.dumps(record))
                self.assertEqual(list(self.root.iterdir()), [leak])

    def test_fixture_logs_are_scanned_after_reaping(self):
        evidence = self.root / 'signer'
        (evidence / 'worker').mkdir(parents=True)
        (evidence / 'worker/daemon.log').write_text('started\n')
        state = {'status': 'passed', 'delivery': {'exposed': []}}
        services.signer_fixture_exposure(state, self.token, evidence, False)
        self.assertEqual(state['delivery'], {'exposed': [], 'fixture_exposure_checked': [str(evidence)]})
        self.assertEqual(state['status'], 'passed')
        (evidence / 'worker/approver.log').write_text('request AUTH ' + self.token + '\n')
        with self.assertRaisesRegex(services.ServiceError, 'exposed in fixture output'):
            services.signer_fixture_exposure(state, self.token, evidence, False)
        self.assertEqual((state['status'], state['delivery']['exposed']),
                         ('failed', [str(evidence / 'worker/approver.log')]))
        failing = {'status': 'failed', 'delivery': {'exposed': []}}
        services.signer_fixture_exposure(failing, self.token, evidence, True)  # in flight: recorded, not masked
        self.assertEqual(failing['delivery']['exposed'], [str(evidence / 'worker/approver.log')])
        untouched = {'status': 'failed'}
        services.signer_fixture_exposure(untouched, self.token, evidence, True)  # never delivered: nothing claimed
        self.assertEqual(untouched, {'status': 'failed'})

    def test_token_exposed_in_named_output_fails_closed_and_still_cleans_up(self):
        leak = self.root / 'ctest.log'
        with self.assertRaisesRegex(services.ServiceError, 'exposed in lane output'):
            with services.signer_resource_delivery('tcp:127.0.0.1:9', self.token, self.root, (leak,)):
                leak.write_text('AUTH ' + self.token)
        self.assertEqual(list(self.root.iterdir()), [leak])

    def test_only_fixture_loopback_endpoint_and_runtime_token_are_delivered(self):
        for endpoint, token in (('tcp:10.0.0.1:9', self.token), ('unix:/tmp/s', self.token),
                                ('tcp:127.0.0.1:0', self.token), ('tcp:127.0.0.1:65536', self.token),
                                ('tcp:127.0.0.1:9', 'T' * 64), ('tcp:127.0.0.1:9', self.token[:-1])):
            with self.subTest(endpoint=endpoint), self.assertRaises(services.ServiceError):
                with services.signer_resource_delivery(endpoint, token, self.root):
                    pass
        self.assertEqual(list(self.root.iterdir()), [])


class EngineScopedDelivery(ScopedDeliveryProject):
    """The engine's single CTest process carries no credential; commands neither."""

    def test_engine_delivers_only_through_the_consumer_allocation(self):
        realized = self.configure()
        self.build_all()
        resolved = r.resolve_checks(self.inventory, self.plan, realized, 'synthetic')
        self.assertIn('signer_endpoint', resolved['required_capabilities'])
        audit = self.audit(realized)
        self.assertTrue(audit['passed'], audit['gaps'])
        listener, token = Listener(), secrets.token_hex(32)
        self.addCleanup(listener.close)
        provisions, environments = [], []

        @contextlib.contextmanager
        def provision(realization, resolved, input_audit, evidence_dir, *, environment=None, exposure_paths=()):
            provisions.append([str(p) for p in exposure_paths])
            with services.signer_resource_delivery(listener.endpoint, token, self.private, exposure_paths) as (spec, delivery):
                yield spec, {'kind': 'delivery_probe_not_signer', 'delivery': delivery}

        execute = r._execute

        def recording(argv, cwd=None, env=None, log_path=None, timeout=None):
            environments.append((list(argv), dict(env or {})))
            return execute(argv, cwd=cwd, env=env, log_path=log_path, timeout=timeout)

        with patch.object(services, 'provisioned_signer', provision), patch.object(r, '_execute', recording):
            receipt = r.run_resolved(resolved, self.build, self.evidence, input_audit=audit, build=False)
        self.assertEqual(len(provisions), 1)
        last = str(Path(realized['build_dir']) / 'Testing/Temporary/LastTest.log')
        lane = Path(receipt['evidence_dir'])
        self.assertEqual(provisions[0], [str(lane / 'ctest.log'), str(lane / 'results.xml'), last])
        delivery = receipt['signer_runtime']['delivery']
        self.assertEqual((delivery['exposure_checked'], delivery['exposed']), (sorted(provisions[0]), []))
        self.assertTrue(Path(last).is_file())  # the real CTest wrote it and it was scanned
        ctest = [argv for argv in receipt['commands'] if argv[0] == 'ctest']
        self.assertEqual(len(ctest), 1, 'single CTest invocation')
        spec = receipt['signer_runtime']['delivery']['resource_spec_file']
        self.assertEqual(ctest[0][-2:], ['--resource-spec-file', spec])
        self.assertFalse(Path(spec).exists())
        self.assertEqual(receipt['ctest_environment']['overlay_names'], [])
        self.assertEqual(receipt['ctest_environment']['effective_hash'], receipt['ctest_environment']['base_hash'])
        self.assertEqual(listener.received, [b'AUTH ' + token.encode() + b'\n'])
        executing = [argv for argv, _ in environments if argv[0] == 'ctest' and '--output-junit' in argv]
        self.assertEqual(executing, ctest, 'single executing CTest process')  # others: --show-only queries
        self.assertEqual(sum(argv[-1] == f'{self.out}/command.env' for argv, _ in environments), 1, 'command ran')
        for argv, env in environments:
            self.assertFalse(any(token in v for v in env.values()), argv)
            self.assertFalse(any(k.startswith(('NOSTR_SIGNER_', 'CTEST_RESOURCE_')) for k in env), argv)
        for name in (*DUMPERS, 'command'):
            self.assertNotIn(token, (self.out / (name + '.env')).read_text(), name)
        self.assertEqual(receipt['observed_executions'][CONSUMER], 'failed')  # never fake signer success
        self.assertNotEqual(receipt['outcome'], 'passed')
        for path in self.evidence.rglob('*'):
            if path.is_file():
                self.assertNotIn(token.encode(), path.read_bytes(), str(path))

    def test_ambient_credentials_refuse_before_provisioning(self):
        for key in ('NOSTR_SIGNER_TOKEN', 'CTEST_RESOURCE_GROUP_0_NIP5F_SIGNER'):
            with self.subTest(key=key), patch.dict(os.environ, {key: 'f' * 64}), \
                    patch.object(services, 'provisioned_signer') as provision:
                shutil.rmtree(self.build, ignore_errors=True)
                realized = self.configure()  # bound into the phase identity, as on a real worker
                self.build_all()
                resolved = r.resolve_checks(self.inventory, self.plan, realized, 'synthetic')
                audit = self.audit(realized)
                with self.assertRaisesRegex(r.RealizationError, 'ambient'):
                    r.run_resolved(resolved, self.build, self.evidence / key, input_audit=audit, build=False)
                provision.assert_not_called()

    def test_forged_registration_resource_refuses_at_resolution(self):
        self.cmake.write_text(self.cmake.read_text().replace(
            'set_tests_properties(forged-label PROPERTIES', 'set_tests_properties(forged-label PROPERTIES RESOURCE_GROUPS "nip5f_signer:1"'))
        realized = self.configure()
        with self.assertRaisesRegex(r.RealizationError, 'exactly test_nip5f_tcp'):
            r.resolve_checks(self.inventory, self.plan, realized, 'synthetic')


if __name__ == '__main__':
    unittest.main()
