"""Finite workflow union regressions; synthetic results are not hosted acceptance."""
import copy
import os
import json
import subprocess
import sys
import tempfile
from pathlib import Path
import re
import unittest
from unittest.mock import patch

from selector_support import ROOT
from profile_contract_support import source_job_blocks
import test_workflow_contracts as fixtures

ci = fixtures.ci


class BroadReceiptTests(unittest.TestCase):
    def setUp(self):
        self.env = patch.dict(os.environ, GITHUB_RUN_ID='42', GITHUB_RUN_ATTEMPT='1', GITHUB_WORKFLOW_SHA='a' * 40)
        self.env.start()
        self.addCleanup(self.env.stop)
        self.sha = 'a' * 40
        self.plan = {'effective_mode': 'broad', 'tested_sha': self.sha}
        self.needs = {job: {'result': 'success'} for job in ci.BROAD_FAMILIES}

    def test_successful_union_then_independent_identity_and_conclusion_refusals(self):
        baseline = ci.broad_aggregate(ROOT, self.sha, self.needs)
        self.assertTrue(baseline['passed'])
        self.assertEqual(ci.broad_receipt_errors(ROOT, self.plan, baseline), [])
        self.assertEqual(baseline['scope'], 'family_conclusions_not_registration_coverage')
        for key, value in [('run_id', 'other'), ('run_attempt', '2'), ('source_identity', {'kind': 'committed', 'sha': 'b' * 40}),
                           ('workflow_sha256', {}), ('workflow_sha', 'b' * 40), ('passed', False), ('kind', 'other')]:
            bad = dict(baseline, **{key: value})
            with self.subTest(key=key):
                self.assertTrue(ci.broad_receipt_errors(ROOT, self.plan, bad))
        for key in baseline:
            bad = dict(baseline)
            del bad[key]
            with self.subTest(missing=key):
                self.assertTrue(ci.broad_receipt_errors(ROOT, self.plan, bad))
        self.assertTrue(ci.broad_receipt_errors(ROOT, self.plan, None))
        self.assertTrue(ci.broad_receipt_errors(ROOT, dict(self.plan, effective_mode='affected'), baseline))
        self.assertEqual(ci.broad_receipt_errors(ROOT, self.plan, baseline), [])

    def test_every_family_missing_cancelled_skipped_and_failed_remains_negative(self):
        for job in ci.BROAD_FAMILIES:
            for result in ('cancelled', 'skipped', 'failure', None):
                needs = copy.deepcopy(self.needs)
                needs[job]['result'] = result
                with self.subTest(job=job, result=result):
                    receipt = ci.broad_aggregate(ROOT, self.sha, needs)
                    self.assertFalse(receipt['passed'])
                    self.assertTrue(ci.broad_receipt_errors(ROOT, self.plan, receipt))
            needs = dict(self.needs)
            del needs[job]
            self.assertFalse(ci.broad_aggregate(ROOT, self.sha, needs)['passed'])
        self.assertFalse(ci.broad_aggregate(ROOT, self.sha, dict(self.needs, unknown={'result': 'success'}))['passed'])

    def test_real_broad_cli_retains_negative_receipt_before_nonzero_exit(self):
        sha = fixtures.s.commit_sha(ROOT, 'HEAD')
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp)
            needs_path = path / 'needs.json'
            for conclusion in ('success', 'failure', 'cancelled', 'skipped'):
                needs = copy.deepcopy(self.needs)
                needs['homed']['result'] = conclusion
                needs_path.write_text(json.dumps(needs))
                result = subprocess.run([sys.executable, str(ROOT / '.github/workflows/dependency_contract.py'),
                    'broad-aggregate', '--sha', sha, '--needs', str(needs_path), '--directory', tmp],
                    env=dict(os.environ, GITHUB_WORKFLOW_SHA=sha), capture_output=True, text=True)
                with self.subTest(conclusion=conclusion):
                    self.assertEqual(result.returncode, 0 if conclusion == 'success' else 1, result.stderr)
                    retained = json.loads((path / 'broad.json').read_text())
                    self.assertEqual(retained['source_identity'], {'kind': 'committed', 'sha': sha})
                    self.assertEqual(retained['family_conclusions']['homed'], conclusion)
                    self.assertEqual(retained['passed'], conclusion == 'success')

    def test_gate_requires_receipt_even_when_family_job_is_green(self):
        p = fixtures.ready_policy()
        plan = fixtures.s.make_plan(p, [], head=self.sha, context='ci', mode='broad')
        needs = {job: {'result': 'skipped'} for job in ci.LANES.values()}
        needs.update(select={'result': 'success'}, core={'result': 'success'}, broad={'result': 'success'})
        lane = fixtures.receipt(p, plan)
        broad = ci.broad_aggregate(ROOT, self.sha, self.needs)
        self.assertTrue(ci.aggregate(ROOT, p, plan, needs, [lane], broad)['passed'])
        missing = ci.aggregate(ROOT, p, plan, needs, [lane])
        self.assertFalse(missing['passed'])
        self.assertFalse(missing['complete_gate_eligible'])
        self.assertIn('required same-run broad family receipt is missing', missing['errors'])
        lane['complete'] = False
        lane['complete_gate_eligible'] = False
        self.assertFalse(ci.aggregate(ROOT, p, plan, needs, [lane], broad)['passed'])


