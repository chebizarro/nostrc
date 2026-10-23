"""Selected original MDK image/peer lifecycle controller.

The selected route composes two separately proven owners: the outer PID
namespace (test-gate --native-custody pidns) owns every host CMake/CTest
descendant and seals after namespace exit; mdk-gate's OwnedBuilder owns the
exact private BuildKit builder, its state volume, recorded peer CIDs and their
anonymous volumes. Neither substitutes for the other. A killed in-namespace
cleanup is replayed from the retained journal by `mdk-gate.py recover` outside
the namespace. The prepared ARM route, legacy `run` and Docker-only `images`
are separate and never aliased. complete/complete_gate_eligible stay false.
"""
import contextlib
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import shutil
import sys
import uuid

CAPABILITY = 'docker'
EXECUTION_DOMAIN = 'mdk_original_linux_v1'
WRAPPER = 'scripts/mdk-docker'
HELPER = 'scripts/mdk-gate.py'
MODULE = 'scripts/test_selection_mdk.py'
FIXED_TAGS = {'08': 'nostrc-mdk-interop:0.8.0', '09': 'nostrc-mdk-interop:0.9.0-probe'}
DRIVER_OVERRIDES = {'08': 'MDK_INTEROP_DRIVER', '09': 'MDK09_PROBE_DRIVER', '011': 'MDK011_INTEROP_DRIVER'}
TAG = re.compile(r'[a-z0-9][a-z0-9._/-]*:[A-Za-z0-9_][A-Za-z0-9_.-]{0,127}')


class MDKError(ValueError):
    pass


def require(condition, message):
    if not condition:
        raise MDKError(message)


def gate(root):
    """The exact source helper, loaded under a unique module name."""
    spec = importlib.util.spec_from_file_location('mdk_gate_' + uuid.uuid4().hex, Path(root) / HELPER)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def identity(path):
    path = Path(path).resolve(strict=True)
    return {'path': str(path), 'sha256': hashlib.sha256(path.read_bytes()).hexdigest()}


def cache_entries(build):
    entries = {}
    for line in (Path(build) / 'CMakeCache.txt').read_text().splitlines():
        match = re.fullmatch(r'([A-Za-z0-9_.+-]+):([A-Z]+)=(.*)', line)
        if match:
            require(match[1] not in entries, 'ambiguous realized cache entry: ' + match[1])
            entries[match[1]] = (match[2], match[3])
    return entries


def _on(entries, name):
    return entries.get(name, ('BOOL', 'OFF'))[1].upper() in ('ON', 'TRUE', '1', 'YES')


def realized_versions(entries):
    """Versions and tags exactly as the realized Groundhog CMake registers them."""
    versions = {}
    if _on(entries, 'BUILD_MDK_INTEROP'):
        versions['08'] = FIXED_TAGS['08']
    if _on(entries, 'BUILD_MDK011_INTEROP'):
        versions['011'] = entries.get('MDK011_INTEROP_IMAGE', ('STRING', ''))[1]
        require(TAG.fullmatch(versions['011']) is not None, 'realized MDK011 image tag is not a plain repository:tag')
        if _on(entries, 'BUILD_MDK09_PROBE') or 'BUILD_MDK09_PROBE' not in entries:
            versions['09'] = FIXED_TAGS['09']
    require(versions, 'realized build registers no original MDK image')
    for version in versions:
        # An external driver is an unowned peer: no image/CID custody exists.
        require(not entries.get(DRIVER_OVERRIDES[version], ('STRING', ''))[1],
                'external MDK driver override is not owned: ' + DRIVER_OVERRIDES[version])
    return versions


