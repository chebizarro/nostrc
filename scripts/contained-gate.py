#!/usr/bin/env python3
"""Explicit prepared-peer Linux custody. No legacy Docker-build/native claims."""
import argparse
import fcntl
import json
import os
from pathlib import Path
import shutil
import stat
import subprocess
import sys
import uuid

from test_selection_custody import (Cancel, ContainerOwner, CustodyError, canonical,
    context_hash, digest, file_hash, immutable, write_json)

PEERS = {
    "08": ("driver", "mdk-interop-driver", "1.93"),
    "09": ("driver-0.9-probe", "mdk09-probe-driver", "1.90.0"),
    "011": ("driver-0.11", "mdk011-interop-driver", "1.97.1"),
}


def git(source, *argv):
    return subprocess.check_output(["git", "-c", "maintenance.autoDetach=false", "-c", "gc.autoDetach=false",
                                    "-C", str(source), *argv])


def source_identity(source):
    return {"kind": "committed", "sha": git(source, "rev-parse", "HEAD").decode().strip(),
            "gitlinks": [x for x in git(source, "ls-tree", "-r", "HEAD").decode().splitlines()
                         if x.startswith("160000 ")]}


def peer_source(source, version):
    directory = source / "tests/interop/mdk" / PEERS[version][0]
    relative = directory.relative_to(source).as_posix()
    if git(source, "diff", "HEAD", "--", relative):
        raise CustodyError("prepared peers require exact committed source context")
    tracked = set(git(source, "ls-files", "--", relative).decode().splitlines())
    actual = {p.relative_to(source).as_posix() for p in directory.rglob("*") if p.is_file()}
    if tracked != actual or not (directory / "Cargo.lock").is_file():
        raise CustodyError("peer context has missing/untracked source inputs")
    return directory


def fixed_lock(version):
    root = Path("/tmp") / ("nostrc-mdk-gate-" + str(os.getuid()))
    root.mkdir(mode=0o700, exist_ok=True)
    info = root.lstat()
    if not stat.S_ISDIR(info.st_mode) or info.st_uid != os.getuid() or info.st_mode & 0o077:
        raise CustodyError("unsafe machine-wide peer lock directory")
    fd = os.open(root / (version + ".lock"), os.O_CREAT | os.O_RDWR | os.O_NOFOLLOW, 0o600)
    info = os.fstat(fd)
    if not stat.S_ISREG(info.st_mode) or info.st_uid != os.getuid() or info.st_mode & 0o077:
        os.close(fd)
        raise CustodyError("unsafe machine-wide peer lock")
    return fd


def extract_toolchain(owner, image, destination):
    """Copy bytes from a stopped immutable image; never execute its entrypoint."""
    owner.create(image, ["/bin/true"], workdir="/tmp")
    owner.rpc("cp", owner.cid + ":/usr/local/rustup", str(destination))
    receipt = owner.finalize()
    if not receipt["cleanup_complete"]:
        raise CustodyError("toolchain extraction container cleanup unverified")
    matches = list((destination / "toolchains").glob("*/bin/cargo"))
    if len(matches) != 1:
        raise CustodyError("prepared Rust image must contain one explicit toolchain")
    toolchain = matches[0].parent.relative_to(destination).as_posix()
    return "/rustup/" + toolchain


def build_recipe(version):
    command = "set -eu; mkdir -p /work/home /work/cargo; cd /work/driver-source; "
    command += "rustc --version; cargo --version; git --version; make --version; perl -e 'use FindBin;'; pkg-config --version; "
    command += "cargo build --release --locked; "
    if version in {"09", "011"}:
        command += "cargo test --release --locked; "
    command += "cp /work/target/release/" + PEERS[version][1] + " /work/driver"
    return ["/bin/sh", "-c", command]


