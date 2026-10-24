"""Independent post-exit qualification of authored full-profile executions.

Raw custody/signer/MDK receipts retain their diagnostic flags. This contract
rechecks physical facts and references those original sealed bytes; it never
changes flags, grants rollout authority, or admits an omission witness.

Portable transport (nostrc-0cfsg.36.8): the production worker qualifies in
place only after the outer pidns supervisor exited and the per-job AppArmor
profile was torn down, packs the exact retained bytes into one bounded framed
archive, re-qualifies that archive from a fresh relocated extraction and only
then atomically replaces its lane receipt. The independent aggregate resolves
every original (logical) path through a map derived from the admitted raw
sealed evidence into its own private extraction; it never opens worker paths,
never falls back to them, and never rewrites a path string. Retained source
claims are checked against the aggregate's own exact checkout (submodules
included). Trust is limited to the same workflow run's artifacts, plan and
checkout: the archive digest is integrity, not external authenticity.
"""
import contextlib
import hashlib
import json
import os
import re
import shutil
import stat
import subprocess
import sys
import tempfile
from pathlib import Path, PurePosixPath

KIND = 'normal_execution_v1'
FIELD = 'normal_execution'
MARKER = dict(schema_version=1, custody='linux_pidns_v1',
              audit_scope='full_configured_universe', registration_policy='all_owned_and_executed')
# Separately authored selective contract (nostrc-0cfsg.36.11). It never changes
# MARKER: only a profile that already authors the full contract may add it, and
# it applies only to fresh, unrestricted, unbroadened affected plans whose exact
# selection this qualifier re-derives from the same tested source history.
SELECTED_FIELD = 'normal_selected_execution'
SELECTED_MARKER = dict(schema_version=1, custody='linux_pidns_v1', audit_scope='full_configured_universe',
                       registration_policy='all_owned_selected_closure_executed',
                       selection='source_bound_dependency_plan')
SELECTED_SCOPE = 'selected_dependency_closure'


APPARMOR_FILES = ('setup.json', 'teardown.json', 'profiles-before.txt', 'profiles-after.txt')


def require(value, message):
    if not value:
        raise ValueError('normal execution: ' + message)


def validate_metadata(policy):
    for pid, profile in policy.get('profiles', {}).items():
        if SELECTED_FIELD in profile:
            require(profile[SELECTED_FIELD] == SELECTED_MARKER and type(profile[SELECTED_FIELD].get('schema_version')) is int
                    and profile.get(FIELD) == MARKER, 'selected-normal contract requires the authored full contract: ' + pid)
        if FIELD not in profile:
            continue
        require(profile[FIELD] == MARKER and type(profile[FIELD].get('schema_version')) is int,
                'invalid authored contract: ' + pid)
        require(profile.get('platform') == 'linux' and profile.get('architecture') == 'x86_64'
                and profile.get('execution_domain') not in ('prepared_peer_linux_v1', 'policy_controller_linux_v1'),
                'unsupported domain/architecture: ' + pid)
        require(not policy.get('diagnostic_nonmembers', {}).get(pid), 'historical partition cannot become normal')


def _omission(value):
    from test_selection import NONMEMBER_AUTHORITY
    import test_selection_subset as subset
    if isinstance(value, dict):
        return (value.get('authority') == NONMEMBER_AUTHORITY or subset.marked(value)
                or bool(value.get('baseline_exclusions')) or value.get('receipt_kind') == 'lane_failure_v1'
                or any(_omission(item) for item in value.values()))
    return isinstance(value, list) and any(_omission(item) for item in value)


class Locator:
    """Original logical paths -> physical files. ``roots=None`` is in place."""
    def __init__(self, roots=None):
        self.roots = roots
        self.seal = self.real = self.result = None

    def path(self, logical):
        logical = str(logical)
        require(os.path.isabs(logical) and os.path.normpath(logical) == logical,
                'non-canonical logical evidence path: ' + logical)
        if self.roots is None:
            return Path(logical)
        hits = [root for root in self.roots if logical == root or logical.startswith(root + os.sep)]
        require(len(hits) == 1, 'evidence path outside the transported roots: ' + logical)
        return self.roots[hits[0]].joinpath(*Path(logical).relative_to(hits[0]).parts)


def _file(locator, logical):
    path = (locator or Locator()).path(logical)
    require(path.is_absolute() and path.resolve(strict=True) == path and path.is_file() and not path.is_symlink(),
            'unsafe/missing retained file: ' + str(logical))
    return path


def _json(locator, logical):
    return json.loads(_file(locator, logical).read_text())


def _sha(path):
    digest = hashlib.sha256()
    with open(path, 'rb') as stream:
        for block in iter(lambda: stream.read(1 << 20), b''):
            digest.update(block)
    return digest.hexdigest()


def retained(path, locator=None):
    return dict(path=str(path), sha256=_sha(_file(locator, path)))


def _verify_native(public, locator):
    """verify_native_public_receipt with the logical/physical roots kept apart."""
    from test_selection_custody import attest_retained_evidence
    from test_selection_services import NATIVE_ROOTS, _native_files
    final = public.get('native_custody_final')
    require(isinstance(final, dict) and set(final) == {'kind', 'evidence_root', 'seal_sha256'}
            and final.get('kind') == 'native_kernel_custody_v1', 'native kernel custody final seal missing or malformed')
    work = Path(final['evidence_root'])
    physical = locator.path(work)
    require(physical.is_absolute() and physical.resolve(strict=True) == physical and physical.is_dir(),
            'native evidence root is not canonical')
    original = {key: value for key, value in public.items() if key != 'native_custody_final'}
    backend = original.get('backend_receipt', original)
    require(Path(backend.get('build_dir', '')).parent == work and _json(locator, work / 'receipt.json') == original,
            'native public receipt differs from original execution binding')
    expected = final['seal_sha256']
    seal_path = _file(locator, work / 'native-seal.json')
    require(isinstance(expected, str) and re.fullmatch(r'[0-9a-f]{64}', expected) and _sha(seal_path) == expected,
            'native finalizer seal bytes changed')
    seal = json.loads(seal_path.read_text())
    require(_native_files(physical) == seal['files'], 'native top-level retained evidence changed after seal')
    locator.result = _json(locator, work / 'receipt.json')
    verification = attest_retained_evidence(physical, locator.result, seal, roots=NATIVE_ROOTS, claim_root=work)
    for name, value in seal['source_claims'].items():
        kept = physical / 'source-inputs' / (hashlib.sha256(name.encode()).hexdigest() + '.input')
        require(not kept.is_symlink() and kept.is_file() and _sha(kept) == value,
                'native retained source input changed after finalizer seal')
    require(verification['status'] == 'verified', 'native retained evidence changed after finalizer seal')
    observed = {'schema_version': 1, 'status': 'verified', 'seal_sha256': expected,
                'retained_evidence': verification, 'complete': False, 'complete_gate_eligible': False}
    require(_json(locator, work / 'native-final.json') == observed,
            'native final verification differs from retained evidence')
    locator.seal = seal
    return seal


