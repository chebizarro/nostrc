"""Independent post-exit qualification of authored full-profile executions.

Raw custody/signer/MDK receipts retain their diagnostic flags. This contract
rechecks physical facts and references those original sealed bytes; it never
changes flags, grants rollout authority, or admits an omission witness.
"""
import hashlib
from pathlib import Path

KIND = 'normal_execution_v1'
FIELD = 'normal_execution'
MARKER = dict(schema_version=1, custody='linux_pidns_v1',
              audit_scope='full_configured_universe', registration_policy='all_owned_and_executed')


def require(value, message):
    if not value:
        raise ValueError('normal execution: ' + message)


def validate_metadata(policy):
    for pid, profile in policy.get('profiles', {}).items():
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


def retained(path):
    path = Path(path)
    require(path.is_absolute() and path.resolve(strict=True) == path and path.is_file() and not path.is_symlink(),
            'unsafe/missing retained file: ' + str(path))
    return dict(path=str(path), sha256=hashlib.sha256(path.read_bytes()).hexdigest())


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


def _signer(backend, profile, post, source_root):
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


def qualify(policy, plan, public_path, adapter_path, apparmor_dir=None):
    from test_selection import (read_json, digest, validate_plan, coverage_state,
                                validate_default_build_evidence, validate_phase_parallelism_evidence)
    import test_selection_subset as subset
    import test_selection_realization as backend_api
    from test_selection_services import verify_native_public_receipt
    validate_metadata(policy)
    validate_plan(plan, policy)
    public_ref, adapter_ref = retained(public_path), retained(adapter_path)
    public, adapter = read_json(public_path), read_json(adapter_path)
    require(public.get('receipt_kind') not in ('lane_failure_v1', KIND), 'not an original execution receipt')
    pid = public.get('profile')
    lane = next((item for item in plan['lanes'] if item['profile'] == pid), None)
    require(lane is not None and policy['profiles'][pid].get(FIELD) == MARKER, 'profile has no authored normal contract')
    profile = policy['profiles'][pid]
    require(all(public.get(key) == plan[key] for key in ('source_identity', 'inventory_hash', 'plan_id'))
            and public.get('profile_hash') == lane['profile_hash'], 'public source/profile/plan binding differs')
    require(plan['source_identity'].get('kind') == 'committed' and plan['effective_mode'] == 'broad',
            'normal full-profile qualification requires a fresh committed broad plan')
    require(not _omission(public) and not subset.active_contracts(policy).get(pid),
            'historical omission authority cannot become normal')
    require(not public.get('baseline_exclusions'), 'omitted registrations cannot become normal')
    verify_native_public_receipt(public)
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
        for name in ('setup.json', 'teardown.json', 'profiles-before.txt', 'profiles-after.txt'):
            extra[name] = retained(directory / name)
        require(read_json(directory / 'setup.json').get('name') == adapter['namespace_apparmor_profile']
                and read_json(directory / 'teardown.json').get('status') == 'removed'
                and read_json(directory / 'teardown.json').get('kind') == 'namespace_apparmor_teardown_v1'
                and (directory / 'profiles-before.txt').read_bytes() == (directory / 'profiles-after.txt').read_bytes(),
                'owned AppArmor profile teardown not proved')
    service_caps = set(profile.get('capabilities', [])) & {'display', 'private_bus'}
    if service_caps:
        service_state = read_json(work / 'services/services.json')
        require(service_state.get('capabilities') == sorted(service_caps) and service_state.get('status') == 'passed'
                and service_state.get('private_home_removed') is True and not service_state.get('cleanup_errors')
                and service_state.get('services') and all(item.get('reaped') is True
                    and item.get('exit_code') in (0, -15) and not item.get('forced_exit')
                    for item in service_state['services']), 'private service setup/cleanup missing')
    raw = public.get('backend_receipt', {})
    resolved = read_json(work / 'resolved.json')
    real = read_json(work / 'build/nostrc-test-realization.json')
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
    checks = {ident: check for ident, check in policy['checks'].items() if pid in check['profiles']}
    require(set(lane['checks']) == set(checks) == set(resolved['checks']) == set(raw.get('checks', {})),
            'normal profile is missing authored checks')
    tests, descriptors = backend_api._metadata(policy, real)
    primaries = sorted(name for check in checks.values() if check['kind'] == 'ctest'
                       for name in check['ctest_tests'][pid])
    closure = backend_api._fixture_closure(primaries, tests, descriptors)
    require(closure == sorted(tests) == resolved['tests'] == raw.get('tests')
            and resolved['registration_hash'] == backend_api._registration_hash(real['tests']),
            'whole configured registration/fixture universe was not executed')
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
    validate_default_build_evidence(profile, raw, checks, resolved=resolved)
    require(any(check.get('build_scope') == 'default' for check in checks.values()), 'default build contract missing')
    validate_phase_parallelism_evidence(profile, raw, require_test=bool(tests))
    if pid in subset.full_profiles(policy):
        subset.validate_full_receipt(public, raw, plan, lane, profile, checks=checks)
    observed = raw.get('observed_executions', {})
    require(set(observed) == set(resolved['expected_executions']) and raw.get('outcome') == 'passed'
            and raw.get('ctest_exit_code') == 0 and raw.get('attempt') == 1 and not raw.get('previous_attempts'),
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
    process, env = raw.get('ctest_process', {}), raw.get('ctest_environment', {})
    require(raw.get('ctest_process_authority') == 'in_memory_executor_v1'
            and process.get('outcome') == 'passed' and process.get('exit_code') == 0
            and process.get('argv') in raw.get('commands', [])
            and env.get('base_hash') == real['phase_context']['environment_hashes']['test']
            and process.get('environment_hash') == env.get('effective_hash'), 'physical CTest argv/environment missing')
    overlays = []
    if 'docker' in raw['required_capabilities']:
        overlays += ['NOSTRC_MDK_STATE', 'NOSTRC_MDK_PYTHON']
    if 'signer_endpoint' in raw['required_capabilities']:
        overlays += ['NOSTR_SIGNER_ENDPOINT', 'NOSTR_SIGNER_TOKEN']
    require(env.get('overlay_names') == sorted(overlays)
            and (overlays or env.get('effective_hash') == env.get('base_hash')),
            'undeclared CTest environment overlay')
    directory = Path(raw['evidence_dir'])
    config = ['-C', real['configuration']] if real['configuration'] else []
    expected_argv = ['ctest', '--test-dir', real['build_dir'], *config, '-I', str(directory / 'ctest-indices.txt'),
                     '--output-on-failure', '--output-junit', str(directory / 'results.xml'),
                     '--parallel', str(raw['execution_parallelism']['test'])]
    argv = process['argv']
    require(argv == expected_argv or (argv[:-2] == expected_argv and argv[-2] == '--timeout'
            and argv[-1].isdigit() and int(argv[-1]) > 0), 'physical CTest argv differs from contract')
    require((directory / 'ctest-indices.txt').read_text() == '0,0,1,' + ','.join(str(i + 1) for i in range(len(tests))) + '\n',
            'physical CTest selection does not cover whole registration universe')
    logfile = directory / 'ctest.log'
    require(logfile.is_relative_to(work) and hashlib.sha256(logfile.read_bytes()).hexdigest() == process.get('output_sha256'),
            'CTest output binding differs')
    require(not any(marker in logfile.read_bytes() for marker in
                    (b'ERROR: AddressSanitizer', b'ERROR: LeakSanitizer', b': runtime error: ')), 'sanitizer report')
    _signer(raw, profile, post, real['repo_root'])
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
    result['qualification_hash'] = digest(result)
    return result


def validate_receipt(policy, plan, receipt):
    from test_selection import read_json
    for field in ('public_evidence', 'adapter_evidence'):
        require(retained(receipt[field]['path']) == receipt[field], 'retained evidence changed: ' + field)
    apparmor = receipt.get('apparmor_evidence', {})
    directory = str(Path(apparmor['setup.json']['path']).parent) if apparmor else None
    expected = qualify(policy, plan, receipt['public_evidence']['path'], receipt['adapter_evidence']['path'], directory)
    require(receipt == expected, 'normal qualification changed/forged')
    return read_json(receipt['public_evidence']['path'])
