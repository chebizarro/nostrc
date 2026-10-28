"""Current fixture input ownership, distinct from complete-profile admission."""
import copy
import hashlib
import unittest

from selector_support import ROOT, s
from realization_support import r
import w11_worker_support as w
from w11_policy_support import before_policy_enrollment, before_policy_source


class WorkerEnrollmentTests(unittest.TestCase):
    def test_worker_calls_public_custody_route_without_replanning_or_wrapper_inference(self):
        text = (ROOT / '.github/workflows/dependency-worker.yml').read_text()
        step = text.split('      - name: Resolve, audit, build and execute exact selected checks', 1)[1]
        route = step.split('          parallel_args=', 1)[0]
        self.assertIn('core.linux|grotto.linux|groundhog.linux)', route)
        self.assertIn('bash scripts/test-gate.sh --head "$REQUESTED_SHA"', route)
        self.assertIn('--plan "$RUNNER_TEMP/dependency/plan.json" --profile "$PROFILE"', route)
        self.assertIn('--native-custody pidns', route)
        self.assertIn('verify_native_public_receipt(raw)', route)
        self.assertIn('exit "$gate_status"', route)
        self.assertNotIn('xvfb-run', route)

    def test_current_authored_fixture_profiles_are_not_boolean_only(self):
        p = s.load_inventory(ROOT / 'tests/selection/inventory.json')
        for ident in ('core.linux', 'groundhog.linux', 'grotto.linux'):
            v = p['profiles'][ident]
            for flag in ('-DENABLE_TCP_IPC=ON', '-DNIP55L_TEST_TRUST_ENV=ON',
                         '-DNIP55L_SECRET_BACKEND=libsecret', '-DBUILD_APPS=ON',
                         '-DGROUNDHOG_STORE_KEY_DEFAULT_BACKEND=secret-service'):
                self.assertEqual(v['configure_args'].count(flag), 1)
            self.assertEqual(v['configure_args'].count('-DBUILD_GNOSTR_APP=' + ('ON' if ident == 'core.linux' else 'OFF')), 1)
            self.assertIn('signer_endpoint', v['capabilities'])
            self.assertEqual(v['environment']['NOSTRC_TEST_NIP5F_APPROVAL'],
                             'unidentified-get-public-key-once')
            self.assertEqual(r._signer_diagnostic_boundary({'profile': v}),
                             {'complete': False, 'complete_gate_eligible': False})

    def test_current_grotto_input_selects_real_signer_fixture_without_rewriting_goldens(self):
        from selector_support import change
        p = s.load_inventory(ROOT / 'tests/selection/inventory.json')
        path = change('apps/grotto/src/main.c')
        current = s.make_plan(p, [path], head='a' * 40, base='b' * 40)
        self.assertIn('supported.nips.nip5f.signer_endpoint', current['selected_checks'])
        self.assertFalse(current['enforcement_ready'])


    def test_live_sources_and_semantic_prerequisites_are_owned(self):
        p = s.load_inventory(ROOT / 'tests/selection/inventory.json')
        for path in w.witness()['source_sha256']:
            self.assertTrue((ROOT / path).is_file(), path)
            self.assertIn('{source}/' + path,
                          p['checks']['selection.contract']['runtime_inputs'])
        check = p['checks']['supported.nips.nip5f.signer_endpoint']
        self.assertEqual(set(check['build_targets']),
                         {'test_nip5f_tcp', 'test_nip5f_approver', 'grotto-daemon', 'grotto-schemas'})
        covered = r._impact_prerequisites(p, check['nodes'])
        for path in ('tests/runtime/test_provisioned_nip5f.py', 'tests/runtime/nip5f_approver.c',
                     'gnome/groundhog/tests/app/test_secret_service.py',
                     'apps/grotto/data/org.nostr.Grotto.gschema.xml', 'apps/grotto/daemon/main_daemon.c',
                     'gnome/seahorse/secret_store.c', 'gnome/seahorse/secret_store.h',
                     'apps/relayd/include/relayd_async_storage.h', 'nips/nip55l/dbus/org.nostr.Signer.xml'):
            self.assertTrue(set(s.classify(p, path)['nodes']) & covered, path)
        self.assertEqual(p['nodes']['nip5f.fixture_inputs']['depends_on'],
                         ['gnome.seahorse', 'relayd.async_storage_api', 'nip55l.dbus_contract'])
        self.assertIn('nostrc_add_test_owners("test_nip5f_tcp" OWNER nip5f.test_inputs nip5f.fixture_inputs)',
                      (ROOT / 'nips/nip5f/TestInventory.cmake').read_text())


if __name__ == '__main__':
    unittest.main()