def validate_mdk_graph(realization, proof):
    """Only the original source-authored image fixtures/peer argv use Docker."""
    import re
    import shlex
    from test_selection_realization import _properties
    from test_selection_mdk import IMAGE_TESTS, WRAPPER
    root, build = Path(realization['repo_root']), Path(realization['build_dir'])
    tests = {item['name']: item for item in realization['tests']}
    fixtures = {'08': 'groundhog-mdk-image', '011': 'groundhog-mdk011-image', '09': 'groundhog-mdk09-image'}
    cases = {'control', 'groundhog-invites-mdk', 'mdk-invites-groundhog', 'mdk-invites-groundhog-gui',
             'adopted-welcome', 'white-noise-welcome', 'white-noise-media', 'media-policy', 'white-noise-voice',
             'white-noise-dm', 'adopted-commits', 'routing-rotation', 'concurrent-commits', 'polls', 'white-noise-reactions'}
    consumers = {}
    if '08' in proof['versions']:
        consumers['groundhog-mdk-interop'] = '08'
    if '011' in proof['versions']:
        consumers.update({'groundhog-mdk011-interop-' + name: '011' for name in cases})
    if '09' in proof['versions']:
        consumers['groundhog-mdk011-interop-mdk09-probe'] = '09'
    admitted = set(consumers) | {IMAGE_TESTS[version] for version in proof['versions']}
    descriptors = {item['name']: item for item in realization['descriptors']}
    require(all(name in admitted for name, desc in descriptors.items() if 'docker' in desc.get('requires', [])),
            'unknown Docker registration; new source contract required')
    for version, data in proof['versions'].items():
        image = tests.get(IMAGE_TESTS[version], {})
        argv = image.get('command', [])
        require(len(argv) == 5 and Path(argv[0]).resolve() == root / WRAPPER
                and argv[1:4] == ['build', '-t', data['tag']] and Path(argv[4]).resolve() == Path(data['context'])
                and _properties(image).get('FIXTURES_SETUP') == [fixtures[version]],
                'MDK image producer argv/fixture changed: ' + version)
    for name, version in consumers.items():
        test = tests.get(name, {})
        props = _properties(test)
        command = test.get('command', [])
        target = 'test-groundhog-mdk-interop' if version == '08' else 'test-groundhog-mdk011-interop'
        artifacts = [artifact for item in realization['targets'].values() if item['name'] == target
                     for artifact in item['artifacts']]
        case = name.removeprefix('groundhog-mdk011-interop-')
        arguments = ([] if version == '08' else ['--gui', '-p', '/groundhog/mdk011-interop-gui/mdk-invites-groundhog']
                     if case == 'mdk-invites-groundhog-gui' else ['-p', '/groundhog/mdk011-interop/' + case])
        require(command and command[0] in artifacts and command[1:] == arguments,
                'MDK consumer executable/case changed: ' + name)
        environment = props.get('ENVIRONMENT', [])
        key = 'GH_MDK09_DRIVER=' if version == '09' else 'GH_MDK_DRIVER='
        drivers = [value[len(key):] for value in environment if value.startswith(key)]
        require(len(drivers) == 1, 'MDK driver environment missing/duplicated: ' + name)
        argv = shlex.split(drivers[0])
        expected = [str(root / WRAPPER), 'run', '--rm', '-i', '-e', 'MDK_DRIVER_LOG', '-e', 'MDK_DRIVER_DEADLINE_S']
        if version == '011':
            expected += ['-e', 'MDK_DRIVER_ARTIFACT_DIR=/artifacts', '-v', str(build / 'mdk011-interop-artifacts') + ':/artifacts', '--user']
            require(len(argv) > len(expected) and re.fullmatch(r'[0-9]+:[0-9]+', argv[len(expected)]),
                    'MDK peer uid/gid binding missing')
            expected.append(argv[len(expected)])
        expected += ['--network', 'host', proof['versions'][version]['tag']]
        require(argv == expected and fixtures[version] in props.get('FIXTURES_REQUIRED', []),
                'MDK peer argv/image fixture changed: ' + name)


def _expected_check_outcome(check, result):
    expected = check.get('expected_outcome', 'pass')
    require((expected == 'pass' and result.get('outcome') == 'passed')
            or (expected == dict(case='mdk09_incompatibility', exit_code=77)
                and result.get('outcome') == 'expected_incompatibility'
                and result.get('exception_case') == expected['case'] and result.get('exit_code') == expected['exit_code']),
            'execution differs from authored expected outcome')


def _sanitizer_environment(profile, source_root):
    from test_selection_realization import phase_environment
    from test_selection_services import SANITIZER_ENVIRONMENT
    authored = phase_environment(profile, source_root, 'test', base_environment={})
    return {key: value for key, value in authored.items() if key in SANITIZER_ENVIRONMENT}


def signer_consumer_junit(data):
    """The consumer's own sealed JUnit case, independent of backend outcome fields.

    Exactly one real run with no failure, error or skip: an internal exit 77
    (CTest skip), not-run or missing case never stands for signer coverage."""
    import xml.etree.ElementTree as ET
    from test_selection_services import SIGNER_CONSUMER
    try:
        cases = [case for case in ET.fromstring(data).iter('testcase') if case.get('name') == SIGNER_CONSUMER]
    except ET.ParseError:
        cases = []
    require(len(cases) == 1 and cases[0].get('status') == 'run'
            and all(cases[0].find(tag) is None for tag in ('failure', 'error', 'skipped')),
            'signer consumer did not actually execute and pass')


def _signer(backend, profile, post, source_root, tests, closure, locator):
    from test_selection_services import SIGNER_CONSENT, SIGNER_EXECUTABLES
    proof = backend.get('runtime_capability_evidence', {}).get('signer_endpoint')
    state = backend.get('signer_runtime')
    if 'signer_endpoint' not in backend.get('required_capabilities', []):
        require(state is None, 'unselected signer runtime')
        return
    require(isinstance(proof, dict) and proof.get('kind') == 'nip5f_one_shot_provision_contract_v1'
            and proof.get('instrumentation') == profile['instrumentation']
            and proof.get('consent') == profile['environment'].get('NOSTRC_TEST_NIP5F_APPROVAL') == SIGNER_CONSENT
            and proof.get('operation') == 'get_public_key' and proof.get('principal') == 'unidentified'
            and proof.get('remember') is False and proof.get('source_identity') == backend['source_identity'],
            'signer source/variant/consent contract missing')
    require(proof.get('sanitizer_environment') == _sanitizer_environment(profile, source_root),
            'signer sanitizer environment differs')
    require(isinstance(state, dict) and state.get('kind') == 'nip5f_one_shot_runtime_v1'
            and state.get('status') == 'passed' and state.get('contract') == proof
            and state.get('approval_snapshot') == '(uint32 1, uint32 1, uint32 0)', 'signer execution/consent not proved')
    require(all(post['input_hashes'].get(path) == value for path, value in proof['inputs'].items())
            and all(post['input_hashes'].get(tool['path']) == tool['sha256']
                    for tool in proof['executables'].values()), 'signer full source/tool audit missing')
    instruments = state.get('instrumented_executables', {})
    require(set(instruments) == {str(Path(backend['build_dir']) / name) for name in SIGNER_EXECUTABLES}
            and all(item.get('asan') is (profile['instrumentation'] == 'asan_ubsan')
                    and post['input_hashes'].get(path) == item.get('sha256')
                    for path, item in instruments.items()), 'signer executable/instrumentation audit missing')
    from test_selection_services import (ServiceError, SIGNER_CONSUMER, SIGNER_DELIVERY, SIGNER_RESOURCE_TYPE,
                                         signer_delivery_admission)
    delivery, evidence = state.get('delivery', {}), Path(backend['evidence_dir'])
    spec = Path(str(delivery.get('resource_spec_file')))
    require(delivery.get('mechanism') == SIGNER_DELIVERY and delivery.get('resource_type') == SIGNER_RESOURCE_TYPE
            and delivery.get('slots') == 1 and delivery.get('registrations') == [SIGNER_CONSUMER]
            and delivery.get('endpoint') == state.get('readiness', {}).get('endpoint')
            and delivery.get('ctest_environment_credentials') is False and delivery.get('removed') is True
            and spec.is_absolute() and not spec.is_relative_to(evidence)
            and delivery.get('exposure_checked') == sorted([str(evidence / 'ctest.log'), str(evidence / 'results.xml'),
                                                            str(Path(backend['build_dir']) / 'Testing/Temporary/LastTest.log')])
            and delivery.get('fixture_exposure_checked') == [str(evidence / 'signer')]
            and delivery.get('exposed') == [],
            'signer credential delivery is not the removed registration-scoped resource')
    try:
        signer_delivery_admission({test['name']: test for test in tests}, closure, {})
    except ServiceError as exc:
        require(False, str(exc))
    signer_consumer_junit(_file(locator, evidence / 'results.xml').read_bytes())
    cleanup = state.get('cleanup', {})
    require(cleanup.get('private_namespace_absent') is True and cleanup.get('cleanup_errors') == []
            and cleanup.get('remember') is False and cleanup.get('stored_via') == 'StoreKey/libsecret',
            'signer private custody/cleanup missing')
    attestation = cleanup.get('caller_attestation', [])
    callers = dict(bus=proof['executables']['dbus-daemon']['path'],
                   daemon=str(Path(backend['build_dir']) / SIGNER_EXECUTABLES[0]),
                   approver=str(Path(backend['build_dir']) / SIGNER_EXECUTABLES[2]))
    require({item.get('process') for item in attestation} == set(callers)
            and len(attestation) == 3 and all(item.get('same_inode') is True
            and item.get('observed') == item.get('expected') == callers[item['process']]
            for item in attestation), 'signer caller attestation missing')
    children = cleanup.get('cleanup', {})
    require({'bus', 'secrets', 'daemon', 'approver'} <= set(children), 'signer service cleanup missing')
    for name, child in children.items():
        require(child.get('reaped') is True and child.get('output_drained') is True
                and not child.get('forced_kill') and child.get('exit_code') in (0, -15)
                and (name == 'client' or child.get('terminated_by_cleanup') is True), 'signer child cleanup failed')
        if name in ('daemon', 'approver', 'client'):
            require(child.get('sanitizer_reports') == [] and child.get('exit_code') == 0,
                    'signer sanitizer/graceful shutdown failed')