def contract(realization, environment=None):
    """Authored/realized provisioning contract only: never readiness or coverage."""
    profile = realization['profile']
    require(profile.get('execution_domain') == EXECUTION_DOMAIN and CAPABILITY in profile.get('capabilities', []),
            'original MDK lifecycle requires its authored execution domain and docker capability')
    require(sys.platform == 'linux' and profile.get('platform') == 'linux'
            and profile.get('architecture') == os.uname().machine,
            'original MDK lifecycle requires exact native Linux profile architecture')
    root, build = Path(realization['repo_root']), Path(realization['build_dir'])
    entries = cache_entries(build)
    require(entries.get('CMAKE_HOME_DIRECTORY', ('INTERNAL', ''))[1] == str(root), 'realized source root mismatch')
    require(entries.get('GROUNDHOG_DOCKER') == ('FILEPATH', str(root / WRAPPER)),
            'realized GROUNDHOG_DOCKER must be the source lifecycle wrapper ' + WRAPPER)
    helper = gate(root)
    versions = {}
    inputs = {}
    for relative in (HELPER, WRAPPER, MODULE):
        path = root / relative
        require(path.is_file() and not path.is_symlink(), 'original MDK lifecycle input missing/indirect: ' + relative)
        inputs[str(path)] = hashlib.sha256(path.read_bytes()).hexdigest()
    require(os.access(root / WRAPPER, os.X_OK), 'original MDK lifecycle wrapper is not executable')
    for version, tag in sorted(realized_versions(entries).items()):
        context = root / 'tests/interop/mdk' / helper.VERSIONS[version][0]
        require((context / 'Dockerfile').is_file(), 'missing original MDK context: ' + str(context))
        for path in sorted(context.rglob('*')):
            if path.is_file():
                inputs[str(path)] = hashlib.sha256(path.read_bytes()).hexdigest()
        versions[version] = {'tag': tag, 'context': str(context), 'digest': helper.digest(context)}
    env = os.environ if environment is None else environment
    docker = shutil.which('docker', path=env.get('PATH'))
    require(docker is not None, 'original MDK lifecycle requires the actual docker executable')
    return {'kind': 'mdk_original_contract_v1', 'source_identity': realization['source_identity'],
            'versions': versions, 'inputs': inputs,
            'tools': {'docker': identity(docker), 'python': identity(sys.executable)},
            'readiness': False, 'complete': False, 'complete_gate_eligible': False,
            'scope': 'original_mdk_images_and_peers_under_outer_pidns_custody_not_parity'}


def _machine_lock_directory():
    locks = Path('/tmp') / ('nostrc-mdk-gate-' + str(os.getuid()))
    locks.mkdir(mode=0o700, exist_ok=True)
    info = locks.lstat()
    require(locks.is_dir() and not locks.is_symlink() and info.st_uid == os.getuid() and not info.st_mode & 0o077,
            'unsafe machine-wide MDK lock directory')
    return locks


