#!/usr/bin/env bash
# Explicit, diagnostic shared-plan adapter. Legacy pre-push/full-gate defaults
# stay in force until the rollout evidence authorizes a cutover.
set -euo pipefail
exec python3 - "$0" "$@" <<'PY'
import argparse
import fcntl
import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
import signal
import subprocess
import sys
import uuid

adapter = Path(sys.argv.pop(1)).resolve()
sys.path.insert(0, str(adapter.parent))
import test_selection as shared

# Read-only Git probes must not refresh the developer's index as a side effect.
os.environ["GIT_OPTIONAL_LOCKS"] = "0"

parser = argparse.ArgumentParser(description="Shared local test gate (diagnostic; not CI/push acceptance)")
parser.add_argument("--repo", default=".")
parser.add_argument("--base")
parser.add_argument("--head", help="execute this committed tree, never the caller's dirty tree")
parser.add_argument("--plan", help="execute exact retained committed plan identity; --profile selects physical lanes without rewriting the plan")
parser.add_argument("--broad", action="store_true")
parser.add_argument("--linux-custody", "--native-custody", choices=("legacy", "ptrace", "pidns", "darwin"), default="legacy",
                    help="explicit native custody: Linux ptrace (no instrumentation) or user/mount/PID namespaces; macOS-only "
                         "darwin seatbelt-tag diagnostic (no wrappers/keyring/instrumentation); none is profile parity")
parser.add_argument("--namespace-apparmor-profile", metavar="NAME",
                    help="pidns only: provisioned path-unattached AppArmor profile the custody helper explicitly enters before unshare; host policy unchanged")
parser.add_argument("--explain", action="store_true", help="plan only; no configure/build/test")
parser.add_argument("--profile", action="append", help="explicit partial profile projection")
parser.add_argument("--pre-push", action="store_true", help="read all ref updates; union ranges per SHA")
parser.add_argument("--refs-file", help="pre-push input file (stdin is reserved by this adapter)")
parser.add_argument("--evidence-dir", help="parent for a unique retained run directory")
args = parser.parse_args()
active = None
worktrees = []
receipt = {"schema_version": 1, "adapter": "test-gate", "execution_api": "test-plan.py resolve/run", "complete": False,
           "diagnostic": True, "status": "failed", "executions": [], "errors": []}
receipt["bypasses"] = {name: os.environ[name] for name in
    ("NOSTRC_SKIP_PRE_PUSH", "NOSTRC_SKIP_LINUX_GATE", "NOSTRC_SKIP_SANITIZER_GATE", "NOSTRC_GATE_LINUX_TESTS")
    if os.environ.get(name) == ("0" if name == "NOSTRC_GATE_LINUX_TESTS" else "1")}
receipt["custody_mode"] = args.linux_custody
if args.namespace_apparmor_profile is not None:
    if args.linux_custody != "pidns":
        parser.error("--namespace-apparmor-profile requires --native-custody pidns")
    receipt["namespace_apparmor_profile"] = args.namespace_apparmor_profile
receipt["complete_gate_eligible"] = False
if args.linux_custody != "legacy":
    receipt["untraced_profile_parity"] = False
run_dir = None


def interrupted(signum, frame):
    raise KeyboardInterrupt()


signal.signal(signal.SIGTERM, interrupted)
signal.signal(signal.SIGINT, interrupted)


def descendants_of(parent):
    # CTest and its tests may create additional process groups. Capture only
    # this live child's ancestry before signalling it (before reparenting),
    # then stop leaves first. Never match by executable name or kill siblings.
    rows = subprocess.check_output(["ps", "-axo", "pid=,ppid="]).decode().splitlines()
    children = {}
    for row in rows:
        pid, ppid = map(int, row.split())
        children.setdefault(ppid, []).append(pid)
    descendants = []
    pending = list(children.get(parent, []))
    while pending:
        pid = pending.pop()
        descendants.append(pid)
        pending.extend(children.get(pid, []))
    return descendants


def stop_descendants(parent, captured=None):
    for pid in reversed(descendants_of(parent) if captured is None else captured):
        try:
            os.kill(pid, signal.SIGKILL)
        except ProcessLookupError:
            pass