def _require_no_mdk_lifecycle(raw, closure, descriptors, recovery):
    """Selected scope without original MDK: no image fixture, peer, runtime or journal may exist.

    When MDK IS selected, qualify's existing image production/use/cleanup
    validation stays mandatory; this never waives it."""
    from test_selection_mdk import CAPABILITY, IMAGE_TESTS
    require('mdk_runtime' not in raw and CAPABILITY not in raw.get('runtime_capability_evidence', {})
            and CAPABILITY not in raw.get('required_capabilities', []) and not recovery
            and not set(closure) & set(IMAGE_TESTS.values())
            and not any(CAPABILITY in descriptors.get(name, {}).get('requires', []) for name in closure),
            'unselected original MDK lifecycle was provisioned or executed')


def _event_ancestry(plan, repo):
    """Persisted event identities must be the tested source's real Git ancestry.

    A pull_request plan tests a two-parent merge: its PR head must be a non-first
    parent of the tested merge and its event base an ancestor of it. Equal trees
    never alias identities, and a forged field cannot hide behind an unchanged
    selection."""
    from test_selection import git, commit_sha
    head = commit_sha(repo, plan['head'])
    events = plan.get('event')
    for event in (events if isinstance(events, list) else [events or {}]):
        if event.get('kind') != 'pull_request':
            continue
        parents = os.fsdecode(git(repo, 'rev-list', '--parents', '-n', '1', head).stdout).split()[1:]
        require(commit_sha(repo, event['tested_sha']) == head and commit_sha(repo, event['pr_head_sha']) in parents[1:]
                and git(repo, 'merge-base', '--is-ancestor', commit_sha(repo, event['base_sha']), head,
                        check=False).returncode == 0,
                'pull_request event identities are not the tested merge ancestry')


def _rederived_plan(policy, plan, repo):
    """The exact source-bound dependency plan, recomputed from the tested history."""
    from test_selection import git_plan
    require(repo is not None, 'selected-normal qualification requires the exact tested checkout')
    _event_ancestry(plan, repo)
    event = plan.get('event') or None
    again = git_plan(repo, policy, head=plan['head'], base=plan.get('base'), context=plan['context'],
                     mode=plan['requested_mode'], event=event, profiles=plan.get('partial_profiles') or None)
    require(again == plan, 'selective plan differs from the source-bound dependency plan re-derived from the tested history')
    return again


def _scope(policy, plan, lane, profile, repo):
    require(lane is not None and profile.get(FIELD) == MARKER, 'profile has no authored normal contract')
    if plan['effective_mode'] == 'broad' or profile.get(SELECTED_FIELD) != SELECTED_MARKER:
        # Exactly the pre-existing full-contract condition and message (1640 qualify).
        require(plan['source_identity'].get('kind') == 'committed' and plan['effective_mode'] == 'broad',
                'normal full-profile qualification requires a fresh committed broad plan')
        return 'full'
    require(plan['source_identity'].get('kind') == 'committed', 'selected-normal requires a committed source')
    require(plan['effective_mode'] == plan['requested_mode'] == 'affected' and not plan.get('partial_profiles')
            and not plan.get('broadening_reasons') and not plan.get('deleted_ref'),
            'selected-normal requires an unrestricted, unbroadened affected plan')
    _rederived_plan(policy, plan, repo)
    return 'selected'