def prepare(args):
    source = Path(args.source_root).resolve(strict=True)
    evidence = Path(args.evidence_dir).resolve()
    evidence.mkdir(parents=True, exist_ok=True)
    state = evidence / uuid.uuid4().hex
    state.mkdir(mode=0o700)
    version = args.version
    identity = source_identity(source)
    context = peer_source(source, version)
    before = context_hash(context)
    work = state / "work"
    work.mkdir()
    shutil.copytree(context, work / "driver-source")
    cancel = Cancel()
    record = {"schema_version": 1, "kind": "prepared_peer_v1", "version": version,
              "source_identity": identity, "source_context": context.relative_to(source).as_posix(),
              "context_sha256": before, "cargo_lock_sha256": file_hash(context / "Cargo.lock"),
              "builder_image_id": immutable(args.builder_image), "rust_image_id": immutable(args.rust_image),
              "implementation": {name: file_hash(Path(__file__).parent / name) for name in
                                 ('contained-gate.py', 'test_selection_custody.py')},
              "status": "failed", "complete": False, "cleanup_complete": False}
    fd = None
    try:
        with cancel.handlers():
            if version in {"08", "09"}:
                fd = fixed_lock(version)
                # Interruptible lock wait; never substitute a delay for acquisition.
                while True:
                    try:
                        fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
                        break
                    except BlockingIOError:
                        if cancel.requested.wait(0.1):
                            raise InterruptedError("cancelled while waiting for fixed peer lock")
            extraction = ContainerOwner(state / "toolchain-owner", cancel=cancel)
            try:
                bin_dir = extract_toolchain(extraction, args.rust_image, state / "rustup")
            finally:
                if not (extraction.directory / "custody.json").exists():
                    extraction.finalize()
            write_json(state / "preparation.json", record)
            builder = ContainerOwner(state / "builder", cancel=cancel)
            environment = {"HOME": "/work/home", "CARGO_HOME": "/work/cargo", "CARGO_TARGET_DIR": "/work/target",
                           "PATH": bin_dir + ":/usr/bin:/bin", "RUSTC": bin_dir + "/rustc", "RUSTDOC": bin_dir + "/rustdoc",
                           "CARGO_NET_GIT_FETCH_WITH_CLI": "true", "CARGO_BUILD_JOBS": "2"}
            record["rust_bin_dir"] = bin_dir
            if not Path(bin_dir).parent.name.startswith(PEERS[version][2]):
                raise CustodyError("Rust image toolchain version differs from locked driver recipe")
            code = builder.run(args.builder_image, build_recipe(version),
                timeout=args.timeout, network="bridge", mounts=[(work, "/work", False), (state / "rustup", "/rustup", True)],
                environment=environment, binding=record)
            record["builder_custody"] = builder.receipt
            record["toolchain_custody"] = extraction.receipt
            record["exit_code"] = code
            record['source_identity_after'] = source_identity(source)
            if context_hash(context) != before or context_hash(work / "driver-source") != before:
                raise CustodyError("source context changed during preparation")
            if code or not builder.receipt["cleanup_complete"]:
                raise CustodyError("peer build/parity/custody did not succeed")
            binary = state / "driver"
            if not stat.S_ISREG((work / "driver").lstat().st_mode):
                raise CustodyError("prepared binary must be a regular non-symlink file")
            shutil.copyfile(work / "driver", binary)
            binary.chmod(0o555)
            record.update(binary=str(binary), binary_sha256=file_hash(binary),
                          rustc_sha256=file_hash(state / "rustup" / bin_dir.removeprefix("/rustup/") / "rustc"),
                          status="succeeded", cleanup_complete=True, complete=True)
    except (OSError, ValueError, CustodyError, subprocess.SubprocessError) as exc:
        record["error"] = str(exc)
        if cancel.requested.is_set():
            record["status"] = "cancelled"
    finally:
        write_json(state / "peer.json", record)
        if fd is not None:
            os.close(fd)
    print(state / "peer.json", flush=True)
    return 0 if record["complete"] else 1