def linux_command(argv, *, cwd=None, env=None, log=None, resource_root=None, private_display=False, native_lane=None):
    """A gated kernel-backed supervisor owns descendants before target exec."""
    global active
    stream = open(log, "xb") if log else subprocess.DEVNULL
    handle = None
    read_fd, write_fd = os.pipe()
    seal_read, seal_write = os.pipe() if native_lane else (-1, -1)
    waited = False
    try:
        try:
            # The supervisor inherits blocked cancellation before Python starts.
            # It cannot launch a target until our pidfd reservation releases it.
            mask = signal.pthread_sigmask(signal.SIG_BLOCK, {signal.SIGTERM, signal.SIGINT})
            try:
                helper = "adapter_namespace_custody.py" if args.linux_custody == "pidns" else "adapter_custody.py"
                resources = (["--private-resource-root", str(resource_root)] +
                             (["--private-display"] if private_display else [])) if resource_root else []
                if args.linux_custody == "pidns" and args.namespace_apparmor_profile is not None:
                    resources = ["--apparmor-profile", args.namespace_apparmor_profile, *resources]
                active = subprocess.Popen([sys.executable, str(adapter.parent / helper),
                                           "--parent-ready-fd", str(read_fd), "--parent-pid", str(os.getpid()),
                                           *resources, *map(str, argv)],
                                           cwd=cwd,
                                           env=dict(os.environ if env is None else env,
                                                    **({'NOSTRC_NATIVE_SEAL_FD': str(seal_write)} if native_lane else {})),
                                           pass_fds=(read_fd, seal_write) if native_lane else (read_fd,),
                                          stdout=stream, stderr=subprocess.STDOUT)
                if seal_write != -1:
                    os.close(seal_write)
                    seal_write = -1
                handle = os.pidfd_open(active.pid)
                # Signal zero validates the actual signaling syscall/authority
                # while the target launch gate is still closed.
                signal.pidfd_send_signal(handle, 0)
                if signal.sigpending() & {signal.SIGTERM, signal.SIGINT}:
                    raise KeyboardInterrupt()
                os.write(write_fd, b"1")
            finally:
                os.close(read_fd)
                read_fd = -1
                os.close(write_fd)
                write_fd = -1
                signal.pthread_sigmask(signal.SIG_SETMASK, mask)
            code = active.wait()
            waited = True
        except BaseException:
            # Repeated cancellation must not interrupt the final evidence drain.
            cancellation_mask = signal.pthread_sigmask(signal.SIG_BLOCK, {signal.SIGTERM, signal.SIGINT})
            handlers = {sig: signal.signal(sig, signal.SIG_IGN)
                        for sig in (signal.SIGTERM, signal.SIGINT)}
            signal.pthread_sigmask(signal.SIG_SETMASK, cancellation_mask)
            try:
                signaling_error = None
                if handle is not None:
                    try:
                        signal.pidfd_send_signal(handle, signal.SIGTERM)
                    except ProcessLookupError:
                        pass
                    except OSError as exc:
                        signaling_error = exc
                # Without a pidfd the unopened gate already saw EOF: no target
                # was authorized. Never substitute a numeric-PID signal.
                if active is not None:
                    try:
                        active.wait(timeout=20 if signaling_error else None)
                        waited = True
                    except subprocess.TimeoutExpired as exc:
                        raise RuntimeError("pidfd cancellation refused; cleanup_unverified: "
                                           + str(signaling_error)) from exc
                if signaling_error:
                    raise RuntimeError("pidfd cancellation refused; cleanup_unverified: "
                                       + str(signaling_error)) from signaling_error
            finally:
                for sig, handler in handlers.items():
                    signal.signal(sig, handler)
            raise
        if code:
            raise RuntimeError(f"command exited {code}: {Path(str(argv[0])).name}; log={log}")
    finally:
        for fd in (read_fd, write_fd):
            if fd != -1:
                os.close(fd)
        if handle is not None:
            os.close(handle)
        active = None
        if log:
            stream.close()
        if seal_write != -1:
            os.close(seal_write)
        if seal_read != -1:
            original = sys.exc_info()[1]
            try:
                if not waited:
                    raise RuntimeError('native namespace barrier was not observed')
                from test_selection_services import attest_native_lane
                anchor = os.read(seal_read, 65).decode('ascii')
                final = attest_native_lane(native_lane, anchor)
                with (native_lane / 'native-final.json').open('x') as output:
                    json.dump(final, output, sort_keys=True, indent=2)
                    output.write('\n')
            except Exception as exc:
                try:
                    with (native_lane / 'native-final.json').open('x') as output:
                        json.dump({'status': 'failed', 'error': str(exc), 'complete': False,
                                   'complete_gate_eligible': False}, output, sort_keys=True)
                        output.write('\n')
                except FileExistsError:
                    pass  # Retain unexpected bytes; never overwrite child artifacts.
                if original is None:
                    raise RuntimeError('native final evidence refused: ' + str(exc)) from exc
                receipt['errors'].append('native final evidence refused: ' + str(exc))
            finally:
                os.close(seal_read)


darwin_records = []


def darwin_command(argv, *, cwd=None, env=None, log=None, native_lane=None):
    from test_gate_darwin import darwin_command as run
    return run(argv, helper=adapter.parent / 'adapter_darwin_custody.py', records=darwin_records,
               errors=receipt['errors'], cwd=cwd, env=env, log=log, native_lane=native_lane)