def qualify(policy, plan, public_path, adapter_path, apparmor_dir=None, *, locator=None, transport=None, repo=None):
    from test_selection import (read_json, digest, validate_plan, coverage_state,
                                validate_default_build_evidence, validate_phase_parallelism_evidence)
    import test_selection_subset as subset
    import test_selection_realization as backend_api
    from test_selection_services import verify_native_public_receipt
    validate_metadata(policy)
    validate_plan(plan, policy)
    locator = locator or Locator()
    public_ref, adapter_ref = retained(public_path, locator), retained(adapter_path, locator)
    public, adapter = _json(locator, public_path), _json(locator, adapter_path)
    require(public.get('receipt_kind') not in ('lane_failure_v1', KIND), 'not an original execution receipt')
    pid = public.get('profile')
    lane = next((item for item in plan['lanes'] if item['profile'] == pid), None)
    require(lane is not None and pid in policy['profiles'], 'profile has no authored normal contract')
    profile = policy['profiles'][pid]
    require(all(public.get(key) == plan[key] for key in ('source_identity', 'inventory_hash', 'plan_id'))
            and public.get('profile_hash') == lane['profile_hash'], 'public source/profile/plan binding differs')
    scope = _scope(policy, plan, lane, profile, repo)
    require(not _omission(public) and not subset.active_contracts(policy).get(pid),
            'historical omission authority cannot become normal')
    require(not public.get('baseline_exclusions'), 'omitted registrations cannot become normal')
    _verify_native(public, locator)
    if locator.roots is None:
        verify_native_public_receipt(public)  # in place, the original verifier must agree
    work = Path(public['native_custody_final']['evidence_root'])
    require(Path(public_path) == work / 'native-receipt.json', 'public receipt is not the post-exit adapter output')
    require(adapter.get('adapter') == 'test-gate' and adapter.get('custody_mode') == 'pidns'
            and adapter.get('status') == 'partial' and not adapter.get('errors') and not adapter.get('bypasses'),
            'outer adapter did not complete cleanly under pidns')
    executions = [item for item in adapter.get('executions', []) if item.get('plan_id') == plan['plan_id']
                  and item.get('source_identity') == plan['source_identity']]
    require(len(executions) == 1 and executions[0].get('status') == 'partial', 'outer source/plan result missing')
    lanes = [item for item in executions[0].get('lanes', []) if item.get('profile') == pid]
    require(len(lanes) == 1 and lanes[0].get('status') == 'passed'
            and lanes[0].get('normalized_receipt') == str(public_path)
            and lanes[0].get('evidence') == str(work)
            and lanes[0].get('retained_evidence') == str(work / 'native-final.json'), 'post-exit lane barrier not proved')
    recovery = lanes[0].get('mdk_recovery', [])
    require(isinstance(recovery, list) and all(item.get('closed_in_namespace') is True
            and 'recovery_exit' not in item for item in recovery), 'MDK required outside-namespace recovery')
    extra = {}
    if adapter.get('namespace_apparmor_profile'):
        require(apparmor_dir is not None, 'AppArmor teardown evidence required')
        directory = Path(apparmor_dir)
        for name in APPARMOR_FILES:
            extra[name] = retained(directory / name, locator)
        teardown = _json(locator, directory / 'teardown.json')
        require(_json(locator, directory / 'setup.json').get('name') == adapter['namespace_apparmor_profile']
                and teardown.get('status') == 'removed' and teardown.get('kind') == 'namespace_apparmor_teardown_v1'
                and _file(locator, directory / 'profiles-before.txt').read_bytes()
                == _file(locator, directory / 'profiles-after.txt').read_bytes(),
                'owned AppArmor profile teardown not proved')
    service_caps = set(profile.get('capabilities', [])) & {'display', 'private_bus'}
    if service_caps:
        service_state = _json(locator, work / 'services/services.json')
        require(service_state.get('capabilities') == sorted(service_caps) and service_state.get('status') == 'passed'
                and service_state.get('private_home_removed') is True and not service_state.get('cleanup_errors')
                and service_state.get('services') and all(item.get('reaped') is True
                    and item.get('exit_code') in (0, -15) and not item.get('forced_exit')
                    for item in service_state['services']), 'private service setup/cleanup missing')
    raw = public.get('backend_receipt', {})
    resolved = _json(locator, work / 'resolved.json')
    real = _json(locator, work / 'build/nostrc-test-realization.json')
    locator.real = real
    require(all(value.get('diagnostic_boundary') == 'linux_pidns_v1' for value in (public, raw, resolved, real))
            and backend_api._profile_fingerprint(real) == real['profile_fingerprint'], 'unbound/non-pidns execution')
    require(real.get('profile_id') == resolved.get('profile_id') == raw.get('profile_id') == pid
            and real.get('configuration') == raw.get('configuration') == profile['build_type']
            and not raw.get('baseline_exclusions') and not resolved.get('baseline_exclusions')
            and raw.get('required_capabilities') == resolved.get('required_capabilities')
            and set(raw['required_capabilities']) <= set(profile.get('capabilities', []))
            and raw.get('runtime_capability_evidence', {}) == resolved.get('runtime_capability_evidence', {}),
            'raw execution domain/capabilities/configuration differs')
    require(real.get('profile') == profile and real.get('source_identity') == plan['source_identity']
            and real.get('inventory_hash') == plan['inventory_hash'] == digest(policy), 'realized source/profile/inventory differs')
    for key, expected in dict(source_identity=plan['source_identity'], inventory_hash=plan['inventory_hash'],
                              profile_fingerprint=real['profile_fingerprint'], phase_context=real['phase_context'],
                              phase_context_hash=real['phase_context_hash']).items():
        require(raw.get(key) == resolved.get(key) == expected, 'raw/resolved identity differs: ' + key)
    require(raw.get('plan_hash') == resolved.get('plan_hash') == digest(plan)
            and raw.get('resolved_hash') == resolved.get('resolved_hash')
            and resolved['resolved_hash'] == digest({k: v for k, v in resolved.items() if k != 'resolved_hash'}),
            'resolved/plan binding differs')
    eligible = {ident: check for ident, check in policy['checks'].items() if pid in check['profiles']}
    tests, descriptors = backend_api._metadata(policy, real)
    def members(selection):
        return sorted(name for check in selection.values() if check['kind'] == 'ctest' for name in check['ctest_tests'][pid])
    # The whole configured universe must be owned (claimed primary or fixture of one) in BOTH scopes.
    universe = backend_api._fixture_closure(members(eligible), tests, descriptors)
    require(universe == sorted(tests) and resolved['registration_hash'] == backend_api._registration_hash(real['tests']),
            'configured registration universe is not wholly owned')
    if scope == 'full':
        checks = eligible
        require(set(lane['checks']) == set(checks) == set(resolved['checks']) == set(raw.get('checks', {})),
                'normal profile is missing authored checks')
    else:
        checks = {ident: eligible[ident] for ident in lane['checks']}
        require(checks and set(checks) <= set(eligible) and set(checks) == set(resolved['checks']) == set(raw.get('checks', {}))
                and resolved.get('primary_tests') == members(checks),
                'selected checks/primaries differ from the source-bound plan lane')
    closure = backend_api._fixture_closure(members(checks), tests, descriptors)
    require(closure == resolved['tests'] == raw.get('tests')
            and (scope == 'selected' or closure == sorted(tests)),
            'whole configured registration/fixture universe was not executed' if scope == 'full'
            else 'executed registrations differ from the exact selected fixture closure')
    pre, post = raw.get('input_audit_before_build'), raw.get('input_audit')
    for audit, after in ((pre, False), (post, True)):
        require(isinstance(audit, dict) and audit.get('passed') is True and audit.get('gaps') == []
                and audit.get('after_build') is after and audit.get('selective_enforcement') is after
                and not audit.get('baseline_exclusions')
                and audit.get('audit_hash') == digest({k: v for k, v in audit.items() if k != 'audit_hash'}),
                'full pre/post input audit missing or incomplete')
        for key in ('source_identity', 'inventory_hash', 'profile_fingerprint', 'phase_context', 'phase_context_hash'):
            require(audit.get(key) == resolved[key], 'audit identity differs: ' + key)
    require(post.get('header_entries', 0) > 0 and isinstance(post.get('header_index_hash'), str)
            and len(post['header_index_hash']) == 64 and post.get('input_hashes'), 'full discovered-header audit missing')
    # Eligible default contracts stay required even when unselected (full audit + prerequisite build).
    validate_default_build_evidence(profile, raw, eligible, resolved=resolved)
    defaults = {ident: check for ident, check in eligible.items() if check.get('build_scope') == 'default'}
    require(defaults, 'default build contract missing')
    validate_phase_parallelism_evidence(profile, raw, require_test=bool(closure))
    if pid in subset.full_profiles(policy):
        subset.validate_full_receipt(public, raw, plan, lane, profile, checks=dict(defaults, **checks))
    observed = raw.get('observed_executions', {})
    # Independently derived: exact registration closure plus selected command checks.
    expected_ids = sorted(closure + [ident for ident, check in checks.items() if check['kind'] != 'ctest'])
    require(resolved.get('expected_executions') == expected_ids == raw.get('expected_executions')
            and sorted(observed) == expected_ids and raw.get('outcome') == 'passed'
            and (raw.get('ctest_exit_code') == 0 if closure else 'ctest_exit_code' not in raw)
            and raw.get('attempt') == 1 and not raw.get('previous_attempts'),
            'selected physical executions missing, retried or failed')
    expected_xfail = {name for check in checks.values() if isinstance(check.get('expected_outcome'), dict)
                      and check['expected_outcome'] == dict(case='mdk09_incompatibility', exit_code=77)
                      for name in check['ctest_tests'][pid]}
    require(all(outcome == 'passed' or (name in expected_xfail and outcome == 'expected_incompatibility')
                for name, outcome in observed.items()), 'skipped/unsupported execution outcome')
    for ident, check in checks.items():
        names = check['ctest_tests'][pid] if check['kind'] == 'ctest' else [ident]
        result = raw['checks'][ident]
        actual = 'expected_incompatibility' if any(observed[name] == 'expected_incompatibility' for name in names) else 'passed'
        require(public.get('checks', {}).get(ident) == result and result.get('outcome') == actual
                and not result.get('sanitizer_reports') and not result.get('initial_failure')
                and all(result.get(key) == names for key in ('executions', 'expected_executions', 'observed_executions')),
                'primary execution accounting differs: ' + ident)
        _expected_check_outcome(check, result)
    if not closure:
        # Command-only selection: no CTest ran, so no CTest evidence may exist.
        require(scope == 'selected' and not raw.get('ctest_process') and not raw.get('ctest_environment')
                and 'ctest_process_authority' not in raw, 'command-only selection carries unexplained CTest evidence')
    else:
        process, env = raw.get('ctest_process', {}), raw.get('ctest_environment', {})
        require(raw.get('ctest_process_authority') == 'in_memory_executor_v1'
                and process.get('outcome') == 'passed' and process.get('exit_code') == 0
                and process.get('argv') in raw.get('commands', [])
                and env.get('base_hash') == real['phase_context']['environment_hashes']['test']
                and process.get('environment_hash') == env.get('effective_hash'), 'physical CTest argv/environment missing')
        overlays = []
        if 'docker' in raw['required_capabilities']:
            overlays += ['NOSTRC_MDK_STATE', 'NOSTRC_MDK_PYTHON']
        # The signer never overlays the CTest environment: its credentials are
        # the admitted registration's resource allocation (see _signer).
        require(env.get('overlay_names') == sorted(overlays)
                and (overlays or env.get('effective_hash') == env.get('base_hash')),
                'undeclared CTest environment overlay')
        directory = Path(raw['evidence_dir'])
        config = ['-C', real['configuration']] if real['configuration'] else []
        expected_argv = ['ctest', '--test-dir', real['build_dir'], *config, '-I', str(directory / 'ctest-indices.txt'),
                         '--output-on-failure', '--output-junit', str(directory / 'results.xml'),
                         '--parallel', str(raw['execution_parallelism']['test'])]
        if 'signer_endpoint' in raw['required_capabilities']:
            expected_argv += ['--resource-spec-file', str((raw.get('signer_runtime') or {}).get('delivery', {}).get('resource_spec_file'))]
        argv = process['argv']
        require(argv == expected_argv or (argv[:-2] == expected_argv and argv[-2] == '--timeout'
                and argv[-1].isdigit() and int(argv[-1]) > 0), 'physical CTest argv differs from contract')
        indices = [str(i + 1) for i, test in enumerate(real['tests']) if test['name'] in set(closure)]
        require(len(indices) == len(closure)
                and _file(locator, directory / 'ctest-indices.txt').read_text() == '0,0,1,' + ','.join(indices) + '\n',
                'physical CTest selection does not cover whole registration universe' if scope == 'full'
                else 'physical CTest selection differs from the exact selected closure')
        logfile = directory / 'ctest.log'
        require(logfile.is_relative_to(work), 'CTest output outside the sealed lane')
        log_bytes = _file(locator, logfile).read_bytes()
        require(hashlib.sha256(log_bytes).hexdigest() == process.get('output_sha256'), 'CTest output binding differs')
        require(not any(marker in log_bytes for marker in
                        (b'ERROR: AddressSanitizer', b'ERROR: LeakSanitizer', b': runtime error: ')), 'sanitizer report')
    _signer(raw, profile, post, real['repo_root'], real['tests'], closure, locator)
    if scope == 'selected' and not ('docker' in raw.get('required_capabilities', []) or 'mdk_runtime' in raw):
        _require_no_mdk_lifecycle(raw, closure, descriptors, recovery)
    if 'docker' in raw.get('required_capabilities', []) or 'mdk_runtime' in raw:
        from test_selection_mdk import validate_runtime
        validate_mdk_graph(real, raw['runtime_capability_evidence']['docker'])
        validate_runtime(raw, plan, lane, profile)
        require(any(item.get('state') == raw['mdk_runtime']['state'] for item in recovery), 'MDK post-exit journal check missing')
    result = dict(schema_version=1, receipt_kind=KIND, lane_id=lane['id'], profile=pid,
                  profile_hash=lane['profile_hash'], profile_fingerprint=real['profile_fingerprint'],
                  source_identity=plan['source_identity'], inventory_hash=plan['inventory_hash'], plan_id=plan['plan_id'],
                  job_conclusion='success', registration_audit='passed', input_audit='passed', checks=raw['checks'],
                  normal_contract=MARKER, public_evidence=public_ref, adapter_evidence=adapter_ref,
                  apparmor_evidence=extra, executed_registrations=closure,
                  full_audit_hashes=dict(pre=pre['audit_hash'], post=post['audit_hash']),
                  **coverage_state(policy))
    if scope == 'selected':
        # Explicitly NOT executed and never passed/covered; this is not whole-universe execution.
        result.update(normal_contract=SELECTED_MARKER, execution_scope=SELECTED_SCOPE,
                      selected_checks=sorted(checks), not_executed_checks=sorted(set(eligible) - set(checks)),
                      not_executed_registrations=sorted(set(tests) - set(closure)),
                      universe_registrations=len(tests), plan_rederived=True,
                      whole_universe_executed=False, full_profile_acceptance=False)
    if transport is not None:
        result['transport'] = transport
    result['qualification_hash'] = digest(result)
    return result


