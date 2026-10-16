#!/usr/bin/env python3
"""Guard existing MDK Docker fixtures; not a selector or acceptance receipt.

run --source-root DIR --evidence-dir DIR --version {08,09,011} [--version ...]
    -- COMMAND [ARGS...]

COMMAND must apply NOSTRC_MDK_CONFIGURE_ARGS (JSON argv) to CMake, then run
existing CTest fixtures/consumers. NOSTRC_MDK_DOCKER and NOSTRC_MDK011_IMAGE
are also provided. The guard spans configure, image setup and all consumers.
"""
import argparse
import contextlib
import fcntl
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import signal
import stat
import subprocess
import sys
import uuid

VERSIONS = {"08": ("driver", "nostrc-mdk-interop:0.8.0"),
            "09": ("driver-0.9-probe", "nostrc-mdk-interop:0.9.0-probe"),
            "011": ("driver-0.11", None)}


def write(path, value):
    tmp = path.with_name(path.name + "." + uuid.uuid4().hex)
    tmp.write_text(json.dumps(value, sort_keys=True) + "\n")
    os.replace(tmp, path)


def digest(root):
    result = hashlib.sha256()
    for path in sorted(root.rglob("*")):
        if path.is_symlink() or not (path.is_dir() or path.is_file()):
            raise ValueError(f"unsupported MDK context entry: {path}")
        if path.is_file():
            result.update(os.fsencode(path.relative_to(root)) + b"\0")
            result.update(hashlib.sha256(path.read_bytes()).digest())
    return result.hexdigest()


@contextlib.contextmanager
def lock(path, waiting=None):
    fd = os.open(path, os.O_CREAT | os.O_RDWR | os.O_NOFOLLOW, 0o600)
    try:
        info = os.fstat(fd)
        if not stat.S_ISREG(info.st_mode) or info.st_uid != os.getuid() or info.st_mode & 0o077:
            raise ValueError(f"unsafe lock: {path}")
        try:
            fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            if waiting is not None:
                write(waiting, {"lock": str(path), "status": "waiting"})
            fcntl.flock(fd, fcntl.LOCK_EX)
        yield
    finally:
        os.close(fd)