def snapshot(source, destination, sha, gitlink_sources=None, prefix=''):
    """Independent shallow Git objects, including exact nested gitlinks."""
    subprocess.run(['git', 'init', '--quiet', str(destination)], check=True)
    subprocess.run(['git', '-c', 'core.hooksPath=/dev/null', '-C', str(destination),
                    'fetch', '--quiet', '--depth=1', str(source), sha], check=True)
    subprocess.run(['git', '-c', 'core.hooksPath=/dev/null', '-C', str(destination),
                    'checkout', '--quiet', '--detach', sha], check=True)
    links = {}
    for line in git(destination, 'ls-tree', '-r', 'HEAD').decode().splitlines():
        if not line.startswith('160000 '):
            continue
        metadata, relative = line.split('\t', 1)
        commit = metadata.split()[2]
        explicit = (gitlink_sources or {}).get(prefix + relative)
        local = Path(explicit) if explicit is not None else Path(source) / relative
        if explicit is None and not (local / '.git').exists():
            raw = git(source, 'rev-parse', '--git-path', 'modules/' + relative).decode().strip()
            local = Path(raw) if Path(raw).is_absolute() else Path(source) / raw
        if not local.exists():
            raise CustodyError('exact gitlink object store unavailable: ' + prefix + relative)
        links[relative] = commit
        nested = snapshot(local.resolve(), destination / relative, commit, gitlink_sources, prefix + relative + '/')
        links.update({relative + '/' + key: value for key, value in nested.items()})
    return links


def validate_peer(path, source, expected):
    path = Path(path).resolve(strict=True)
    record = json.loads(path.read_text())
    parent = path.parent.lstat()
    if parent.st_uid != os.getuid() or parent.st_mode & 0o077:
        raise CustodyError('peer receipt requires private operator-owned preparation state')
    version = record['version']
    if version not in expected or record.get('kind') != 'prepared_peer_v1' or record.get('complete') is not True:
        raise CustodyError('unselected/incomplete prepared peer')
    context = peer_source(source, version)
    if record['context_sha256'] != context_hash(context) or record['cargo_lock_sha256'] != file_hash(context / 'Cargo.lock'):
        raise CustodyError('prepared source context/lock identity differs')
    from test_selection_custody import verify_final_receipt
    builder = record['builder_custody']
    verify_final_receipt(builder, builder['binding'])
    for key in ('version', 'context_sha256', 'cargo_lock_sha256', 'builder_image_id', 'rust_image_id', 'source_identity'):
        if builder['binding'][key] != record[key]:
            raise CustodyError('prepared build binding differs: ' + key)
    if (builder['image_id'] != record['builder_image_id'] or builder['network'] != 'bridge'
            or builder['argv'] != build_recipe(version)):
        raise CustodyError('prepared builder domain differs')
    extraction = record['toolchain_custody']
    if (not extraction.get('cleanup_complete') or extraction.get('rpc_indeterminate')
            or extraction.get('image_id') != record['rust_image_id']
            or extraction.get('container_id') != extraction.get('removed_container_id')):
        raise CustodyError('prepared Rust image extraction not finalized')
    binary = Path(record['binary'])
    if not stat.S_ISREG(binary.lstat().st_mode) or binary.parent.resolve() != path.parent:
        raise CustodyError('prepared binary is not the owned regular artifact')
    if file_hash(binary) != record['binary_sha256']:
        raise CustodyError('prepared binary identity differs')
    return record, file_hash(path)


def work_file(work, relative):
    path = Path(work) / relative
    if (path.is_symlink() or not stat.S_ISREG(path.lstat().st_mode)
            or not path.resolve().is_relative_to(Path(work).resolve())):
        raise CustodyError('runtime evidence must be a regular file inside owned work')
    return path