def verified_transport(policy, plan, receipt):
    """validate_receipt for one transported bundle; returns the public receipt and the
    locator over that bundle's verified, sealed bytes (for read-only diagnostics)."""
    require('transport' in receipt and _PORTABLE_ONLY, 'not a transported normal receipt')
    public = validate_receipt(policy, plan, receipt)
    return public, _LOADED[receipt['qualification_hash']]['locator']


def validate_receipt(policy, plan, receipt):
    if 'transport' in receipt or _PORTABLE_ONLY:
        # Aggregation: only a receipt loaded from its own verified bundle; no
        # fallback to (or acceptance of) original worker paths.
        key = receipt.get('qualification_hash')
        require(key not in _LOAD_ERRORS, 'portable bundle refused: ' + str(_LOAD_ERRORS.get(key)))
        entry = _LOADED.get(key)
        require(entry is not None and entry['receipt'] == receipt and 'transport' in receipt,
                'normal receipt was not loaded from its verified portable bundle')
        expected = qualify(policy, plan, receipt['public_evidence']['path'], receipt['adapter_evidence']['path'],
                           entry['apparmor'], locator=entry['locator'], transport=entry['transport'], repo=entry['repo'])
        require(receipt == expected, 'normal qualification changed/forged')
        verify_source_closure(entry['locator'], entry['repo'], plan)
        return _json(entry['locator'], receipt['public_evidence']['path'])
    for field in ('public_evidence', 'adapter_evidence'):
        require(retained(receipt[field]['path']) == receipt[field], 'retained evidence changed: ' + field)
    apparmor = receipt.get('apparmor_evidence', {})
    directory = str(Path(apparmor['setup.json']['path']).parent) if apparmor else None
    expected = qualify(policy, plan, receipt['public_evidence']['path'], receipt['adapter_evidence']['path'], directory)
    require(receipt == expected, 'normal qualification changed/forged')
    return _json(None, receipt['public_evidence']['path'])