def stop_tree(child):
    if child.poll() is not None:
        return  # never signal a reaped PID that the OS could have reused
    # CTest creates its own process groups. Capture descendants before signalling
    # and reparenting; never match executable names or signal sibling sessions.
    rows = subprocess.check_output(["ps", "-axo", "pid=,ppid="]).decode().splitlines()
    children = {}
    for row in rows:
        pid, ppid = map(int, row.split())
        children.setdefault(ppid, []).append(pid)
    found = []
    def visit(pid):
        for entry in children.get(pid, []):
            visit(entry)
        found.append(pid)
    visit(child.pid)
    for pid in found:
        try:
            os.kill(pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
    child.wait()


def preserve_artifact(state, record):
    source = record.get("artifact_source")
    if source and Path(source).is_file():
        if Path(source).is_symlink():
            raise ValueError("MDK vectors artifact must not be a symlink")
        target = state / "containers" / (record["name"] + "-vectors.jsonl")
        if not target.exists():
            shutil.copyfile(source, target)
        record["artifact"] = str(target)
        record["artifact_sha256"] = hashlib.sha256(target.read_bytes()).hexdigest()
    return record


def docker(argv):
    state = Path(os.environ["NOSTRC_MDK_STATE"])
    config = json.loads((state / "config.json").read_text())
    real = config["docker"]
    if len(argv) == 4 and argv[:2] == ["build", "-t"]:
        tag, context = argv[2:]
        matches = [(v, d) for v, d in config["images"].items() if d["tag"] == tag]
        if len(matches) != 1:
            raise ValueError(f"unguarded MDK build tag: {tag}")
        version, data = matches[0]
        if Path(context).resolve() != Path(data["context"]) or digest(Path(context)) != data["digest"]:
            raise ValueError("MDK build context differs from admitted snapshot")
        with lock(state / (version + ".lock")):
            record = dict(data, version=version, build_complete=False)
            target = state / (version + "-image.json")
            write(target, record)  # a failed rebuild invalidates earlier success
            iid = state / (version + "-" + uuid.uuid4().hex + ".iid")
            code = subprocess.call([real, "build", "--iidfile", str(iid), "-t", tag, context])
            if code:
                return code
            image = iid.read_text().strip()
            if not re.fullmatch(r"sha256:[0-9a-f]{64}", image):
                raise ValueError("Docker build did not return an immutable image ID")
            if digest(Path(context)) != data["digest"]:
                raise ValueError("MDK context changed during build")
            actual = subprocess.check_output([real, "image", "inspect", "--format", "{{.Id}}", image]).decode().strip()
            if actual != image:
                raise ValueError("Docker inspection does not match build ID")
            write(target, dict(record, image_id=image, build_complete=True))
        return 0
    if not argv or argv[0] != "run":
        raise ValueError("only existing MDK build/run command shapes are supported")
    i = 1
    while i < len(argv) and argv[i].startswith("-"):
        if argv[i] in ("--rm", "-i"):
            i += 1
        elif argv[i] in ("-e", "-v", "--network", "--user") and i + 1 < len(argv):
            i += 2
        else:
            raise ValueError(f"unsupported MDK run option: {argv[i]}")
    if i != len(argv) - 1:
        raise ValueError("MDK driver image/command shape changed")
    matches = [(v, d) for v, d in config["images"].items() if d["tag"] == argv[i]]
    if len(matches) != 1:
        raise ValueError(f"unguarded MDK run tag: {argv[i]}")
    version, data = matches[0]
    with lock(state / (version + ".lock")):
        image = json.loads((state / (version + "-image.json")).read_text())
        if not image.get("build_complete") or image["digest"] != data["digest"]:
            raise ValueError("MDK image build is incomplete or stale")
        if digest(Path(data["context"])) != data["digest"]:
            raise ValueError("MDK context changed after build")
    name = "nostrc-mdk-" + config["run_id"] + "-" + uuid.uuid4().hex
    record = dict(name=name, version=version, image_id=image["image_id"], status="starting")
    for n, arg in enumerate(argv[:i]):
        if arg == "-v" and n + 1 < i and argv[n + 1].endswith(":/artifacts"):
            record["artifact_source"] = str(Path(argv[n + 1][:-len(":/artifacts")]) / "vectors.jsonl")
    target = state / "containers" / (name + ".json")
    write(target, record)  # before Docker create, including cancellation races
    command = [real, "run", "--init", "--name", name, "--label", "org.nostrc.mdk.run=" + config["run_id"],
               "--cidfile", str(target.with_suffix(".cid")), *argv[1:i], image["image_id"]]
    code = subprocess.call(command)
    write(target, preserve_artifact(state, dict(record, status="exited", exit_code=code)))
    return code


def run(args):
    source = Path(args.source_root).resolve(strict=True)
    parent = Path(args.evidence_dir).resolve()
    parent.mkdir(parents=True, exist_ok=True)
    state = parent / uuid.uuid4().hex
    state.mkdir(mode=0o700)
    (state / "containers").mkdir()
    real = shutil.which("docker")
    if not real:
        raise ValueError("Docker required; no unguarded fallback")
    subprocess.run([real, "info"], check=True, stdout=subprocess.DEVNULL)
    images = {}
    for version in sorted(set(args.version)):
        directory, tag = VERSIONS[version]
        context = source / "tests/interop/mdk" / directory
        if not (context / "Dockerfile").is_file():
            raise ValueError(f"missing MDK context: {context}")
        content = digest(context)
        images[version] = dict(context=str(context), digest=content,
                               tag=tag or "nostrc-mdk-interop:0.11.0-" + content)
    config = dict(schema_version=1, run_id=state.name, docker=real, source=str(source), images=images)
    write(state / "config.json", config)
    wrapper = state / "docker"
    wrapper.write_text("#!/bin/sh\nexec " + shlex.join([sys.executable, str(Path(__file__).resolve()), "docker"]) + ' "$@"\n')
    wrapper.chmod(0o700)
    configure = ["-DGROUNDHOG_DOCKER:FILEPATH=" + str(wrapper)]
    if "011" in images:
        configure += ["-DMDK011_INTEROP_IMAGE=" + images["011"]["tag"]]
    write(state / "configure-args.json", configure)
    env = dict(os.environ, NOSTRC_MDK_STATE=str(state), NOSTRC_MDK_DOCKER=str(wrapper),
               NOSTRC_MDK_CONFIGURE_ARGS=json.dumps(configure),
               NOSTRC_MDK011_IMAGE=images.get("011", {}).get("tag", ""))
    print(f"mdk-gate: lifecycle evidence {state}", flush=True)
    status, code = "failed", 1
    def cancelled(signum, frame):
        raise KeyboardInterrupt()
    handlers = {s: signal.signal(s, cancelled) for s in (signal.SIGTERM, signal.SIGINT)}
    try:
        locks = Path("/tmp") / ("nostrc-mdk-gate-" + str(os.getuid()))
        locks.mkdir(mode=0o700, exist_ok=True)
        info = locks.lstat()
        if not stat.S_ISDIR(info.st_mode) or info.st_uid != os.getuid() or info.st_mode & 0o077:
            raise ValueError("unsafe machine-wide MDK lock directory")
        with contextlib.ExitStack() as stack:
            # Fixed order avoids deadlock. Hold through setup AND all consumers.
            for version in sorted(set(images) & {"08", "09"}):
                stack.enter_context(lock(locks / (version + ".lock"), state / (version + "-waiting.json")))
            command = args.command[1:] if args.command[:1] == ["--"] else args.command
            if not command:
                raise ValueError("missing guarded command")
            child = subprocess.Popen(command, env=env, start_new_session=True)
            try:
                code = child.wait()
                if code == 0:
                    for version in images:
                        image = json.loads((state / (version + "-image.json")).read_text())
                        if not image.get("build_complete"):
                            raise ValueError("command succeeded without completed MDK image build")
                    for path in (state / "containers").glob("*.json"):
                        container = json.loads(path.read_text())
                        if container.get("status") != "exited" or container.get("exit_code") != 0:
                            raise ValueError("guarded command concealed a failed/incomplete MDK consumer")
                    status = "command_succeeded"
            finally:
                # Before releasing fixed-tag locks, stop only owned descendants
                # and containers, even when cancellation interrupts Docker create.
                for s in handlers:
                    signal.signal(s, signal.SIG_IGN)
                stop_tree(child)
                for path in (state / "containers").glob("*.json"):
                    entry = json.loads(path.read_text())
                    removed = subprocess.run([real, "rm", "-f", entry["name"]],
                                             stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
                    if removed.returncode:
                        # --rm normally removed an exited peer already. Confirm
                        # absence with the daemon; an unavailable daemon is not
                        # silent successful cleanup.
                        remaining = subprocess.run([real, "ps", "-aq", "--filter", "name=^/" + entry["name"] + "$"],
                                                   capture_output=True, text=True)
                        if remaining.returncode or remaining.stdout.strip():
                            config.setdefault("cleanup_errors", []).append(entry["name"])
                    write(path, preserve_artifact(state, entry))
                if config.get("cleanup_errors"):
                    status, code = "cleanup_failed", 1
    except KeyboardInterrupt:
        status, code = "cancelled", 130
    except (ValueError, OSError, subprocess.SubprocessError) as exc:
        config["error"] = str(exc)
        status, code = "failed", 1
    finally:
        for s, handler in handlers.items():
            signal.signal(s, handler)
        config.update(status=status, exit_code=code, complete=False,
                      image_results=[json.loads(p.read_text()) for p in state.glob("*-image.json")],
                      containers=[json.loads(p.read_text()) for p in (state / "containers").glob("*.json")])
        write(state / "receipt.json", config)
    return code


def main():
    if sys.argv[1:2] == ["docker"]:
        return docker(sys.argv[2:])
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="operation", required=True)
    gate = sub.add_parser("run")
    gate.add_argument("--source-root", required=True)
    gate.add_argument("--evidence-dir", required=True)
    gate.add_argument("--version", choices=VERSIONS, action="append", required=True)
    gate.add_argument("command", nargs=argparse.REMAINDER)
    return run(parser.parse_args())


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (ValueError, OSError, KeyError, subprocess.SubprocessError) as exc:
        print(f"mdk-gate: {exc}", file=sys.stderr)
        sys.exit(1)