def work_json(work, relative):
    path = work_file(work, relative)
    if path.stat().st_size > 128 * 1024 * 1024:
        raise CustodyError('runtime evidence JSON exceeds bounded admission size')
    return json.loads(path.read_text())


def run_lane(args):
    import contextlib
    import test_selection as selection
    from test_selection_custody import validate_lifecycle, attest_retained_evidence
    source = Path(args.source_root).resolve(strict=True)
    identity = source_identity(source)
    if git(source, 'diff', 'HEAD', '--'):
        raise CustodyError('contained execution requires exact committed source')
    policy = selection.load_inventory(source / args.inventory)
    plan = selection.read_json(args.plan)
    selection.validate_plan(plan, policy)
    if plan['source_identity'] != {'kind': 'committed', 'sha': identity['sha']}:
        raise CustodyError('plan source differs from exact committed checkout')
    profile = policy['profiles'][args.profile]
    if profile.get('execution_domain') != 'prepared_peer_linux_v1' or args.profile not in plan['expected_lane_ids']:
        raise CustodyError('distinct selected prepared-peer profile required')
    versions = profile.get('prepared_peers')
    if not isinstance(versions, list) or len(versions) != len(set(versions)) or set(versions) - set(PEERS):
        raise CustodyError('invalid prepared peer projection')
    evidence = Path(args.evidence_dir).resolve()
    evidence.mkdir(parents=True, exist_ok=True)
    state = evidence / uuid.uuid4().hex
    state.mkdir(mode=0o700)
    work, control, peers = [state / key for key in ('work', 'control', 'peers')]
    for directory in (work, control, peers):
        directory.mkdir()
    cancel = Cancel()
    normalized = {'schema_version': 1, 'status': 'failed', 'diagnostic_eligible': False,
                  'complete_gate_eligible': False, 'complete': False}
    code = 1
    with cancel.handlers(), contextlib.ExitStack() as locks:
        for version in sorted(set(versions) & {'08', '09'}):
            fd = fixed_lock(version)
            locks.callback(os.close, fd)
            while True:
                try:
                    fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
                    break
                except BlockingIOError:
                    write_json(state / (version + '-waiting.json'), {'status': 'waiting', 'version': version})
                    if cancel.requested.wait(0.1):
                        raise CustodyError('cancelled while waiting for fixed peer lifecycle lock')
        selected = {}
        for receipt_path in args.peer_receipt:
            peer, receipt_hash = validate_peer(receipt_path, source, versions)
            version = peer['version']
            if version in selected:
                raise CustodyError('duplicate prepared peer')
            target = peers / version
            target.mkdir()
            shutil.copyfile(peer['binary'], target / 'driver')
            (target / 'driver').chmod(0o555)
            selected[version] = {'binary_sha256': peer['binary_sha256'], 'context_sha256': peer['context_sha256'],
                                 'preparation_receipt_sha256': receipt_hash, 'builder_image_id': peer['builder_image_id'],
                                 'rust_image_id': peer['rust_image_id']}
        if set(selected) != set(versions):
            raise CustodyError('missing selected prepared peers')
        overrides = {'08': 'MDK_INTEROP_DRIVER', '09': 'MDK09_PROBE_DRIVER', '011': 'MDK011_INTEROP_DRIVER'}
        for version in versions:
            if '-D' + overrides[version] + ':STRING=/peers/' + version + '/driver' not in profile['configure_args']:
                raise CustodyError('prepared native driver override absent from exact profile')
        gitlink_sources = {}
        for item in args.gitlink_source:
            name, location = item.split('=', 1)
            if name in gitlink_sources or not name or name.startswith('/') or '..' in Path(name).parts:
                raise CustodyError('invalid/duplicate explicit gitlink source')
            gitlink_sources[name] = Path(location).resolve(strict=True)
        links = snapshot(source, state / 'source', identity['sha'], gitlink_sources)
        if set(gitlink_sources) - set(links):
            raise CustodyError('unclaimed explicit gitlink source')
        if selection.load_inventory(state / 'source' / args.inventory) != policy:
            raise CustodyError('snapshotted inventory differs')
        binding = {'source_identity': plan['source_identity'], 'source_gitlinks': links,
                   'profile_id': args.profile, 'profile_hash': selection.digest(profile),
                   'inventory_hash': selection.digest(policy), 'plan_id': plan['plan_id'], 'peers': selected}
        write_json(control / 'inventory.json', policy)
        write_json(control / 'plan.json', plan)
        write_json(control / 'peers.json', selected)
        owner = ContainerOwner(state / 'owner', cancel=cancel)
        def admitted(created):
            write_json(control / 'run.json', {'binding': binding, 'profile': profile, 'peers': selected,
                       'owner': created.receipt, 'container_id': created.cid, 'timeout': args.timeout})
        environment = {'HOME': '/tmp/home', 'PYTHONDONTWRITEBYTECODE': '1'}
        # Docker Desktop can present read-only Git objects with remapped owners.
        # Trust only this inspected source and exact nested gitlinks, per process.
        safe = ['/source'] + ['/source/' + name for name in sorted(links)]
        environment['GIT_CONFIG_COUNT'] = str(len(safe))
        for index, directory in enumerate(safe):
            environment['GIT_CONFIG_KEY_' + str(index)] = 'safe.directory'
            environment['GIT_CONFIG_VALUE_' + str(index)] = directory
        code = owner.run(args.image, ['python3', '/source/scripts/contained-runner.py'], timeout=args.timeout + 30,
            mounts=[(state / 'source', '/source', True), (control, '/control', True), (peers, '/peers', True), (work, '/work', False)],
            environment=environment,
            workdir='/source', binding=binding, before_start=admitted,
            on_running=lambda current: print('contained-gate: running ' + current.cid, file=sys.stderr, flush=True))
        if (work / 'backend.json').is_file():
            normalized = work_json(work, 'backend.json')
        normalized['contained_lifecycle'] = {'receipt': owner.receipt,
            'receipt_sha256': file_hash(owner.directory / 'custody.json'),
            'runtime_state': work_json(work, 'phase-state.json') if (work / 'phase-state.json').exists() else None}
        normalized['original_image_builds'] = [{'registration': name, 'status': 'not_executed',
            'reason': 'distinct prepared-native route; original Docker build not executed'} for name in
            ('groundhog-mdk-interop-image', 'groundhog-mdk011-interop-image', 'groundhog-mdk011-interop-image-mdk09')]
        try:
            if not owner.receipt['cleanup_complete']:
                raise CustodyError('cannot attest retained evidence before verified namespace exit')
            runtime_state = normalized['contained_lifecycle']['runtime_state']
            if runtime_state.get('keyring_runtime') is not None:
                if work_json(work, 'execution/keyring-setup.json') != runtime_state['keyring_runtime']:
                    raise CustodyError('keyring setup bytes changed before final namespace exit')
                if owner.receipt['cleanup_inspect']['HostConfig'].get('Tmpfs') != {'/tmp': 'rw,nosuid,nodev,size=1g'}:
                    raise CustodyError('keyring private HOME tmpfs isolation differs')
                home_verified = runtime_state.get('keyring_home_final') == {'verified': True}
                normalized['contained_lifecycle']['keyring_cleanup'] = {
                    'status': 'verified' if home_verified else 'namespace_only', 'container_id': owner.cid,
                    'scope': 'private_pid_mount_namespace_exit_drain_removal', 'home_tmpfs_destroyed': home_verified,
                    'daemon_log_sha256': file_hash(work_file(work, 'private-keyring.raw'))}
            if runtime_state['backend_sha256'] != file_hash(work_file(work, 'backend.json')):
                raise CustodyError('runtime backend changed before final namespace exit')
            if runtime_state['cache_sha256'] is not None and runtime_state['cache_sha256'] != file_hash(work_file(work, 'build/CMakeCache.txt')):
                raise CustodyError('runtime cache changed before final namespace exit')
            if runtime_state['phase_context_hash'] != normalized['phase_context_hash'] or runtime_state['phase_context'] != normalized['phase_context']:
                raise CustodyError('runtime/normalized phase context differs')
            normalized['contained_lifecycle']['retained_evidence'] = attest_retained_evidence(work, normalized, runtime_state)
            validate_lifecycle(normalized['contained_lifecycle'], plan, args.profile, profile,
                               normalized.get('profile_fingerprint'), evidence=normalized)
            for path, expected in runtime_state.get('artifacts', {}).items():
                relative = Path(path).relative_to('/work')
                if '..' in relative.parts or file_hash(work_file(work, relative)) != expected:
                    raise CustodyError('runtime artifact changed before final namespace exit')
            if code or normalized['contained_lifecycle']['runtime_state']['status'] != 'succeeded':
                raise CustodyError('contained runtime did not succeed')
        except (CustodyError, KeyError, TypeError, ValueError, OSError) as exc:
            normalized['job_conclusion'] = 'failure'
            normalized['diagnostic_eligible'] = False
            normalized['custody_error'] = str(exc)
            code = 1 if code == 0 else code
        normalized['complete_gate_eligible'] = False
        if isinstance(normalized.get('backend_receipt'), dict):
            normalized['backend_receipt']['complete_gate_eligible'] = False
            if code:
                normalized['backend_receipt']['diagnostic_eligible'] = False
        write_json(state / 'receipt.json', normalized)
    print(state / 'receipt.json', flush=True)
    return code