@contextlib.contextmanager
def lifecycle(realization, resolved, input_audit, evidence_dir, *, environment=None):
    """Span exactly the selected CTest invocation; always fence and clean up."""
    proof = contract(realization, environment)
    require(resolved.get('runtime_capability_evidence', {}).get(CAPABILITY) == proof,
            'original MDK resolved provisioning contract mismatch')
    require(input_audit.get('passed') is True and input_audit.get('after_build') is True,
            'original MDK lifecycle requires a passing complete post-build input audit')
    for path, sha in proof['inputs'].items():
        require(input_audit.get('input_hashes', {}).get(path) == sha,
                'original MDK input lacks exact full-audit binding: ' + path)
    from test_selection_services import ServiceError, _namespace_resources
    try:
        # The host CMake/CTest tree is owned only inside the outer PID namespace.
        _namespace_resources(os.environ.get('NOSTRC_NATIVE_RESOURCE_ROOT', ''), False)
    except (ServiceError, OSError) as exc:
        raise MDKError('original MDK lifecycle requires live pidns custody: ' + str(exc)) from exc
    root = Path(realization['repo_root'])
    helper = gate(root)
    state = Path(evidence_dir) / ('mdk-' + uuid.uuid4().hex)
    state.mkdir(mode=0o700, parents=False)
    for name in ('containers', 'clients', 'contexts'):
        (state / name).mkdir()
    record = {'kind': 'mdk_original_runtime_v1', 'contract': proof, 'state': str(state), 'status': 'failed',
              'host_process_tree': 'outer_pidns_namespace_exit_seal_required',
              'complete': False, 'complete_gate_eligible': False, 'docker_cleanup_complete': False}
    owner = None
    try:
        real = proof['tools']['docker']['path']
        daemon, endpoint = helper.local_daemon(real)
        images = {version: {'tag': data['tag'], 'context': data['context'], 'digest': data['digest']}
                  for version, data in proof['versions'].items()}
        config = dict(schema_version=1, run_id=state.name, docker=real, docker_executable=proof['tools']['docker'],
                      docker_endpoint=endpoint, daemon_id=daemon, source=str(root), state=str(state), images=images,
                      helper=identity(root / HELPER), wrapper=identity(root / WRAPPER), configure_args=[],
                      execution_domain=EXECUTION_DOMAIN)
        helper.write(state / 'config.json', config)
        owner = helper.OwnedBuilder(config)
        with contextlib.ExitStack() as locks:
            directory = _machine_lock_directory()
            # Selected tags are fixed (including the authored 011 tag): serialize them.
            for version in sorted(images):
                tag = hashlib.sha256(images[version]['tag'].encode()).hexdigest()[:16]
                locks.enter_context(helper.lock(directory / ('tag-' + tag + '.lock'), state / (version + '-waiting.json')))
            for version, data in images.items():
                snapshot = state / 'contexts' / version
                shutil.copytree(data['context'], snapshot)
                require(helper.digest(snapshot) == data['digest'] and helper.digest(Path(data['context'])) == data['digest'],
                        'original MDK context changed while snapshotting')
                data['build_context'] = str(snapshot)
            helper.same_daemon(config)
            owner.setup()
            helper.write(state / 'config.json', config)
            helper.write(state / 'phase.json', {'run_id': state.name, 'phase': 'active'})
            record['builder'] = config['builder']
            yield {'NOSTRC_MDK_STATE': str(state), 'NOSTRC_MDK_PYTHON': proof['tools']['python']['path']}, record
            record['status'] = 'passed'
    except BaseException as exc:
        record['status'] = 'cancelled' if isinstance(exc, KeyboardInterrupt) else 'failed'
        record['error'] = type(exc).__name__ + ': ' + str(exc)
        raise
    finally:
        try:
            if owner is not None:
                result = owner.cleanup()
                record['docker_lifecycle'] = result
                record['docker_cleanup_complete'] = result['cleanup_complete']
            else:
                record['docker_cleanup_complete'] = True  # nothing allocated before identity/state
            errors = []
            record['images'] = helper.retained_records(state.glob('*-image.json'), errors)
            record['containers'] = helper.retained_records((state / 'containers').glob('*.json'), errors)
            record['journal_errors'] = errors
            if not (record['docker_cleanup_complete'] and not errors
                    and all(c.get('removed') for c in record['containers'])):
                record['status'] = 'failed' if record['status'] == 'passed' else record['status']
            if contract(realization, environment) != proof:
                record['status'] = 'failed'
                record.setdefault('error', 'original MDK inputs/tools changed during execution')
        finally:
            with (Path(evidence_dir) / 'mdk-runtime.json').open('x') as stream:
                json.dump(record, stream, sort_keys=True, indent=2)
                stream.write('\n')
        if record['status'] != 'passed' and sys.exc_info()[0] is None:
            raise MDKError('original MDK lifecycle cleanup/identity failed: ' + str(state))


def recover_lane(evidence_root, helper, log_dir):
    """Outer adapter, after namespace exit: replay cleanup for unclosed selected journals.

    Returns retained records; any journal not cleanly closed by the in-namespace
    lifecycle fails the lane even when recovery then removes every resource.
    """
    import subprocess
    records = []
    for config_path in sorted(Path(evidence_root).rglob('mdk-*/config.json')):
        state = config_path.parent
        config = json.loads(config_path.read_text())
        if config.get('execution_domain') != EXECUTION_DOMAIN or config.get('state') != str(state):
            continue
        phase = json.loads((state / 'phase.json').read_text()) if (state / 'phase.json').exists() else None
        builder = json.loads((state / 'builder.json').read_text()) if (state / 'builder.json').exists() else None
        closed = phase == {'run_id': state.name, 'phase': 'closed'} and bool(builder and builder.get('cleanup_complete'))
        record = {'state': str(state), 'closed_in_namespace': closed}
        if not closed:
            log = Path(log_dir) / ('mdk-recover-' + state.name + '.log')
            with log.open('xb') as stream:
                result = subprocess.run([sys.executable, str(helper), 'recover', '--state', str(state)],
                                        stdout=stream, stderr=subprocess.STDOUT, timeout=600)
            record.update(recovery_exit=result.returncode, log=str(log))
        records.append(record)
    return records