def command(argv, *, cwd=None, env=None, log=None, resource_root=None, private_display=False, native_lane=None):
    # macOS remains the inherited legacy compatibility path, NOT safe unified
    # arbitrary-descendant custody. Do not substitute a reserved-group-only path.
    if args.linux_custody == "darwin":
        if platform.system() != "Darwin":
            raise RuntimeError("darwin custody is an explicit macOS-only diagnostic; target not launched")
        if resource_root is not None or private_display:
            raise RuntimeError("darwin custody hosts no private display/bus resources; target not launched")
        return darwin_command(argv, cwd=cwd, env=env, log=log, native_lane=native_lane)
    if args.linux_custody != "legacy":
        if platform.system() != "Linux":
            raise RuntimeError("native kernel custody unavailable on macOS: EndpointSecurity descendant client requires com.apple.developer.endpoint-security.client (not provisioned); kqueue NOTE_TRACK is ENOTSUP; target not launched"
                               if args.linux_custody == "pidns" else "ptrace custody is an explicit Linux-only diagnostic")
        if (not hasattr(os, "pidfd_open") or not hasattr(signal, "pidfd_send_signal")
                or signal.getsignal(signal.SIGCHLD) != signal.SIG_DFL):
            raise RuntimeError("Linux ptrace/pidfd custody unavailable before command launch")
        return linux_command(argv, cwd=cwd, env=env, log=log,
                             resource_root=resource_root, private_display=private_display, native_lane=native_lane)
    global active
    stream = open(log, "xb") if log else subprocess.DEVNULL
    try:
        active = subprocess.Popen([str(x) for x in argv], cwd=cwd, env=env,
                                  stdout=stream, stderr=subprocess.STDOUT, start_new_session=True)
        try:
            code = active.wait()
        except BaseException:
            # Give the shared backend its cancellation boundary so it can drain
            # raw command output and finalize the failed attempt before exit.
            captured = descendants_of(active.pid)
            try:
                os.killpg(active.pid, signal.SIGTERM)
                # CTest can put tests in separate groups. Capture ancestry
                # before TERM reparents them; leave the backend itself alive
                # to drain their pipes and retain cancellation evidence.
                stop_descendants(active.pid, captured)
                active.wait(timeout=15)
            except ProcessLookupError:
                pass
            except subprocess.TimeoutExpired:
                stop_descendants(active.pid)
            raise
        finally:
            # Also reap grandchildren left behind when the direct child exited.
            try:
                os.killpg(active.pid, signal.SIGTERM)
            except ProcessLookupError:
                pass
            try:
                active.wait(timeout=10)
            except subprocess.TimeoutExpired:
                os.killpg(active.pid, signal.SIGKILL)
                active.wait()
            try:
                os.killpg(active.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            active = None
        if code:
            raise RuntimeError(f"command exited {code}: {Path(str(argv[0])).name}; log={log}")
    finally:
        if log:
            stream.close()


def git(*argv, repo=None):
    return shared.git(repo or args.repo, *argv).stdout


def default_branch():
    for ref in ("refs/remotes/origin/HEAD", "refs/remotes/origin/master", "refs/remotes/origin/main"):
        try:
            return shared.commit_sha(args.repo, ref)
        except shared.ContractError:
            pass
    return None


def checkout(sha, destination):
    command(["git", "-c", "core.hooksPath=/dev/null", "-C", args.repo,
             "worktree", "add", "--detach", destination, sha], log=destination.parent / (destination.name + "-checkout.log"))
    worktrees.append(destination)


def snapshot(source, head):
    """Use a disposable index; never stage/reset the developer's real index."""
    original_index = Path(os.fsdecode(git("rev-parse", "--git-path", "index")).strip())
    if not original_index.is_absolute():
        original_index = Path(args.repo) / original_index
    index_copy = source.parent / "original.index"
    shutil.copyfile(original_index, index_copy)
    index_tree = subprocess.check_output(["git", "-C", args.repo, "write-tree"],
                                        env=dict(os.environ, GIT_INDEX_FILE=str(index_copy))).decode().strip()
    # Gitlinks cannot encode dirty submodule contents. Reject rather than omit.
    for entry in git("ls-files", "--stage", "-z").split(b"\0"):
        if entry.startswith(b"160000 "):
            path = os.fsdecode(entry.split(b"\t", 1)[1])
            sub = Path(args.repo) / path
            if (sub / ".git").exists():
                if git("status", "--porcelain", "--untracked-files=all", repo=sub):
                    raise RuntimeError(f"dirty submodule cannot be snapshotted: {path}")
            elif sub.is_dir() and any(sub.iterdir()):
                raise RuntimeError(f"nonempty uninitialized submodule cannot be snapshotted: {path}")
    env = dict(os.environ, GIT_INDEX_FILE=str(source.parent / "snapshot.index"))
    command(["git", "-C", args.repo, "read-tree", head], env=env)
    # Explicit generated root exclusions, not an allowlist of known sources.
    excludes = ["build", "_build", "build-*", "cmake-build-*"]
    evidence_parent = run_dir.parent
    if evidence_parent.is_relative_to(Path(args.repo)):
        excludes.append(str(evidence_parent.relative_to(args.repo)))
    argv = ["git", "-C", args.repo, "add", "-A", "--", "."]
    argv += [f":(top,exclude){p}" for p in excludes]
    command(argv, env=env, log=source.parent / "snapshot-add.log")
    # An embedded repository would otherwise become an undeclared gitlink.
    if b"adding embedded git repository" in (source.parent / "snapshot-add.log").read_bytes():
        raise RuntimeError("unsupported untracked embedded repository in snapshot")
    tree = subprocess.check_output(["git", "-C", args.repo, "write-tree"], env=env).decode().strip()
    def gitlinks(revision):
        return {entry.split(b"\t", 1)[1] for entry in git("ls-tree", "-rz", revision).split(b"\0")
                if entry.startswith(b"160000 ")}
    if gitlinks(tree) - gitlinks(head):
        raise RuntimeError("new/untracked submodule materialization is unsupported in dirty snapshots")
    changes = []
    for revision in (index_tree, tree):
        modes = {}
        for entry in git("ls-tree", "-rz", revision).split(b"\0"):
            if entry:
                meta, path = entry.split(b"\t", 1)
                modes[os.fsdecode(path)] = meta.split()[0]
        for change in shared.diff_records(args.repo, head, revision):
            change["executable"] = modes.get(change.get("new_path")) == b"100755"
            change["gitlink"] = modes.get(change.get("new_path")) == b"160000"
            changes.append(change)
    command(["git", "-C", source, "read-tree", "--reset", "-u", tree])
    digest = hashlib.sha256()
    for entry in git("ls-tree", "-rz", tree).split(b"\0"):
        if not entry:
            continue
        meta, raw_path = entry.split(b"\t", 1)
        mode, kind, oid = meta.split()
        path = source / os.fsdecode(raw_path)
        if kind == b"commit":
            content = oid
        elif mode == b"120000":
            content = os.fsencode(os.readlink(path))
            target = (path.parent / os.fsdecode(content)).resolve()
            if not target.is_relative_to(source):
                raise RuntimeError(f"snapshot symlink escapes checkout: {os.fsdecode(raw_path)}")
        else:
            content = path.read_bytes()
        digest.update(mode + b"\0" + raw_path + b"\0" + hashlib.sha256(content).digest())
    return changes, {"kind": "dirty", "sha": head, "snapshot_digest": digest.hexdigest()}, tree


def materialize_submodules(source, directory, profile, initialized):
    # Same build prerequisites as the existing full-gate adapter, at the
    # candidate's gitlink OIDs; do not fetch the unrelated docs submodule.
    candidates = ("third_party/nsync",) if profile.get("source_dir") == "libgo" else ("third_party/nsync", "third_party/nostrdb")
    required = [path for path in candidates if path not in initialized
                and git("ls-files", "--stage", "--", path, repo=source).startswith(b"160000 ")]
    records = []
    def hydrate(parent, cache_parent, relative):
        row = git('ls-files', '--stage', '-z', '--', relative, repo=parent).split(b'\0')[0]
        fields, path = row.split(b'\t', 1)
        mode, oid, stage = fields.decode().split()
        if mode != '160000' or stage != '0' or os.fsdecode(path) != relative:
            raise RuntimeError('submodule is not one exact selected gitlink: ' + relative)
        config = shared.git(parent, 'config', '-f', '.gitmodules', '--get-regexp', r'^submodule\..*\.path$', check=False)
        names = [line.split(None, 1)[0][len('submodule.'):-len('.path')]
                 for line in config.stdout.decode().splitlines() if line.split(None, 1)[1] == relative]
        if len(names) != 1:
            raise RuntimeError('submodule lacks exact source-authored path/name: ' + relative)
        cache = Path(cache_parent) / relative
        usable = False
        if cache.is_dir() and not cache.is_symlink() and cache.resolve().is_relative_to(Path(cache_parent).resolve()):
            probe = shared.git(cache, 'rev-parse', '--show-toplevel', check=False)
            usable = (probe.returncode == 0 and Path(os.fsdecode(probe.stdout).strip()).resolve() == cache.resolve()
                      and shared.git(cache, 'cat-file', '-e', oid + '^{commit}', check=False).returncode == 0)
        argv = ['git', '-c', 'core.hooksPath=/dev/null']
        if usable:
            # File transport is enabled for ONLY this exact overridden module;
            # unknown/unavailable caches retain the original network route.
            argv += ['-c', 'protocol.file.allow=always', '-c', 'submodule.' + names[0] + '.url=' + str(cache.resolve())]
        argv += ['-C', parent, 'submodule', 'update', '--init', '--checkout', '--', relative]
        destination = Path(parent) / relative
        key = hashlib.sha256(os.fsencode(destination)).hexdigest()[:16]
        command(argv, log=directory / ('submodule-' + key + '.log'))
        if git('rev-parse', 'HEAD', repo=destination).decode().strip() != oid:
            raise RuntimeError('materialized submodule differs from selected gitlink: ' + relative)
        records.append(dict(path=str(destination), gitlink=oid, actual_head=oid,
                            object_source=str(cache.resolve()) if usable else 'source-authored network URL'))
        for entry in git('ls-files', '--stage', '-z', repo=destination).split(b'\0'):
            if entry.startswith(b'160000 '):
                hydrate(destination, cache, os.fsdecode(entry.split(b'\t', 1)[1]))
    for relative in required:
        hydrate(source, Path(args.repo), relative)
    if required:
        shared.write_json(directory / 'submodules.json', {'kind': 'exact_gitlink_materialization_v1', 'modules': records,
            'scope': 'source objects only; full configure/Ninja/header/runtime audits remain mandatory'})
    initialized.update(required)


def host_guard(profile, checks, policy, profile_id):
    if args.linux_custody == "ptrace" and profile.get("instrumentation") != "none":
        raise RuntimeError("unsupported ptrace custody instrumentation: "
                           + str(profile.get("instrumentation")) + "; lane not launched")
    if profile.get("execution_domain") == "prepared_peer_linux_v1":
        raise RuntimeError("prepared profile requires scripts/contained-gate.py; legacy outer custody is unchanged")
    host = {"Darwin": "macos", "Linux": "linux", "Windows": "windows"}.get(platform.system())
    arch = {"amd64": "x86_64", "aarch64": "arm64"}.get(platform.machine().lower(), platform.machine().lower())
    wanted_arch = {"amd64": "x86_64", "aarch64": "arm64"}.get(profile["architecture"], profile["architecture"])
    if {"darwin": "macos"}.get(profile["platform"], profile["platform"]) != host or wanted_arch not in {"native", arch}:
        raise RuntimeError(f"selected platform/architecture requires an unimplemented guarded container/native adapter: {profile['platform']}/{wanted_arch}; host={host}/{arch}")
    if any(policy["checks"][x]["kind"] not in {"ctest", "command", "build_contract"} for x in checks):
        raise RuntimeError("selected check kind is unsupported by this outer adapter; not bypassed")
    profile_checks = [check for check in policy["checks"].values() if profile_id in check["profiles"]]
    source_only = bool(profile_checks) and all(check["kind"] == "command" and not check.get("build_targets")
                                              for check in profile_checks)
    caps = set(profile.get("capabilities", []))
    caps.update(cap for ident in checks for cap in policy["checks"][ident].get("requires", []))
    if args.linux_custody == "darwin":
        if host != "macos":
            raise RuntimeError("darwin custody is an explicit macOS-only diagnostic; lane not launched")
        instrumentation = profile.get("instrumentation")
        if instrumentation == "asan_ubsan":
            # Only C's demonstrated domain: Apple clang ASan/UBSan on arm64 with
            # authored detect_leaks=0 (LeakSanitizer is unsupported by the
            # toolchain, identically outside custody). No TSan/fuzz/x86 inference.
            asan = dict(item.split("=", 1) for item in profile.get("environment", {}).get("ASAN_OPTIONS", "").split(":") if "=" in item)
            cc = profile.get("environment", {}).get("CC", "clang")
            apple = shutil.which(cc) and "apple clang" in subprocess.check_output([cc, "--version"]).decode(errors="replace").lower()
            if arch != "arm64" or profile.get("compiler") != "clang" or not apple or asan.get("detect_leaks") != "0":
                raise RuntimeError("darwin custody instrumentation outside the demonstrated Apple clang arm64 "
                                   "ASan/UBSan detect_leaks=0 domain; lane not launched")
        elif instrumentation != "none":
            raise RuntimeError(f"darwin custody instrumentation {instrumentation} is unproven; lane not launched")
        refused = caps & {"display", "private_bus", "gnome_keyring", "signer_endpoint", "docker"}
        if refused:
            raise RuntimeError(f"darwin custody does not implement {sorted(refused)}; lane not launched")
    if 'gnome_keyring' in caps:
        if any(policy['checks'][ident]['kind'] != 'ctest' and
               'gnome_keyring' in policy['checks'][ident].get('requires', []) for ident in checks):
            raise RuntimeError('gnome_keyring commands require live contained private HOME/private bus/throwaway Secret Service custody; native adapter not implemented; lane not launched')
        if not shutil.which('gnome-keyring-daemon'):
            raise RuntimeError('selected gnome_keyring capability requires the actual gnome-keyring-daemon executable; lane not launched')
    if "nak" in caps and not shutil.which("nak"):
        raise RuntimeError("selected nak capability requires the actual nak executable; lane not launched")
    if 'signer_endpoint' in caps:
        if (host != 'linux' or args.linux_custody != 'pidns'
                or profile['architecture'] != platform.machine()):
            raise RuntimeError('signer fixture requires exact native Linux architecture and pidns custody; lane not launched')
        required = {'ENABLE_TCP_IPC': 'ON', 'NIP55L_TEST_TRUST_ENV': 'ON',
                    'NIP55L_SECRET_BACKEND': 'libsecret', 'BUILD_APPS': 'ON'}
        for name, value in required.items():
            flags = [flag for flag in profile.get('configure_args', [])
                     if flag.startswith('-D' + name + '=') or flag.startswith('-D' + name + ':')]
            if flags != ['-D' + name + '=' + value]:
                raise RuntimeError('signer fixture lacks exact authored configuration: ' + name)
        if profile.get('environment', {}).get('NOSTRC_TEST_NIP5F_APPROVAL') != 'unidentified-get-public-key-once':
            raise RuntimeError('signer fixture lacks authored one-shot consent')
        for ident in checks:
            check = policy['checks'][ident]
            if 'signer_endpoint' in check.get('requires', []) and (
                    check['kind'] != 'ctest' or check.get('ctest_tests', {}).get(profile_id) != ['test_nip5f_tcp']):
                raise RuntimeError('signer fixture supports only exact test_nip5f_tcp; lane not launched')
        if not {'display', 'private_bus'} <= caps:
            raise RuntimeError('signer fixture requires authored namespace-owned display/private bus resources')
    if 'docker' in caps:
        # Original MDK images/peers: Docker scope by the selected controller,
        # host tree by the PID namespace. Never legacy/ptrace or other domains.
        if (host != 'linux' or args.linux_custody != 'pidns' or profile['architecture'] != platform.machine()
                or profile.get('execution_domain') != 'mdk_original_linux_v1'):
            raise RuntimeError("selected capabilities require an unimplemented guarded adapter: ['docker'] "
                               '(original MDK image/peer lifecycle requires its authored domain, exact native '
                               'Linux architecture and pidns custody); lane not launched')
        caps = caps - {'docker'}
    unsupported = caps - {"display", "private_bus", "nak", "gnome_keyring", "signer_endpoint"}
    if unsupported:
        raise RuntimeError(f"selected capabilities require an unimplemented guarded adapter: {sorted(unsupported)}")
    wrapped = caps & {"display", "private_bus"}
    if wrapped and args.linux_custody != "pidns" and any(policy["checks"][ident]["kind"] != "ctest" for ident in checks):
        raise RuntimeError(f"unsupported cancellation custody: selected command/build_contract lane requires wrappers {sorted(wrapped)}; not launched")
    if wrapped and args.linux_custody == "ptrace":
        # Root wrapper death need not leave the backend alive for final receipts.
        # No wrapped proof is admitted by this bounded, unwrapped diagnostic.
        raise RuntimeError(f"unsupported {args.linux_custody} custody wrappers: {sorted(wrapped)}; lane not launched")
    # Never reuse the developer's display/session bus as proof of isolation.
    prefix = []
    if args.linux_custody == "pidns" and wrapped:
        # The backend itself is the namespace's retained root, not the child
        # of a bus/display wrapper that could die before receipt finalization.
        for executable in (["dbus-daemon", "dbus-send"] if "private_bus" in caps else []) + (["Xvfb", "xauth"] if "display" in caps else []):
            if not shutil.which(executable):
                raise RuntimeError(f"selected private resources require {executable}; lane not launched")
    elif "private_bus" in caps:
        if not shutil.which("dbus-run-session"):
            raise RuntimeError("selected private_bus requires dbus-run-session")
        prefix += ["dbus-run-session", "--"]
    if "display" in caps and args.linux_custody != "pidns":
        if not shutil.which("xvfb-run"):
            raise RuntimeError("selected display requires xvfb-run")
        prefix += ["xvfb-run", "-a", "-s", "-screen 0 1280x800x24"]
    # Backend defaults must not silently pick a different compiler family.
    if source_only:
        # Core source-only realization does not configure or attest a compiler.
        return prefix, dict(os.environ, **profile.get("environment", {})), True
    compiler = profile["compiler"]
    cc = profile.get("environment", {}).get("CC", {"gcc": "gcc", "clang": "clang"}.get(compiler))
    if not cc or not shutil.which(cc):
        raise RuntimeError(f"unsupported/unavailable compiler: {compiler}/{cc}")
    version = subprocess.check_output([cc, "--version"]).decode(errors="replace").lower()
    if (compiler == "gcc" and ("clang" in version or not any(x in version for x in ("gcc", "free software foundation")))) or (compiler == "clang" and "clang" not in version):
        raise RuntimeError(f"compiler identity does not match profile: {compiler}")
    if host == "macos" and profile.get("source_dir", ".") != "libgo":
        flags = profile.get("configure_args", [])
        if "-DNIP55L_SECRET_BACKEND=libsecret" not in flags or "-DGROUNDHOG_STORE_KEY_DEFAULT_BACKEND=secret-service" not in flags:
            raise RuntimeError("host profile lacks mandatory private Secret Service backend configuration")
    return prefix, dict(os.environ, CC=cc, **{k: v for k, v in profile.get("environment", {}).items() if k != "CC"}), False


def execute(head, events, number, dirty=False):
    directory = run_dir / str(number)
    directory.mkdir()
    source = directory / "source"
    record = {"head": head, "events": events, "status": "failed", "lanes": []}
    receipt["executions"].append(record)
    checkout(head, source)
    inventory_path = "tests/selection/inventory.json"
    policy = shared.load_inventory(source / inventory_path)
    changes, identity, tree = [], None, None
    if dirty:
        changes, identity, tree = snapshot(source, head)
        policy = shared.load_inventory(source / inventory_path)
    if args.plan:
        plan = shared.read_json(args.plan)
        shared.validate_plan(plan, policy)
        if plan['source_identity'] != {'kind': 'committed', 'sha': head}:
            raise RuntimeError('retained plan must bind the exact committed checkout')
        if args.profile and not set(args.profile) <= {lane['profile'] for lane in plan['lanes']}:
            raise RuntimeError('retained plan does not select requested profile')
    else:
        plan = shared.git_plan(args.repo, policy, head=head, base=args.base, context="local",
                               mode="broad" if args.broad else "affected", event=events, profiles=args.profile)
    if dirty:
        old = shared.validate_inventory(json.loads(git("show", f"{head}:{inventory_path}")))
        head_modes = {}
        for entry in git("ls-tree", "-rz", head).split(b"\0"):
            if entry:
                meta, path = entry.split(b"\t", 1)
                head_modes[os.fsdecode(path)] = meta.split()[0]
        modes = {}
        for entry in git("ls-tree", "-rz", tree).split(b"\0"):
            if entry:
                meta, path = entry.split(b"\t", 1)
                modes[os.fsdecode(path)] = meta.split()[0]
        for change in changes:
            change["old_executable"] = head_modes.get(change.get("old_path")) == b"100755"
            change["old_gitlink"] = head_modes.get(change.get("old_path")) == b"160000"
            change["executable"] = change.get("executable", False) or modes.get(change.get("new_path")) == b"100755"
            change["gitlink"] = change.get("gitlink", False) or modes.get(change.get("new_path")) == b"160000"
        plan = shared.make_plan(policy, plan["changes"] + changes, head=head, base=args.base, context="local",
                                mode="broad" if args.broad else "affected", event=plan["event"], profiles=args.profile,
                                broadening=plan["broadening_reasons"], base_policies=[old], source_identity=identity)
    shared.write_json(directory / "plan.json", plan)
    record.update(plan=str(directory / "plan.json"), source_identity=plan["source_identity"], plan_id=plan["plan_id"])
    print(f"{head}: {plan['effective_mode']}, {len(plan['selected_checks'])} checks, {len(plan['lanes'])} lanes", flush=True)
    if args.explain:
        for reason in plan["broadening_reasons"]:
            print(f"  broad: {reason}", flush=True)
        for lane in plan["lanes"]:
            print(f"  {lane['profile']}: {', '.join(lane['checks'])}", flush=True)
        record["status"] = "planned"
        return
    submodules_ready = set()
    receipts = []
    for lane in plan["lanes"]:
        profile_id = lane["profile"]
        if args.plan and args.profile and profile_id not in args.profile:
            continue
        profile = policy["profiles"][profile_id]
        # Never interpolate policy IDs as paths.
        lane_dir = directory / ("lane-" + hashlib.sha256(profile_id.encode()).hexdigest()[:16])
        lane_dir.mkdir()
        state = {"profile": profile_id, "status": "failed", "evidence": str(lane_dir)}
        record["lanes"].append(state)
        guard = adapter.parent / "check-macos-keychain.sh"
        if platform.system() == "Darwin":
            # A failing guard aborts the entire run, not just this lane.
            command(["bash", guard], log=lane_dir / "keychain-before.log")
        try:
            prefix, env, source_only = host_guard(profile, lane["checks"], policy, profile_id)
            if not source_only:
                materialize_submodules(source, lane_dir, profile, submodules_ready)
            # Machine-wide, cross-checkout serialization. OS locks release on
            # cancellation; never remove a lock file another waiter has open.
            locks = Path("/tmp") / f"nostrc-test-gate-{os.getuid()}"
            locks.mkdir(mode=0o700, exist_ok=True)
            if locks.is_symlink() or locks.stat().st_uid != os.getuid() or locks.stat().st_mode & 0o077:
                raise RuntimeError("unsafe machine-wide gate lock directory")
            lock_path = locks / (hashlib.sha256(profile_id.encode()).hexdigest() + ".lock")
            with open(lock_path, "a") as lock:
                fcntl.flock(lock, fcntl.LOCK_EX)
                common = ["--repo", source, "--profile", profile_id, "--plan", directory / "plan.json",
                          "--build-dir", lane_dir / "build", "--evidence-dir", lane_dir / "evidence"]
                if args.linux_custody == 'darwin':
                    common += ['--diagnostic-boundary', 'darwin_seatbelt_v1']
                elif args.linux_custody != 'legacy':
                    common += ['--diagnostic-boundary', 'linux_' + args.linux_custody + '_v1']
                resolved = lane_dir / "resolved.json"
                result = lane_dir / "receipt.json"
                # One private display/bus spans configure and execution: their
                # addresses are part of the backend environment fingerprint.
                # TERM must raise so the backend cleans its command groups.
                run_api = '''import contextlib, json, os, pathlib, runpy, signal, sys
def stop(signum, frame):
    raise KeyboardInterrupt()
signal.signal(signal.SIGTERM, stop)
scripts = sys.argv.pop(1)
service_scripts, service_caps, service_evidence = sys.argv[1:4]
del sys.argv[1:4]
seal_fd = os.environ.pop('NOSTRC_NATIVE_SEAL_FD', None)
if seal_fd is not None:
    seal_fd = int(seal_fd)
    os.set_inheritable(seal_fd, False)
    import ctypes
    libc = ctypes.CDLL(None, use_errno=True)
    if sys.platform == 'linux':
        # Same-UID workers must not reopen this private pipe through procfs.
        if libc.prctl(4, 0, 0, 0, 0) != 0 or libc.prctl(3, 0, 0, 0, 0) != 0:
            raise RuntimeError('native finalizer nondumpable boundary unavailable')
    else:
        # macOS has no procfs descriptor-reopen path. PT_DENY_ATTACH closes
        # ptrace attach only; task-port access is AMFI/taskgated host policy,
        # which the adapter's fail-closed seal admission must have established
        # (test_gate_darwin.seal_admission) before this finalizer was launched.
        libc.ptrace.argtypes = [ctypes.c_int, ctypes.c_int, ctypes.c_void_p, ctypes.c_int]
        if libc.ptrace(31, 0, None, 0) != 0:
            raise RuntimeError('native finalizer PT_DENY_ATTACH boundary unavailable')
observed, original_resolution = [], []
def observe(result):
    observed.append(json.loads(json.dumps(result)))
def observe_resolution(result):
    original_resolution.append(json.loads(json.dumps(result)))
sys.path.insert(0, scripts)
cli = runpy.run_path(str(pathlib.Path(scripts) / "test-plan.py"))
args = cli["parser"]().parse_args(sys.argv[1:])
directory = pathlib.Path(args.output).parent
resolve = ["resolve", "--repo", args.repo, "--inventory", args.inventory, "--profile", args.profile,
           "--plan", args.plan, "--build-dir", args.build_dir, "--evidence-dir", args.evidence_dir,
           "--configure", "--output", args.resolved]
if getattr(args, 'diagnostic_boundary', None):
    resolve += ['--diagnostic-boundary', args.diagnostic_boundary]
try:
    with contextlib.ExitStack() as resources:
        if service_caps:
            sys.path.insert(0, service_scripts)
            from test_selection_services import namespace_services, resource_mountpoint
            resources.enter_context(namespace_services(service_caps.split(','), service_evidence,
                                    resource_mountpoint(pathlib.Path(service_evidence).parent), finalizer_fd=seal_fd))
        with (directory / "resolve.log").open("x") as log, contextlib.redirect_stdout(log), contextlib.redirect_stderr(log):
            status = cli["main"](resolve, **({'result_observer': observe_resolution} if seal_fd is not None else {}))
        if status:
            sys.exit(status)
        with (directory / "run.log").open("x") as log, contextlib.redirect_stdout(log), contextlib.redirect_stderr(log):
            status = cli["main"](sys.argv[1:], **({'result_observer': observe} if seal_fd is not None else {}))
        if status:
            sys.exit(status)
    sys.exit(0)
finally:
    if seal_fd is not None:
        # Only this trusted root retains the FD. Popen closes it in services
        # and workloads; the original in-memory receipt cannot be replaced by
        # a late filesystem writer before we anchor the seal to the parent.
        signal.pthread_sigmask(signal.SIG_BLOCK, {signal.SIGTERM, signal.SIGINT})
        for sig in (signal.SIGTERM, signal.SIGINT):
            signal.signal(sig, signal.SIG_IGN)
        try:
            if observed:
                sys.path.insert(0, service_scripts)
                from test_selection_services import seal_native_lane
                sys.stdout.flush(); sys.stderr.flush()
                digest = seal_native_lane(directory, args.repo, observed[-1], original_resolution[-1])
                os.write(seal_fd, digest.encode('ascii'))
        finally:
            os.close(seal_fd)
'''
                service_caps = set(profile.get("capabilities", []))
                service_caps.update(cap for ident in lane["checks"] for cap in policy["checks"][ident].get("requires", []))
                service_caps = service_caps & {"display", "private_bus"} if args.linux_custody == "pidns" else set()
                if service_caps:
                    state["private_services"] = str(lane_dir / "services/services.json")
                resource_root = None
                if service_caps:
                    # Short owned mountpoint: sockets cannot live under deep evidence roots.
                    from test_selection_services import allocate_resource_mountpoint
                    resource_root = allocate_resource_mountpoint(lane_dir)
                try:
                    command(prefix + [sys.executable, "-c", run_api, source / "scripts", adapter.parent,
                                      ','.join(sorted(service_caps)), lane_dir / "services", "run", *common,
                                      "--resolved", resolved, "--phase", "all", "--timeout", str(profile["timeout_seconds"]),
                                      "--output", result],
                            env=env, log=lane_dir / "lane.log", resource_root=resource_root,
                            private_display="display" in service_caps,
                            native_lane=lane_dir if args.linux_custody in ('pidns', 'darwin') else None)
                finally:
                    # After the helper's wait barrier only the empty host
                    # mountpoint remains; private HOME/socket bytes were tmpfs.
                    if resource_root:
                        from test_selection_services import release_resource_mountpoint
                        release_resource_mountpoint(lane_dir)
                    if args.linux_custody == 'pidns' and (lane_dir / 'evidence').is_dir():
                        # Docker resources outlive a namespace kill: replay exact
                        # journaled cleanup outside it, never as success.
                        from test_selection_mdk import recover_lane
                        state['mdk_recovery'] = recover_lane(lane_dir / 'evidence', source / 'scripts/mdk-gate.py', lane_dir)
                if any(not item['closed_in_namespace'] for item in state.get('mdk_recovery', [])):
                    raise RuntimeError('original MDK lifecycle required out-of-namespace recovery; lane failed: '
                                       + json.dumps(state['mdk_recovery'], sort_keys=True))
                if args.linux_custody in ('pidns', 'darwin'):
                    import test_selection_realization as realization_backend
                    realization_backend._validate_source_identity(source, plan['source_identity'], lane_dir / 'build')
                    state['retained_evidence'] = str(lane_dir / 'native-final.json')
                    from test_selection_services import native_public_receipt
                    public = native_public_receipt(lane_dir)
                    result = lane_dir / 'native-receipt.json'
                    with result.open('x') as output:
                        json.dump(public, output, sort_keys=True, indent=2)
                        output.write('\n')
                    state['normalized_receipt'] = str(result)
                receipts.append(result)
            state["status"] = "passed"
        except (RuntimeError, OSError, subprocess.SubprocessError) as exc:
            state["error"] = str(exc)
            print(f"test-gate: {profile_id}: {exc}", file=sys.stderr, flush=True)
        finally:
            if args.linux_custody == 'darwin':
                # Every custody invocation for this lane, including refusals,
                # recovery and cleanup_unverified outcomes.
                state['darwin_custody'] = [r for r in darwin_records if r['log'].startswith(str(lane_dir) + os.sep)]
            if platform.system() == "Darwin":
                # Read-only, even on failure/cancellation. Never attempt repair.
                command(["bash", guard], log=lane_dir / "keychain-after.log")
    aggregate = shared.aggregate(policy, plan, [shared.read_json(p) for p in receipts], complete=False, repo=source)
    shared.write_json(directory / "aggregate.json", aggregate)
    record["status"] = "partial" if aggregate["passed"] and all(x["status"] == "passed" for x in record["lanes"]) else "failed"
    if args.plan and args.profile:
        # This is one exact physical lane, not successful aggregation of all
        # sibling lanes. Preserve the original failing/incomplete aggregate.
        record['scope'] = 'explicit_retained_plan_lane_projection'
        record['complete'] = record['complete_gate_eligible'] = False
        record['status'] = 'partial' if record['lanes'] and all(x['status'] == 'passed' for x in record['lanes']) else 'failed'


try:
    args.repo = git("rev-parse", "--show-toplevel").decode().strip()
    if args.pre_push and (args.head or args.base):
        raise RuntimeError("--pre-push cannot be combined with --head/--base")
    if args.refs_file and not args.pre_push:
        raise RuntimeError("--refs-file requires --pre-push")
    common = Path(os.fsdecode(git("rev-parse", "--git-common-dir")).strip())
    if not common.is_absolute():
        common = Path(args.repo) / common
    parent = Path(args.evidence_dir).resolve() if args.evidence_dir else common.resolve() / "nostrc-test-gate"
    parent.mkdir(parents=True, exist_ok=True)
    run_dir = parent / uuid.uuid4().hex
    run_dir.mkdir(mode=0o700)
    print(f"test-gate: diagnostic evidence: {run_dir}", flush=True)
    if args.plan and (not args.head or args.base or args.broad or args.pre_push):
        raise RuntimeError('--plan requires --head and refuses --base/--broad/--pre-push')
    groups = {}
    if args.pre_push:
        if not args.refs_file:
            raise RuntimeError("--pre-push requires --refs-file (preserved hook input)")
        lines = Path(args.refs_file).read_text().splitlines()
        if not lines:
            raise RuntimeError("no pre-push refs received")
        for line in lines:
            fields = line.split()
            if len(fields) != 4:
                raise RuntimeError("malformed pre-push ref update")
            local_ref, oid, remote_ref, base = fields
            if shared.zero_oid(oid):
                receipt.setdefault("deleted_refs", []).append({"local_ref": local_ref, "remote_ref": remote_ref, "remote_oid": base})
                continue
            head = shared.commit_sha(args.repo, oid)
            event = {"kind": "local", "tested_sha": head, "base_sha": base,
                     "local_ref": local_ref, "remote_ref": remote_ref, "remote_oid": base}
            if shared.zero_oid(base):
                event["default_branch"] = default_branch()
            groups.setdefault(head, []).append(event)
        for number, (head, events) in enumerate(groups.items()):
            execute(head, events, number)
    else:
        head = shared.commit_sha(args.repo, args.head or "HEAD")
        base = args.base
        if base is None and args.head is None:
            default = default_branch()
            if default:
                base = git("merge-base", default, head).decode().strip()
        execute(head, {"kind": "local", "base_sha": base}, 0, dirty=args.head is None)
    statuses = [x["status"] for x in receipt["executions"]]
    receipt["status"] = "failed" if "failed" in statuses else ("planned" if args.explain else "partial")
except KeyboardInterrupt:
    receipt["status"] = "cancelled"
    receipt["errors"].append("interrupted; owned child groups stopped; no coverage claimed"
                             if args.linux_custody == "legacy" else
                             f"interrupted; {args.linux_custody} diagnostic cleanup attempted; no profile parity or coverage claimed")
except (RuntimeError, OSError, ValueError, KeyError, shared.ContractError, subprocess.SubprocessError) as exc:
    receipt["errors"].append(str(exc))
finally:
    for source in reversed(worktrees):
        # Submodules make git worktree remove refuse even with --force. A second
        # --force is Git's explicit permission for this private checkout only.
        subprocess.run(["git", "-C", args.repo, "worktree", "remove", "--force", "--force", str(source)],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    if args.linux_custody == "darwin":
        receipt["darwin_custody"] = darwin_records  # every invocation, including checkout/guards/refusals
    if run_dir:
        shared.write_json(run_dir / "adapter-receipt.json", receipt)
        print(f"test-gate: {receipt['status']}; NOT complete CI/push coverage; receipt={run_dir / 'adapter-receipt.json'}", flush=True)
    for error in receipt["errors"]:
        print(f"test-gate: {error}", file=sys.stderr)
sys.exit(130 if receipt["status"] == "cancelled" else 1 if receipt["status"] == "failed" else 0)
PY