class OrdinaryToolSetupTests(unittest.TestCase):
    def test_real_executable_versions_and_hashes_not_package_presence_or_bus_assumptions(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            for executable, version in [('nak', 'nak version v0.19.7'), ('gnome-keyring-daemon', 'gnome-keyring-daemon 46.1')]:
                path = root / executable
                path.write_text('#!/bin/sh\nprintf "%s\\n" "' + version + '"\n')
                path.chmod(0o755)
            sha = 'a' * 40
            with patch.dict(os.environ, PATH=str(root), PROFILE='groundhog.linux', PLAN_ID='plan',
                            GITHUB_RUN_ID='42', GITHUB_RUN_ATTEMPT='1', GITHUB_WORKFLOW_SHA=sha):
                record = ci.ordinary_tool_setup(sha)
                self.assertTrue(record['setup_verified'])
                self.assertEqual(set(record['tools']), {'nak', 'gnome_keyring'})
                self.assertEqual(record['scope'], 'executables_only_not_bus_display_or_profile_acceptance')
                for tool in record['tools'].values():
                    self.assertEqual(len(tool['sha256']), 64)
                    self.assertEqual(Path(tool['path']).parent, root.resolve())
                with patch.dict(os.environ, PROFILE='grotto.linux'):
                    self.assertTrue(ci.ordinary_tool_setup(sha)['setup_verified'])
                with patch.dict(os.environ, PROFILE='grotto.linux.asan_ubsan'):
                    with self.assertRaisesRegex(fixtures.s.ContractError, 'not this profile'):
                        ci.ordinary_tool_setup(sha)
                (root / 'nak').write_text('#!/bin/sh\necho "nak version v0.19.8"\n')
                with self.assertRaisesRegex(fixtures.s.ContractError, 'does not match pinned'):
                    ci.ordinary_tool_setup(sha)
                (root / 'nak').write_text('#!/bin/sh\necho "nak version v0.19.7"\n')
                (root / 'gnome-keyring-daemon').unlink()
                with self.assertRaisesRegex(fixtures.s.ContractError, 'unavailable: gnome-keyring-daemon'):
                    ci.ordinary_tool_setup(sha)
                (root / 'gnome-keyring-daemon').write_text('#!/bin/sh\nexit 3\n')
                (root / 'gnome-keyring-daemon').chmod(0o755)
                with self.assertRaisesRegex(fixtures.s.ContractError, 'version failed'):
                    ci.ordinary_tool_setup(sha)

    def test_install_and_verification_are_ordinary_only_and_precede_execution(self):
        text = fixtures.workflow('dependency-worker')
        for step in ('Go toolchain for ordinary runtime peers', 'Install pinned nak for ordinary runtime peers',
                     'Verify ordinary peer and keyring executable setup'):
            block = text.split('- name: ' + step, 1)[1].split('- name:', 1)[0]
            self.assertIn("if: runner.os == 'Linux' && (inputs.profile == 'groundhog.linux' || inputs.profile == 'grotto.linux')", block)
            self.assertLess(text.index('- name: ' + step), text.index('- name: Resolve, audit, build and execute exact selected checks'))
            self.assertNotIn('continue-on-error', block)
            self.assertNotIn('|| true', block)
        self.assertIn('go install github.com/fiatjaf/nak@v0.19.7', text)
        self.assertIn('dependency_contract.py ordinary-tools --sha "$REQUESTED_SHA"', text)
        self.assertIn('${{ runner.temp }}/dependency/ordinary-tool-setup.json', text)
        setup = text.split('- name: Install Linux profile prerequisites', 1)[1].split('- name:', 1)[0]
        self.assertIn('gnome-keyring', setup)


class BroadUnionWiringTests(unittest.TestCase):
    def test_exact_reusable_family_and_job_union(self):
        expected = {
            'core': ('nostrc-ci', {'build-and-test'}),
            'libgo': ('libgo-ci', {'build-and-test', 'gofiber-linux-sanitizers', 'fuzz-driver'}),
            'hanami': ('libhanami-ci', {'build-and-test'}),
            'gi': ('libnostr-gi', {'generate-gir'}),
            'groundhog': ('groundhog-ci', {'groundhog', 'groundhog-sanitizers'}),
            'grotto': ('grotto-ci', {'signer'}),
            'nip55l': ('nip55l-libsecret-ci', {'refusing-session'}),
            'homed': ('nostr-homed-ci', {'build-and-test', 'systemd-opensession'}),
            'signet': ('signet-ci', {'build-and-test'}),
            'static': ('static-checks', {'static-checks'}),
            'appimage': ('gnostr-appimage', {'build-appimage'}),
            'windows': ('windows-validation', {'build'}),
            'bp': ('bp-matrix', {'bp-matrix'}),
            'bp_extended': ('bp-matrix-nightly', {'nightly-matrix'}),
            'fuzz': ('grotto-fuzz', {'select-fuzz', 'fuzz-test', 'coverage-report'}),
            'mdk08': ('marmot-mdk-interop', {'mdk-interop'}),
            'mdk011': ('marmot-mdk011-interop', {'mdk011-adopted-matrix'}),
        }
        self.assertEqual(ci.BROAD_FAMILIES, {job: family for job, (family, _) in expected.items()})
        blocks = source_job_blocks(ROOT)
        broad = fixtures.workflow('dependency-broad')
        self.assertEqual({key.split(':')[1] for key in blocks if key.startswith('.github/workflows/dependency-broad.yml:')},
                         set(expected) | {'broad_gate'})
        for job, (family, children) in expected.items():
            with self.subTest(family=family):
                prefix = f'.github/workflows/{family}.yml:'
                self.assertEqual({key[len(prefix):] for key in blocks if key.startswith(prefix)}, children)
                call = fixtures.job_block(broad, job)
                self.assertIn(f'uses: ./.github/workflows/{family}.yml', call)
                self.assertIn('sha: ${{ inputs.sha }}', call)
                self.assertIn('artifact_prefix: validation-', call)
                self.assertNotIn('    if:', call)
                self.assertNotIn('continue-on-error', call)
        gate = fixtures.job_block(broad, 'broad_gate')
        needs = re.search(r'needs: \[(.*?)\]', gate).group(1)
        self.assertEqual({x.strip() for x in needs.split(',')}, set(expected))
        self.assertIn('if: always()', gate)
        self.assertIn('dependency_contract.py broad-aggregate', gate)
        self.assertIn('dependency-broad-${{ inputs.sha }}-${{ github.run_attempt }}', gate)
        self.assertIn('if-no-files-found: error', gate)

    def test_broad_homed_call_cannot_inherit_pr_skip_or_widen_legacy_entrypoint(self):
        job = fixtures.job_block(fixtures.workflow('nostr-homed-ci'), 'systemd-opensession')
        self.assertIn("if: inputs.sha != '' || github.event_name != 'pull_request' || startsWith(github.ref, 'refs/heads/')", job)
        self.assertIn('runs-on: ubuntu-24.04', job)
        self.assertIn('continue-on-error: true', job)
        self.assertNotIn('secrets:', job)
        self.assertNotIn('self-hosted', job)

    def test_static_membership_regressions_have_pinned_submodule_sources(self):
        text = fixtures.workflow('static-checks')
        checkout = text.split('- name: Checkout', 1)[1].split('- name: Verify requested checkout', 1)[0]
        self.assertIn('submodules: recursive', checkout)
        self.assertIn('ref: ${{ inputs.sha || github.sha }}', checkout)
        self.assertIn('persist-credentials: false', checkout)
        self.assertLess(text.index('submodules: recursive'), text.index('python3 -m unittest discover -s tests/selection'))
        self.assertNotIn('git submodule update --remote', text)

    def test_aggregate_runs_after_download_failure_and_uses_only_same_run_broad_receipt(self):
        gate = fixtures.job_block(fixtures.workflow('dependency-tests'), 'gate')
        self.assertIn('name: dependency-broad-${{ needs.select.outputs.sha }}-${{ github.run_attempt }}', gate)
        account = gate.split('- name: Account for every expected lane and actual worker conclusion', 1)[1]
        self.assertIn("if: always() && needs.select.outputs.run == 'true'", account)
        self.assertNotIn('run-id:', gate)
        self.assertNotIn('github-token:', gate)
        self.assertNotIn('continue-on-error', gate)