def _validate_contract_shape(proof, identity_value):
    require(isinstance(proof, dict) and proof.get('kind') == 'mdk_original_contract_v1'
            and proof.get('source_identity') == identity_value and proof.get('readiness') is False
            and proof.get('complete') is False and proof.get('complete_gate_eligible') is False
            and proof.get('scope') == 'original_mdk_images_and_peers_under_outer_pidns_custody_not_parity',
            'original MDK contract missing or promoted')
    versions = proof.get('versions', {})
    require(versions and set(versions) <= {'08', '09', '011'}
            and all(re.fullmatch(r'[0-9a-f]{64}', v.get('digest', '')) and TAG.fullmatch(v.get('tag', ''))
                    for v in versions.values()), 'original MDK versions/contexts unbound')
    require(all(v['tag'] == FIXED_TAGS[k] for k, v in versions.items() if k in FIXED_TAGS),
            'original MDK fixed tag changed')
    require(set(proof.get('tools', {})) == {'docker', 'python'}
            and all(re.fullmatch(r'[0-9a-f]{64}', t.get('sha256', '')) and Path(t.get('path', '')).is_absolute()
                    for t in proof['tools'].values()), 'original MDK tool bindings missing')


def validate_runtime(backend, plan, lane, profile):
    """Aggregation: typed contract + runtime state only; never raw run/images/prepared receipts."""
    required = (profile.get('execution_domain') == EXECUTION_DOMAIN
                or CAPABILITY in backend.get('required_capabilities', []))
    state = backend.get('mdk_runtime')
    if not required:
        require(state is None, 'undeclared original MDK runtime')
        return
    require(profile.get('execution_domain') == EXECUTION_DOMAIN and CAPABILITY in profile.get('capabilities', []),
            'original MDK runtime on an unauthored profile')
    proof = backend.get('runtime_capability_evidence', {}).get(CAPABILITY)
    _validate_contract_shape(proof, plan['source_identity'])
    audit = backend.get('input_audit_details', backend.get('input_audit'))
    require(isinstance(audit, dict) and audit.get('passed') is True and audit.get('after_build') is True
            and all(audit.get('input_hashes', {}).get(p) == s for p, s in proof['inputs'].items()),
            'original MDK full post-build audit binding missing')
    require(backend.get('custody_mode', lane.get('custody_mode')) == 'pidns' or lane.get('custody_mode') == 'pidns',
            'original MDK runtime requires outer pidns custody')
    require(isinstance(state, dict) and state.get('kind') == 'mdk_original_runtime_v1'
            and state.get('contract') == proof and state.get('status') == 'passed'
            and state.get('docker_cleanup_complete') is True and not state.get('journal_errors')
            and state.get('host_process_tree') == 'outer_pidns_namespace_exit_seal_required'
            and state.get('complete') is False and state.get('complete_gate_eligible') is False,
            'original MDK runtime/cleanup receipt missing or failed')
    lifecycle_result = state.get('docker_lifecycle', {})
    require(lifecycle_result.get('scope') == 'original_docker_build_and_recorded_peers_only'
            and lifecycle_result.get('cleanup_complete') is True and not lifecycle_result.get('errors'),
            'original MDK Docker cleanup unverified')
    require(all(c.get('removed') is True and re.fullmatch(r'[0-9a-f]{64}', c.get('container_id', ''))
                for c in state.get('containers', [])), 'original MDK peer CID removal unproved')
    for image in state.get('images', []):
        require(image.get('digest') == proof['versions'].get(image.get('version'), {}).get('digest')
                and (not image.get('build_complete') or re.fullmatch(r'sha256:[0-9a-f]{64}', image.get('image_id', ''))),
                'original MDK image identity differs from the bound context')