def parser():
    root = argparse.ArgumentParser(description=__doc__)
    commands = root.add_subparsers(dest="operation", required=True)
    prep = commands.add_parser("prepare", help="actual locked Cargo build/parity in owned non-root containers")
    prep.add_argument("--source-root", required=True)
    prep.add_argument("--evidence-dir", required=True)
    prep.add_argument("--version", choices=PEERS, required=True)
    prep.add_argument("--builder-image", required=True)
    prep.add_argument("--rust-image", required=True)
    prep.add_argument("--timeout", type=int, default=1800)
    lane = commands.add_parser('run', help='actual shared-engine prepared profile in one contained Linux namespace')
    lane.add_argument('--source-root', required=True)
    lane.add_argument('--evidence-dir', required=True)
    lane.add_argument('--image', required=True)
    lane.add_argument('--inventory', default='tests/selection/inventory.json')
    lane.add_argument('--plan', required=True)
    lane.add_argument('--profile', required=True)
    lane.add_argument('--peer-receipt', action='append', default=[])
    lane.add_argument('--gitlink-source', action='append', default=[], metavar='PATH=OBJECT_REPO',
                      help='explicit local Git object source; exact gitlink commit is fetched/verified')
    lane.add_argument('--timeout', type=int, default=1800)
    return root


if __name__ == "__main__":
    try:
        arguments = parser().parse_args()
        if arguments.timeout <= 0:
            raise CustodyError("timeout must be positive")
        sys.exit(prepare(arguments) if arguments.operation == 'prepare' else run_lane(arguments))
    except (OSError, ValueError, KeyError, CustodyError, subprocess.SubprocessError) as error:
        print("contained-gate: " + str(error), file=sys.stderr)
        if 'arguments' in globals():
            destination = Path(arguments.evidence_dir)
            destination.mkdir(parents=True, exist_ok=True)
            write_json(destination / ('failure-' + uuid.uuid4().hex + '.json'),
                       {'schema_version': 1, 'operation': arguments.operation, 'status': 'failed',
                        'error': str(error), 'complete': False, 'diagnostic_eligible': False,
                        'complete_gate_eligible': False, 'cleanup_complete': False})
        sys.exit(1)