# ---------------------------------------------------------------- transport --
TRANSPORT_FORMAT = 'nostrc_normal_transport_v1'
ARCHIVE = 'normal-transport.bin'
ORIGINAL = 'original-native-receipt.json'
MAGIC = b'NOSTRC-NORMAL-TRANSPORT-1\n'
LIMITS = dict(archive_bytes=8 << 30, manifest_bytes=64 << 20, members=400_000, name_bytes=1024,
              link_bytes=1024, receipt_bytes=16 << 20, headroom_bytes=256 << 20)
_LOADED, _LOAD_ERRORS = {}, {}
_PORTABLE_ONLY = 0
# Only what qualification reads and the native seal covers travels; anything
# else in the lane stays in the separately retained raw evidence artifact.
LANE_FILES = ('receipt.json', 'resolved.json', 'resolve.log', 'run.log', 'lane.log',
              'native-seal.json', 'native-final.json', 'native-receipt.json')
LANE_ROOTS = ('build', 'evidence', 'services', 'source-inputs')


def _roots(public_path):
    """Logical roots fixed by the sealed public receipt path (worker layout only)."""
    public = Path(str(public_path))
    work = public.parent
    require(public.name == 'native-receipt.json' and os.path.isabs(str(public)) and os.path.normpath(str(public)) == str(public)
            and work.name.startswith('lane-') and work.parent.name.isdigit() and len(work.parts) >= 6,
            'unsupported transported evidence root shape')
    run = work.parents[1]
    return dict(work=work, run=run, apparmor=run.parent.parent / 'namespace-apparmor',
                lane=(work.parent.name, work.name))


def _name_ok(name):
    pure = PurePosixPath(name)
    return (isinstance(name, str) and 0 < len(name.encode()) <= LIMITS['name_bytes'] and not pure.is_absolute()
            and str(pure) == name and '..' not in pure.parts and '.' not in pure.parts and '\0' not in name
            and pure.parts[0] in ('run', 'apparmor'))


def _members(run, lane, apparmor):
    """Exact retained bytes of the run's adapter receipt, the one lane tree and AppArmor evidence."""
    members, total = [], 0
    def add(name, path):
        nonlocal total
        info = os.lstat(path)
        require(_name_ok(name) and len(members) < LIMITS['members'], 'transport member limit/name refused: ' + name)
        if stat.S_ISDIR(info.st_mode):
            members.append(dict(name=name, kind='dir'))
        elif stat.S_ISREG(info.st_mode):
            total += info.st_size
            require(total <= LIMITS['archive_bytes'], 'transport input exceeds archive limit')
            members.append(dict(name=name, kind='file', size=info.st_size, sha256=_sha(path),
                                executable=bool(info.st_mode & 0o111)))
        elif stat.S_ISLNK(info.st_mode):
            target = os.readlink(path)
            resolved = os.path.normpath(os.path.join(os.path.dirname(name), target))
            require(0 < len(target.encode()) <= LIMITS['link_bytes'] and not os.path.isabs(target)
                    and resolved.split('/')[0] == name.split('/')[0] and '..' not in resolved.split('/'),
                    'unsupported (absolute/escaping) retained symlink: ' + name)
            members.append(dict(name=name, kind='symlink', target=target))
        else:
            raise ValueError('normal execution: special retained file refused: ' + name)
    def tree(name, path):
        add(name, path)
        if stat.S_ISDIR(os.lstat(path).st_mode):
            for child in sorted(os.listdir(path)):
                tree(name + '/' + child, path / child)
    add('run', run)
    add('run/adapter-receipt.json', run / 'adapter-receipt.json')
    add('run/' + lane[0], run / lane[0])
    work = run.joinpath(*lane)
    add('run/' + '/'.join(lane), work)
    for name in sorted(os.listdir(work)):
        if name in LANE_FILES:
            add('run/%s/%s' % ('/'.join(lane), name), work / name)
        elif name in LANE_ROOTS:
            tree('run/%s/%s' % ('/'.join(lane), name), work / name)
    if apparmor is not None:
        add('apparmor', apparmor)
        for name in APPARMOR_FILES:
            add('apparmor/' + name, apparmor / name)
    return members, total


def package(receipt_path, out_path, identity, apparmor):
    """Write the framed archive; returns the transport descriptor (not yet published)."""
    roots = _roots(receipt_path)
    members, total = _members(roots['run'], roots['lane'], roots['apparmor'] if apparmor else None)
    manifest = (json.dumps(dict(format=TRANSPORT_FORMAT, identity=identity, members=members),
                           sort_keys=True, separators=(',', ':')) + '\n').encode()
    require(len(manifest) <= LIMITS['manifest_bytes'], 'transport manifest exceeds limit')
    size = len(MAGIC) + 8 + len(manifest) + total
    require(shutil.disk_usage(Path(out_path).parent).free >= size + LIMITS['headroom_bytes'],
            'insufficient space for transport archive')
    digest = hashlib.sha256()
    fd = os.open(out_path, os.O_WRONLY | os.O_CREAT | os.O_EXCL | getattr(os, 'O_NOFOLLOW', 0), 0o644)
    with os.fdopen(fd, 'wb') as stream:
        def write(data):
            digest.update(data)
            stream.write(data)
        write(MAGIC); write(len(manifest).to_bytes(8, 'big')); write(manifest)
        for member in members:
            if member['kind'] != 'file':
                continue
            source = (roots['run'] if member['name'].split('/')[0] == 'run' else roots['apparmor']).joinpath(
                *member['name'].split('/')[1:])
            check, copied = hashlib.sha256(), 0
            with open(source, 'rb') as data:
                for block in iter(lambda: data.read(1 << 20), b''):
                    check.update(block); copied += len(block); write(block)
            require(copied == member['size'] and check.hexdigest() == member['sha256'],
                    'retained evidence changed while packing: ' + member['name'])
    return dict(format=TRANSPORT_FORMAT, archive=ARCHIVE, archive_bytes=size, archive_sha256=digest.hexdigest(),
                manifest_sha256=hashlib.sha256(manifest).hexdigest(), members=len(members))


