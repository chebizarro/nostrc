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
parser.add_argument("--broad", action="store_true")
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
run_dir = None


def interrupted(signum, frame):
    raise KeyboardInterrupt()


signal.signal(signal.SIGTERM, interrupted)
signal.signal(signal.SIGINT, interrupted)


def stop_descendants(parent):
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
    for pid in reversed(descendants):
        try:
            os.kill(pid, signal.SIGKILL)
        except ProcessLookupError:
            pass


def command(argv, *, cwd=None, env=None, log=None):
    """Own only our child group, including configure/build grandchildren."""
    global active
    stream = open(log, "xb") if log else subprocess.DEVNULL
    try:
        active = subprocess.Popen([str(x) for x in argv], cwd=cwd, env=env,
                                  stdout=stream, stderr=subprocess.STDOUT, start_new_session=True)
        try:
            code = active.wait()
        except BaseException:
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
    if required:
        command(["git", "-c", "core.hooksPath=/dev/null", "-C", source, "submodule", "update", "--init", "--", *required],
                log=directory / "submodules.log")
    initialized.update(required)


def host_guard(profile, checks, policy):
    host = {"Darwin": "macos", "Linux": "linux", "Windows": "windows"}.get(platform.system())
    arch = {"amd64": "x86_64", "aarch64": "arm64"}.get(platform.machine().lower(), platform.machine().lower())
    wanted_arch = {"amd64": "x86_64", "aarch64": "arm64"}.get(profile["architecture"], profile["architecture"])
    if {"darwin": "macos"}.get(profile["platform"], profile["platform"]) != host or wanted_arch not in {"native", arch}:
        raise RuntimeError(f"selected platform/architecture requires an unimplemented guarded container/native adapter: {profile['platform']}/{wanted_arch}; host={host}/{arch}")
    if any(policy["checks"][x]["kind"] != "ctest" for x in checks):
        raise RuntimeError("selected non-CTest checks are not supported by the shared backend; not bypassed")
    caps = set(profile.get("capabilities", []))
    unsupported = caps - {"display", "private_bus"}
    if unsupported:
        raise RuntimeError(f"selected capabilities require an unimplemented guarded adapter: {sorted(unsupported)}")
    # Never reuse the developer's display/session bus as proof of isolation.
    prefix = []
    if "private_bus" in caps:
        if not shutil.which("dbus-run-session"):
            raise RuntimeError("selected private_bus requires dbus-run-session")
        prefix += ["dbus-run-session", "--"]
    if "display" in caps:
        if not shutil.which("xvfb-run"):
            raise RuntimeError("selected display requires xvfb-run")
        prefix += ["xvfb-run", "-a", "-s", "-screen 0 1280x800x24"]
    # Backend defaults must not silently pick a different compiler family.
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
    return prefix, dict(os.environ, CC=cc, **{k: v for k, v in profile.get("environment", {}).items() if k != "CC"})


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
            prefix, env = host_guard(profile, lane["checks"], policy)
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
                resolved = lane_dir / "resolved.json"
                result = lane_dir / "receipt.json"
                # One private display/bus spans configure and execution: their
                # addresses are part of the backend environment fingerprint.
                # TERM must raise so the backend cleans its command groups.
                run_api = '''import contextlib, pathlib, runpy, signal, sys
def stop(signum, frame):
    raise KeyboardInterrupt()
signal.signal(signal.SIGTERM, stop)
scripts = sys.argv.pop(1)
sys.path.insert(0, scripts)
cli = runpy.run_path(str(pathlib.Path(scripts) / "test-plan.py"))
args = cli["parser"]().parse_args(sys.argv[1:])
directory = pathlib.Path(args.output).parent
resolve = ["resolve", "--repo", args.repo, "--inventory", args.inventory, "--profile", args.profile,
           "--plan", args.plan, "--build-dir", args.build_dir, "--evidence-dir", args.evidence_dir,
           "--configure", "--output", args.resolved]
with (directory / "resolve.log").open("x") as log, contextlib.redirect_stdout(log), contextlib.redirect_stderr(log):
    status = cli["main"](resolve)
if status:
    sys.exit(status)
with (directory / "run.log").open("x") as log, contextlib.redirect_stdout(log), contextlib.redirect_stderr(log):
    sys.exit(cli["main"](sys.argv[1:]))
'''
                command(prefix + [sys.executable, "-c", run_api, source / "scripts", "run", *common,
                                  "--resolved", resolved, "--phase", "all", "--output", result],
                        env=env, log=lane_dir / "lane.log")
                receipts.append(result)
            state["status"] = "passed"
        except (RuntimeError, OSError, subprocess.SubprocessError) as exc:
            state["error"] = str(exc)
            print(f"test-gate: {profile_id}: {exc}", file=sys.stderr, flush=True)
        finally:
            if platform.system() == "Darwin":
                # Read-only, even on failure/cancellation. Never attempt repair.
                command(["bash", guard], log=lane_dir / "keychain-after.log")
    aggregate = shared.aggregate(policy, plan, [shared.read_json(p) for p in receipts], complete=False, repo=source)
    shared.write_json(directory / "aggregate.json", aggregate)
    record["status"] = "partial" if aggregate["passed"] and all(x["status"] == "passed" for x in record["lanes"]) else "failed"


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
    receipt["errors"].append("interrupted; owned child groups stopped; no coverage claimed")
except (RuntimeError, OSError, ValueError, KeyError, shared.ContractError, subprocess.SubprocessError) as exc:
    receipt["errors"].append(str(exc))
finally:
    for source in reversed(worktrees):
        # Submodules make git worktree remove refuse even with --force. A second
        # --force is Git's explicit permission for this private checkout only.
        subprocess.run(["git", "-C", args.repo, "worktree", "remove", "--force", "--force", str(source)],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    if run_dir:
        shared.write_json(run_dir / "adapter-receipt.json", receipt)
        print(f"test-gate: {receipt['status']}; NOT complete CI/push coverage; receipt={run_dir / 'adapter-receipt.json'}", flush=True)
    for error in receipt["errors"]:
        print(f"test-gate: {error}", file=sys.stderr)
sys.exit(130 if receipt["status"] == "cancelled" else 1 if receipt["status"] == "failed" else 0)
PY
