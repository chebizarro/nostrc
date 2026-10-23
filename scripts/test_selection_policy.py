#!/usr/bin/env python3
"""Owned provisioned policy workload. Never native-profile or image-build parity."""
import argparse
import contextlib
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import time
import uuid

CAPABILITY = 'namespace_policy_controller'
INPUTS = ('scripts/test_selection_policy.py', 'tests/selection/namespace-testhost.Dockerfile',
          'tests/selection/namespace_policy_entry.sh', 'tests/selection/namespace_testhost_lifecycle.py',
          'tests/selection/namespace_host_probe.py', 'scripts/adapter_namespace_custody.py')
REQUIRED = {'environment', 'mapping-probe', 'kernel-prerequisite', 'kernel-matrix', 'policy-suite',
            'unsequenced', 'gui-frame', 'linux-packages', 'pre-push', 'full-gate', 'linux-smoke',
            'sanitizer', 'signer-names', 'keychain-guard'}
ENTRY_COMMAND = ('mkdir -p "$HOME"; git config --global --add safe.directory /source; '
                 'exec bash tests/selection/namespace_policy_entry.sh')


class PolicyError(ValueError):
    pass


def require(condition, message):
    if not condition:
        raise PolicyError(message)


def hashed(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def write(path, value):
    Path(path).write_text(json.dumps(value, sort_keys=True, indent=2) + '\n')


def tool(name, env=None):
    value = shutil.which(name, path=(env or os.environ).get('PATH'))
    require(value is not None, 'policy controller executable unavailable: ' + name)
    path = Path(value).resolve(strict=True)
    return dict(path=str(path), sha256=hashed(path))


def run(argv, log=None, timeout=60):
    # No shell, daemon-wide cleanup, process-name or host numeric-PID actions.
    start = time.time()
    process = subprocess.run([str(x) for x in argv], capture_output=True, timeout=timeout)
    if log:
        Path(log).write_bytes(process.stdout + process.stderr)
        write(str(log) + '.process.json', dict(argv=[str(x) for x in argv],
            exit_code=process.returncode, started=start, finished=time.time()))
    require(process.returncode == 0, 'policy controller command failed: ' + str(argv[0:2]))
    return process.stdout.decode().strip()


def source_inputs(root):
    result = {}
    for name in INPUTS:
        path = root / name
        require(path.is_file() and not path.is_symlink(), 'policy source missing/indirect: ' + name)
        result[name] = hashed(path)
    return result


def prepare_image(root, output):
    root, output = root.resolve(), output.resolve()
    require(not output.exists(), 'policy image setup already exists; retain the previous attempt')
    output.parent.mkdir(parents=True, exist_ok=True)
    directory = output.parent / ('policy-image-' + uuid.uuid4().hex)
    directory.mkdir(mode=0o700)
    tools = {name: tool(name) for name in ('docker', 'git')}
    tools['python'] = tool(sys.executable)
    inputs = source_inputs(root)
    sha = run([tools['git']['path'], '-C', root, 'rev-parse', 'HEAD'])
    require(not run([tools['git']['path'], '-C', root, 'status', '--porcelain', '--untracked-files=no']),
            'policy setup refuses dirty tracked source')
    iid = directory / 'image.id'
    argv = [tools['docker']['path'], 'build', '--progress', 'plain', '--iidfile', str(iid),
            '--label', 'nostrc.policy.recipe=' + inputs[INPUTS[1]],
            '--label', 'nostrc.policy.source=' + sha, '-f', str(root / INPUTS[1]), str(root / 'tests/selection')]
    state = dict(kind='policy_testhost_setup_v1', source_sha=sha, inputs=inputs, tools=tools,
                 argv=argv, log=str(directory / 'image-build.log'), passed=False,
                 complete=False, complete_gate_eligible=False,
                 scope='provisioned_policy_image_not_native_or_builder_cleanup')
    try:
        run(argv, directory / 'image-build.log', timeout=1800)
        image = iid.read_text().strip()
        require(re.fullmatch('sha256:[0-9a-f]{64}', image), 'invalid policy image identity')
        observed = json.loads(run([tools['docker']['path'], 'image', 'inspect', image]))[0]
        state.update(image=image, image_inspect=observed, passed=True)
    finally:
        write(output, state)


def contract(realization, env):
    profile = realization['profile']
    require(realization['profile_id'] == 'static.policy' and profile.get('execution_domain') == 'policy_controller_linux_v1',
            'policy controller requires its explicit authored execution domain')
    require(sys.platform == 'linux' and profile['platform'] == 'linux'
            and profile['architecture'] == os.uname().machine == 'x86_64',
            'policy controller requires actual Linux/x86_64; no architecture alias')
    root = Path(realization['repo_root'])
    setup_path = Path(env.get('RUNNER_TEMP', '/nonexistent')) / 'dependency/policy-testhost-setup.json'
    setup = json.loads(setup_path.read_text())
    require(setup.get('kind') == 'policy_testhost_setup_v1' and setup.get('passed') is True,
            'policy testhost requires actual image producer evidence')
    require(setup['source_sha'] == realization['source_identity'].get('sha')
            and setup['inputs'] == source_inputs(root), 'policy image producer source differs')
    tools = {name: tool(name, env) for name in ('docker', 'git')}
    tools['python'] = tool(sys.executable)
    require(tools == setup['tools'], 'policy producer/controller tools differ')
    require(re.fullmatch('sha256:[0-9a-f]{64}', setup['image']), 'invalid immutable policy image')
    observed = json.loads(run([tools['docker']['path'], 'image', 'inspect', setup['image']]))[0]
    require(observed == setup['image_inspect'] and observed['Os'] == 'linux' and observed['Architecture'] == 'amd64',
            'policy image readback/platform differs')
    require(observed['Config']['Labels']['nostrc.policy.recipe'] == setup['inputs'][INPUTS[1]]
            and observed['Config']['Labels']['nostrc.policy.source'] == setup['source_sha'],
            'policy image lacks exact source/recipe producer labels')
    status_path = Path(setup['log'] + '.process.json')
    status = json.loads(status_path.read_text())
    expected = [tools['docker']['path'], 'build', '--progress', 'plain', '--iidfile',
        str(Path(setup['log']).parent / 'image.id'), '--label', 'nostrc.policy.recipe=' + setup['inputs'][INPUTS[1]],
        '--label', 'nostrc.policy.source=' + setup['source_sha'], '-f', str(root / INPUTS[1]), str(root / 'tests/selection')]
    require(status['argv'] == setup['argv'] == expected and status['exit_code'] == 0
            and Path(setup['log']).is_file(), 'policy physical image producer evidence differs')
    return dict(kind='policy_controller_contract_v1', source_identity=realization['source_identity'],
        inputs=setup['inputs'], tools=tools, image=setup['image'],
        setup_inputs={str(setup_path): hashed(setup_path), str(status_path): hashed(status_path),
                      setup['log']: hashed(setup['log'])}, readiness=False,
        scope='authored_provisioned_policy_workload_not_native_profile')


def clone_source(root, destination, git, logroot):
    # A worktree's .git pointer cannot be imported into the container. Materialize
    # independent local Git objects, never shared alternates or a writable host gitdir.
    log = logroot / ('clone-' + hashlib.sha256(os.fsencode(root)).hexdigest()[:16] + '.log')
    run([git, 'clone', '--no-local', '--no-hardlinks', '--no-checkout', '--', root, destination], log)
    sha = run([git, '-C', root, 'rev-parse', 'HEAD'])
    run([git, '-c', 'core.hooksPath=/dev/null', '-C', destination, 'checkout', '--detach', sha])
    manifest = {}
    records = subprocess.check_output([git, '-C', str(root), 'ls-files', '--stage', '-z']).split(b'\0')
    for record in records:
        if not record:
            continue
        meta, raw = record.split(b'\t', 1)
        mode, oid, stage = meta.decode().split()
        name = os.fsdecode(raw)
        require(stage == '0', 'unmerged policy source')
        path, copied = root / name, destination / name
        if mode == '160000':
            require(path.is_dir() and run([git, '-C', path, 'rev-parse', 'HEAD']) == oid,
                    'uninitialized/mismatched policy source submodule: ' + name)
            manifest[name] = dict(mode=mode, oid=oid, files=clone_source(path, copied, git, logroot))
        else:
            require(path.is_symlink() == copied.is_symlink(), 'policy snapshot file kind differs')
            original = os.fsencode(os.readlink(path)) if path.is_symlink() else path.read_bytes()
            duplicate = os.fsencode(os.readlink(copied)) if copied.is_symlink() else copied.read_bytes()
            require(original == duplicate and (path.lstat().st_mode & 0o111) == (copied.lstat().st_mode & 0o111),
                    'policy snapshot bytes/mode differ: ' + name)
            manifest[name] = dict(mode=mode, sha256=hashlib.sha256(original).hexdigest())
    return manifest


@contextlib.contextmanager
def controller(realization, resolved, spec, audit):
    from test_selection_realization import phase_environment
    env = phase_environment(realization['profile'], realization['repo_root'], 'test')
    proof = contract(realization, env)
    require(resolved['runtime_capability_evidence'].get(CAPABILITY) == proof,
            'policy resolved executable/image contract differs')
    expected = [spec['argv'][0], str(Path(realization['repo_root']) / INPUTS[0]), 'attach',
                '--control', str(Path(realization['build_dir']) / 'namespace-policy/control')]
    require(Path(spec['argv'][0]).resolve() == Path(proof['tools']['python']['path'])
            and spec['argv'] == expected and spec['requires'] == [CAPABILITY]
            and audit.get('passed') and audit.get('after_build'), 'policy command requires exact argv and full post-build source audit')
    for name in INPUTS:
        path = str(Path(realization['repo_root']) / name)
        require(audit.get('input_hashes', {}).get(path) == proof['inputs'][name], 'policy source lacks full input audit: ' + name)
    directory = Path(realization['build_dir']) / 'namespace-policy'
    directory.mkdir(mode=0o700)
    control, evidence = directory / 'control', directory / 'workload'
    control.mkdir(mode=0o700); evidence.mkdir(mode=0o777); evidence.chmod(0o777)
    state = dict(kind='authored_policy_controller_v1', contract=proof, status='failed',
                 scope='provisioned_policy_workload_not_native_profile', complete=False,
                 complete_gate_eligible=False, cleanup_complete=False)
    owner = None
    try:
        manifest = clone_source(Path(realization['repo_root']), directory / 'source', proof['tools']['git']['path'], control)
        write(directory / 'source-manifest.json', manifest)
        state['source_manifest_sha256'] = hashed(directory / 'source-manifest.json')
        (control / 'image.id').write_text(proof['image'] + '\n')
        import runpy
        lifecycle = runpy.run_path(str(Path(realization['repo_root']) / INPUTS[3]))
        lifecycle['prepare'](control)
        owner = json.loads((control / 'owner.json').read_text())
        docker = proof['tools']['docker']['path']
        argv = [docker, 'create', '--cidfile', str(control / 'container.cid'), '--name', owner['name'],
            '--label', 'nostrc.namespace-policy=' + owner['token'], '--user', '1001:1001', '--cap-drop=ALL',
            '--security-opt=no-new-privileges', '--security-opt=seccomp=unconfined',
            '--security-opt=systempaths=unconfined', '--security-opt=apparmor=unconfined',
            '--network', 'none', '--read-only', '--tmpfs', '/tmp:rw,exec,nosuid,nodev,mode=1777',
            '--tmpfs', '/tmp/.X11-unix:rw,nosuid,nodev,mode=1777',
            '--mount', f'type=bind,source={directory / "source"},target=/source,readonly',
            '--mount', f'type=bind,source={evidence},target=/evidence',
            '-e', 'HOME=/tmp/namespace-home', '-e', 'NOSTRC_CUSTODY_TEST_EVIDENCE=/evidence/custody',
            '--entrypoint', '/bin/bash', proof['image'], '-c', ENTRY_COMMAND]
        run(argv, control / 'create.log')
        cid = (control / 'container.cid').read_text().strip()
        require(re.fullmatch('[0-9a-f]{64}', cid), 'invalid owned policy CID')
        before = json.loads(run([docker, 'container', 'inspect', cid]))[0]
        write(control / 'container-before.json', before)
        host = before['HostConfig']
        binds = {item['Destination']: item for item in before['Mounts'] if item['Type'] == 'bind'}
        require(before['Id'] == cid and before['Image'] == proof['image']
                and before['Config']['User'] == '1001:1001' and not before['Config'].get('Volumes')
                and before['Config']['Labels'].get('nostrc.namespace-policy') == owner['token']
                and before['Name'] == '/' + owner['name'] and host['Privileged'] is False
                and not host['CapAdd'] and host['CapDrop'] == ['ALL']
                and host['NetworkMode'] == 'none' and host['PidMode'] != 'host'
                and host['ReadonlyRootfs'] is True and not host.get('Binds')
                and set(binds) == {'/source', '/evidence'}
                and binds['/source']['Source'] == str(directory / 'source') and binds['/source']['RW'] is False
                and binds['/evidence']['Source'] == str(evidence) and binds['/evidence']['RW'] is True
                and all(item['Type'] in {'bind', 'tmpfs'} for item in before['Mounts'])
                and set(host['Tmpfs']) == {'/tmp', '/tmp/.X11-unix'}
                and set(host['SecurityOpt']) == {'no-new-privileges', 'seccomp=unconfined',
                                               'systempaths=unconfined', 'apparmor=unconfined'},
                'policy container effective isolation/mounts differ before target launch')
        write(control / 'controller.json', dict(docker=proof['tools']['docker'], cid=cid))
        state['create_argv'] = argv
        yield state
        observed = json.loads(run([docker, 'container', 'inspect', cid]))[0]
        write(control / 'container-after.json', observed)
        require(all(observed[key] == before[key] for key in ('Id', 'Image', 'Name', 'Config', 'HostConfig', 'Mounts'))
                and observed['State']['Running'] is False and observed['State']['ExitCode'] == 0,
                'policy container did not physically exit successfully')
        workload = json.loads((evidence / 'regression.json').read_text())
        require(workload['source_sha'] == proof['source_identity']['sha'] and workload['passed'] is True
                and set(workload['commands']) == REQUIRED and all(type(v) is int and v == 0 for v in workload['commands'].values())
                and workload['kernel_matrix']['tests'] >= 79 and workload['kernel_matrix']['skipped'] == 0
                and workload['native_profile_acceptance'] is False and workload['complete'] is False,
                'policy complete unchanged workload/kernel evidence missing or failed')
        require(set(workload['tools']) == {'python3', 'git', 'cc', 'cmake', 'ninja', 'Xvfb', 'dbus-daemon', 'gnome-keyring-daemon'}
                and all(re.fullmatch('[0-9a-f]{64}', t['sha256']) and Path(t['path']).is_absolute()
                        for t in workload['tools'].values()), 'policy container tool bindings missing')
        state.update(status='passed', workload=workload)
    finally:
        # Parent-owned cleanup runs even when the command is nonzero/cancelled.
        if owner is not None:
            try:
                require(json.loads((control / 'owner.json').read_text()) == owner, 'policy ownership journal changed')
                require(lifecycle['cleanup'](control) == 0, 'owned policy container cleanup unverified')
                state['cleanup_complete'] = True
                state['cleanup'] = json.loads((control / 'cleanup.json').read_text())
            except BaseException:
                state['status'] = 'failed'
                raise
            finally:
                write(directory / 'receipt.json', state)
        else:
            write(directory / 'receipt.json', state)
        require(contract(realization, env) == proof, 'policy source/tool/image inputs changed during execution')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('operation', choices=('prepare-image', 'attach'))
    parser.add_argument('--source', type=Path)
    parser.add_argument('--output', type=Path)
    parser.add_argument('--control', type=Path)
    args = parser.parse_args()
    if args.operation == 'prepare-image':
        require(args.source is not None and args.output is not None, 'prepare-image requires source/output')
        prepare_image(args.source, args.output)
        return 0
    require(args.control is not None, 'attach requires parent-owned control')
    value = json.loads((args.control / 'controller.json').read_text())
    require(re.fullmatch('[0-9a-f]{64}', value['cid']) and hashed(value['docker']['path']) == value['docker']['sha256'],
            'policy attach executable/CID differs from parent')
    return subprocess.call([value['docker']['path'], 'start', '--attach', value['cid']])


if __name__ == '__main__':
    try:
        sys.exit(main())
    except (PolicyError, OSError, ValueError, KeyError, subprocess.SubprocessError) as exc:
        print('policy controller: ' + str(exc), file=sys.stderr)
        sys.exit(1)