def _validate_manifest(manifest, transport, identity, roots, apparmor):
    require(isinstance(manifest, dict) and set(manifest) == {'format', 'identity', 'members'}
            and manifest['format'] == TRANSPORT_FORMAT and manifest['identity'] == identity
            and isinstance(manifest['members'], list) and len(manifest['members']) == transport['members']
            <= LIMITS['members'], 'transport manifest identity/shape differs')
    lane = 'run/' + '/'.join(roots['lane'])
    allowed = {'run', 'run/adapter-receipt.json', 'run/' + roots['lane'][0]}
    if apparmor:
        allowed |= {'apparmor'} | {'apparmor/' + name for name in APPARMOR_FILES}
    kinds, total = {}, 0
    for member in manifest['members']:
        require(isinstance(member, dict) and _name_ok(member.get('name')) and member['name'] not in kinds,
                'unsafe/duplicate transport member')
        name, kind = member['name'], member.get('kind')
        inner = name[len(lane) + 1:].split('/') if name.startswith(lane + '/') else None
        require(name in allowed or name == lane or (inner and (
                (len(inner) == 1 and (inner[0] in LANE_FILES and kind == 'file' or inner[0] in LANE_ROOTS and kind == 'dir'))
                or (len(inner) > 1 and inner[0] in LANE_ROOTS))),
                'cross-lane/unadmitted transport member: ' + name)
        require(kind == 'file' or name not in allowed or name in ('run', 'apparmor', 'run/' + roots['lane'][0]),
                'unexpected transport member kind: ' + name)
        require(kind != 'symlink' or (inner and len(inner) > 1), 'symlink outside sealed lane roots: ' + name)
        parent = name.rsplit('/', 1)[0] if '/' in name else None
        require(parent is None or kinds.get(parent) == 'dir', 'transport member parent is not a listed directory: ' + name)
        if kind == 'dir':
            require(set(member) == {'name', 'kind'}, 'malformed directory member')
        elif kind == 'file':
            require(set(member) == {'name', 'kind', 'size', 'sha256', 'executable'} and type(member['size']) is int
                    and 0 <= member['size'] and isinstance(member['sha256'], str)
                    and re.fullmatch(r'[0-9a-f]{64}', member['sha256']) and type(member['executable']) is bool,
                    'malformed file member')
            total += member['size']
        elif kind == 'symlink':
            target = member.get('target')
            require(set(member) == {'name', 'kind', 'target'} and isinstance(target, str)
                    and 0 < len(target.encode()) <= LIMITS['link_bytes'] and not os.path.isabs(target) and '\0' not in target,
                    'malformed symlink member')
            resolved = os.path.normpath(os.path.join(os.path.dirname(name), target)).split('/')
            require(resolved[0] == name.split('/')[0] and '..' not in resolved, 'transport symlink escapes its root')
        else:
            raise ValueError('normal execution: unsupported transport member kind')
        kinds[name] = kind
    require(kinds.get('run') == 'dir' and kinds.get('run/adapter-receipt.json') == 'file' and kinds.get(lane) == 'dir'
            and (not apparmor or all(kinds.get('apparmor/' + n) == 'file' for n in APPARMOR_FILES)),
            'transport lacks required evidence roots')
    return total


def extract(archive, destination, transport, identity, roots, apparmor):
    """Type/size limits first, then the digest, then bounded streamed materialization (one descriptor)."""
    fd = os.open(archive, os.O_RDONLY | getattr(os, 'O_NOFOLLOW', 0))
    with os.fdopen(fd, 'rb') as stream:
        info = os.fstat(stream.fileno())
        require(stat.S_ISREG(info.st_mode) and type(transport.get('archive_bytes')) is int
                and info.st_size == transport['archive_bytes'] <= LIMITS['archive_bytes'],
                'transport archive missing/oversized/size differs')
        digest = hashlib.sha256()
        for block in iter(lambda: stream.read(1 << 20), b''):
            digest.update(block)
        require(digest.hexdigest() == transport['archive_sha256'], 'transport archive bytes differ')
        stream.seek(0)
        require(stream.read(len(MAGIC)) == MAGIC, 'not a normal transport archive')
        length = int.from_bytes(stream.read(8), 'big')
        require(length <= LIMITS['manifest_bytes'], 'transport manifest exceeds limit')
        manifest_bytes = stream.read(length)
        require(len(manifest_bytes) == length and hashlib.sha256(manifest_bytes).hexdigest() == transport['manifest_sha256'],
                'transport manifest bytes differ')
        manifest = json.loads(manifest_bytes)
        total = _validate_manifest(manifest, transport, identity, roots, apparmor)
        require(len(MAGIC) + 8 + length + total == info.st_size, 'transport archive length differs from manifest')
        require(shutil.disk_usage(destination).free >= total + LIMITS['headroom_bytes'], 'insufficient extraction space')
        links = []
        for member in manifest['members']:
            path = destination.joinpath(*member['name'].split('/'))
            if member['kind'] == 'dir':
                os.mkdir(path, 0o700)
            elif member['kind'] == 'symlink':
                links.append((member['target'], path))
            else:
                fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL | getattr(os, 'O_NOFOLLOW', 0),
                             0o700 if member['executable'] else 0o600)
                check, remaining = hashlib.sha256(), member['size']
                with os.fdopen(fd, 'wb') as output:
                    while remaining:
                        block = stream.read(min(remaining, 1 << 20))
                        require(block, 'transport archive truncated')
                        check.update(block); output.write(block); remaining -= len(block)
                require(check.hexdigest() == member['sha256'], 'transport member bytes differ: ' + member['name'])
        require(stream.read(1) == b'', 'trailing transport bytes')
    for target, path in links:
        os.symlink(target, path)
    for target, path in links:
        top = destination / path.relative_to(destination).parts[0]
        require(Path(os.path.realpath(path)).is_relative_to(top), 'transport symlink resolves outside its root')
    return manifest


def verify_source_closure(locator, repo, plan):
    """Every sealed source claim equals the same tested commit in this independent checkout."""
    identity = plan['source_identity']
    require(identity.get('kind') == 'committed', 'portable normal proof requires a committed source')
    repo = Path(repo).resolve(strict=True)
    def git(*args):
        return subprocess.run(['git', '-C', str(repo), *args], capture_output=True, text=True, check=True).stdout
    require(git('rev-parse', 'HEAD').strip() == identity['sha'], 'aggregate checkout is not the tested source')
    require(git('status', '--porcelain=v1', '--ignore-submodules=none', '--untracked-files=no') == '',
            'aggregate checkout differs from the tested source')
    require(all(line[:1] == ' ' for line in git('submodule', 'status', '--recursive').splitlines()),
            'aggregate checkout submodules are missing or differ from the tested gitlinks')
    source = str(locator.real['repo_root'])
    claims = locator.seal['source_claims']
    require(os.path.isabs(source) and os.path.normpath(source) == source and isinstance(claims, dict) and claims,
            'unsupported/empty sealed source closure')
    require(claims == _derived_claims(locator.result, source),
            'sealed source claims differ from the sealed raw input audits')
    tracked = set(git('ls-files', '-z', '--recurse-submodules').split('\0')) - {''}
    for name, expected in claims.items():
        require(os.path.normpath(name) == name and name.startswith(source + os.sep), 'source claim outside sealed source root')
        relative = Path(name).relative_to(source)
        require(relative.as_posix() in tracked, 'source claim is not a tracked file of the tested commit: ' + name)
        candidate = repo.joinpath(*relative.parts)
        resolved = candidate.resolve(strict=True)
        require(resolved.is_relative_to(repo) and resolved.is_file() and _sha(resolved) == expected,
                'sealed source input differs from the tested checkout: ' + name)


def _derived_claims(result, source):
    """Lexical counterpart of the sealer's claim derivation over the sealed raw receipt."""
    claims = {}
    def visit(value):
        if isinstance(value, dict):
            for path, expected in (value.get('input_hashes') or {}).items():
                if os.path.isabs(path) and os.path.normpath(path) == path and path.startswith(source + os.sep):
                    require(claims.setdefault(path, expected) == expected, 'contradictory sealed source input claim')
            for child in value.values():
                visit(child)
        elif isinstance(value, list):
            for child in value:
                visit(child)
    visit(result)
    return claims


def _identity(receipt):
    return {key: receipt.get(key) for key in ('plan_id', 'lane_id', 'profile', 'profile_hash', 'source_identity')}


def _load_transport(path, receipt, repo, plan, temps):
    transport = receipt.get('transport')
    require(isinstance(transport, dict) and set(transport) == {'format', 'archive', 'archive_bytes', 'archive_sha256',
            'manifest_sha256', 'members'} and transport['format'] == TRANSPORT_FORMAT and transport['archive'] == ARCHIVE
            and type(transport['archive_bytes']) is int and type(transport['members']) is int,
            'normal receipt lacks a portable transport')
    profile = receipt.get('profile')
    require(isinstance(profile, str) and path.parent.name == profile and re.fullmatch(
            re.escape('dependency-receipt-%s-%s-' % (plan['plan_id'], profile)) + r'[1-9][0-9]*', path.parent.parent.name),
            'normal receipt is not inside its own lane artifact')
    require(receipt.get('plan_id') == plan['plan_id'], 'transported receipt belongs to another plan')
    roots = _roots(receipt['public_evidence']['path'])
    require(receipt['adapter_evidence']['path'] == str(roots['run'] / 'adapter-receipt.json'),
            'adapter evidence is not the sealed run receipt')
    apparmor = receipt.get('apparmor_evidence') or {}
    require(not apparmor or (set(apparmor) == set(APPARMOR_FILES) and all(
            apparmor[name]['path'] == str(roots['apparmor'] / name) for name in APPARMOR_FILES)),
            'AppArmor evidence is not the job teardown directory')
    archive = path.parent / ARCHIVE
    temp = Path(tempfile.mkdtemp(prefix='nostrc-normal-transport-')).resolve()
    temps.append(temp)
    extract(archive, temp, transport, _identity(receipt), roots, bool(apparmor))
    mapping = {str(roots['run']): temp / 'run'}
    if apparmor:
        mapping[str(roots['apparmor'])] = temp / 'apparmor'
    require(not str(roots['apparmor']).startswith(str(roots['run']) + os.sep), 'overlapping transport roots')
    return dict(receipt=receipt, locator=Locator(mapping), transport=transport, repo=repo,
                apparmor=str(roots['apparmor']) if apparmor else None)


@contextlib.contextmanager
def transported_receipts(paths, *, repo, plan):
    """Aggregate-side loader. Normal receipts resolve only inside their own verified bundles."""
    global _PORTABLE_ONLY
    temps, keys, receipts = [], [], []
    _PORTABLE_ONLY += 1
    try:
        for path in map(Path, paths):
            require(path.stat().st_size <= LIMITS['receipt_bytes'], 'receipt exceeds limit: ' + str(path))
            receipt = json.loads(path.read_text())
            receipts.append(receipt)
            if not (isinstance(receipt, dict) and receipt.get('receipt_kind') == KIND):
                continue
            key = receipt.get('qualification_hash')
            require(isinstance(key, str) and key not in _LOADED and key not in _LOAD_ERRORS,
                    'duplicate/malformed normal receipt')
            keys.append(key)
            try:
                _LOADED[key] = _load_transport(path, receipt, repo, plan, temps)
            except (OSError, ValueError, KeyError, TypeError, AttributeError, subprocess.SubprocessError) as exc:
                _LOAD_ERRORS[key] = str(exc)
        yield receipts
    finally:
        _PORTABLE_ONLY -= 1
        for key in keys:
            _LOADED.pop(key, None)
            _LOAD_ERRORS.pop(key, None)
        for temp in temps:
            shutil.rmtree(temp, ignore_errors=True)  # private extraction created above only


def _strip(receipt):
    return {key: value for key, value in receipt.items() if key not in ('transport', 'qualification_hash')}


def worker_qualify(repo, plan_path, pid, evidence_root, apparmor_dir, receipts_dir, *, policy=None):
    """Production worker step after pidns exit + AppArmor teardown; raw stays unless all checks pass.

    ``policy`` is only for synthetic tests; production loads the checkout's inventory."""
    sys.path.insert(0, str(Path(repo) / 'scripts'))
    import test_selection as selection
    if policy is None:
        policy = selection.load_inventory(Path(repo) / 'tests/selection/inventory.json')
    plan = selection.read_json(plan_path)
    if policy['profiles'].get(pid, {}).get(FIELD) != MARKER:
        print('normal qualification: %s has no authored normal contract; raw receipt unchanged' % pid)
        return None
    if plan['effective_mode'] != 'broad' and policy['profiles'][pid].get(SELECTED_FIELD) != SELECTED_MARKER:
        # Selective plan without the separately authored selected contract: the raw
        # receipt keeps its own (non-normal) accounting; nothing is promoted.
        print('normal qualification: %s has no authored selected-normal contract; raw receipt unchanged' % pid)
        return None
    evidence_root, receipts_dir = Path(evidence_root).resolve(strict=True), Path(receipts_dir).resolve(strict=True)
    apparmor_dir = Path(apparmor_dir)
    require(apparmor_dir == evidence_root.parent / 'namespace-apparmor', 'AppArmor evidence is not the job directory')
    current = receipts_dir / 'receipt.json'
    require(current.is_file() and not current.is_symlink(), 'raw worker receipt missing')
    matches = [path for path in evidence_root.glob('*/0/lane-*/native-receipt.json')
               if (lambda raw: raw.get('plan_id') == plan['plan_id'] and raw.get('profile') == pid
                   and raw.get('source_identity') == plan['source_identity'])(json.loads(path.read_text()))]
    require(len(matches) == 1 and matches[0].read_bytes() == current.read_bytes(),
            'normal contract requires exactly one retained pidns lane equal to the published raw receipt')
    public_path = matches[0]
    roots = _roots(public_path)
    adapter = json.loads((roots['run'] / 'adapter-receipt.json').read_text())
    provisioned = (apparmor_dir / 'setup.json').exists()
    require(provisioned == bool(adapter.get('namespace_apparmor_profile')),
            'provisioned AppArmor profile and gate custody disagree')
    apparmor = str(apparmor_dir) if provisioned else None
    in_place = qualify(policy, plan, str(public_path), str(roots['run'] / 'adapter-receipt.json'), apparmor, repo=repo)
    partial = receipts_dir / ('.' + ARCHIVE + '.partial')
    temps = []
    try:
        transport = package(public_path, partial, _identity(in_place), provisioned)
        temp = Path(tempfile.mkdtemp(prefix='nostrc-normal-selfcheck-')).resolve()
        temps.append(temp)
        extract(partial, temp, transport, _identity(in_place), roots, provisioned)
        mapping = {str(roots['run']): temp / 'run'}
        if provisioned:
            mapping[str(roots['apparmor'])] = temp / 'apparmor'
        locator = Locator(mapping)
        relocated = qualify(policy, plan, str(public_path), str(roots['run'] / 'adapter-receipt.json'), apparmor,
                            locator=locator, transport=transport, repo=repo)
        require(_strip(relocated) == _strip(in_place), 'relocated qualification differs from in-place qualification')
        verify_source_closure(locator, repo, plan)
        raw = current.read_bytes()
        with open(receipts_dir / ORIGINAL, 'xb') as stream:
            stream.write(raw)
        os.replace(partial, receipts_dir / ARCHIVE)
        staged = receipts_dir / '.receipt.json.normal'
        with open(staged, 'x') as stream:
            json.dump(relocated, stream, sort_keys=True, indent=2)
            stream.write('\n')
        os.replace(staged, current)  # the only lane receipt; raw bytes retained beside it
        return relocated
    finally:
        partial.unlink(missing_ok=True)  # owned partial output only
        for temp in temps:
            shutil.rmtree(temp, ignore_errors=True)


def main(argv=None):
    import argparse
    parser = argparse.ArgumentParser(description='normal execution worker qualification')
    commands = parser.add_subparsers(dest='command', required=True)
    worker = commands.add_parser('worker-qualify')
    for name in ('repo', 'plan', 'profile', 'evidence-root', 'apparmor-evidence', 'receipts-dir'):
        worker.add_argument('--' + name, required=True)
    args = parser.parse_args(argv)
    try:
        worker_qualify(args.repo, args.plan, args.profile, args.evidence_root, args.apparmor_evidence, args.receipts_dir)
    except (OSError, ValueError, KeyError, TypeError, AttributeError, RuntimeError, subprocess.SubprocessError) as exc:
        print('normal qualification refused; raw receipt retained unchanged: %s' % exc, file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
