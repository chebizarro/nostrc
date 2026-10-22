#!/usr/bin/env python3
"""Guard existing MDK Docker fixtures; not a selector or acceptance receipt.

run --source-root DIR --evidence-dir DIR --version {08,09,011} [--version ...]
    -- COMMAND [ARGS...]

COMMAND must apply NOSTRC_MDK_CONFIGURE_ARGS (JSON argv) to CMake, then run
existing CTest fixtures/consumers. NOSTRC_MDK_DOCKER and NOSTRC_MDK011_IMAGE
are also provided. The guard spans configure, image setup and all consumers.
This standalone wrapper is not a live custody verifier for selector admission;
environment/config and its final receipt never authorize guarded selector MDK.
Only the original session and recorded containers have scoped cleanup results.
Detached/reparented descendants cannot be proved absent: run refuses lifecycle
success with cleanup_unverified even when the guarded command succeeds.

images --source-root DIR --evidence-dir DIR --version {08,09,011} [--probe]
    builds the original Dockerfile/context snapshots using a unique, explicit
    docker-container Buildx builder. Optional network-disabled hello is only a
    process smoke. This fixed coordinator can attest the owned Docker resource
    scope, never native arbitrary-tree cleanup, selector coverage or MDK parity.
    Output images remain immutable artifacts; the builder, state volume and
    recorded peers must be removed and their client output fully drained.
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
import threading
import time
import uuid

VERSIONS = {"08": ("driver", "nostrc-mdk-interop:0.8.0"),
            "09": ("driver-0.9-probe", "nostrc-mdk-interop:0.9.0-probe"),
            "011": ("driver-0.11", None)}


def write(path, value):
    tmp = path.with_name(path.name + "." + uuid.uuid4().hex)
    tmp.write_text(json.dumps(value, sort_keys=True) + "\n")
    os.replace(tmp, path)


def canonical(value):
    return json.dumps(value, sort_keys=True, separators=(",", ":"), ensure_ascii=True).encode()


def file_identity(path):
    path = Path(path).resolve(strict=True)
    return {"path": str(path), "sha256": hashlib.sha256(path.read_bytes()).hexdigest()}


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
def lock(path, waiting=None, timeout=None):
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
            if timeout is None:
                fcntl.flock(fd, fcntl.LOCK_EX)
            else:
                deadline = time.monotonic() + timeout
                while True:
                    try:
                        fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
                        break
                    except BlockingIOError:
                        if time.monotonic() >= deadline:
                            raise ValueError("owned Docker client did not drain its lock: " + str(path))
                        threading.Event().wait(0.02)
        yield
    finally:
        os.close(fd)


def child_exit(child, block=False):
    # WNOWAIT retains the session leader's PID until owned-session cleanup.
    # Reaping first would permit PID/PGID reuse and hide orphaned grandchildren.
    return os.waitid(os.P_PID, child.pid, os.WEXITED | os.WNOWAIT | (0 if block else os.WNOHANG))


def wait_child(child):
    result = child_exit(child, block=True)
    return result.si_status if result.si_code == os.CLD_EXITED else -result.si_status


def session_members(session, root_pid, group=None):
    active = []
    rows = subprocess.check_output(["ps", "-axo", "pid=,stat="]).decode().splitlines()
    for row in rows:
        raw_pid, status = row.split(None, 1)
        pid = int(raw_pid)
        if pid == root_pid or status.startswith("Z"):
            continue
        try:
            if os.getsid(pid) == session and (group is None or os.getpgid(pid) == group):
                active.append(pid)
        except ProcessLookupError:
            pass
    return active


def stop_original_session(child):
    # This proves nothing about descendants which changed session/reparented.
    # The unreaped leader reserves our original PGID. Signal that owned group,
    # never a name match or a previously reaped/reused PID. Cooperative TERM
    # lets the backend stop its separately owned CTest command groups first.
    if child_exit(child) is None:
        os.killpg(child.pid, signal.SIGTERM)
        ended = threading.Event()
        waiter = threading.Thread(target=lambda: (wait_child(child), ended.set()), daemon=True)
        waiter.start()
        if not ended.wait(10):
            os.killpg(child.pid, signal.SIGKILL)
        waiter.join(timeout=5)
        if waiter.is_alive():
            raise ValueError("owned command did not stop; cleanup unconfirmed")
    members = session_members(child.pid, child.pid, child.pid)
    if members:
        try:
            os.killpg(child.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        except PermissionError:
            # Darwin rejects a zombie-only group. It is benign only when
            # the last active member really exited before this signal.
            if session_members(child.pid, child.pid, child.pid):
                raise
    # Observe actual quiescence, not an arbitrary delay interpreted as success.
    # A different process group surviving in our session is an explicit failure;
    # do not signal an unreserved group/PID that could have been reused.
    deadline = time.monotonic() + 2
    members = session_members(child.pid, child.pid)
    while members and time.monotonic() < deadline:
        members = session_members(child.pid, child.pid)
    child.wait()  # release the reserved leader only after group cleanup/check
    if members:
        raise ValueError("owned session descendants remain; cleanup unconfirmed: " + str(members))


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


RUN_LABEL = "org.nostrc.mdk.run"
CONTEXT_LABEL = "org.nostrc.mdk.context"
VERSION_LABEL = "org.nostrc.mdk.version"


def docker_env(config):
    env = dict(os.environ, BUILDX_CONFIG=str(Path(config["state"]) / "buildx"),
               BUILDX_METADATA_PROVENANCE="max", DOCKER_HOST=config["docker_endpoint"])
    env.pop("DOCKER_CONTEXT", None)
    return env


def cli(config, args, *, timeout=60, log=None):
    child = subprocess.Popen([config["docker"], *args], env=docker_env(config),
                             stdout=subprocess.PIPE, stderr=subprocess.PIPE, start_new_session=True)
    try:
        stdout, stderr = child.communicate(timeout=timeout)
    except subprocess.TimeoutExpired as exc:
        # A client/plugin can retain a pipe after direct-child exit. Reserve its
        # original process group until signalling, and bound the final drain too.
        # Neither client reaping nor eventual absence discharges server uncertainty.
        write(Path(config["state"]) / ("uncertain-" + uuid.uuid4().hex + ".json"),
              {"argv": args, "pid": child.pid, "reason": "Docker API request timed out; server completion unverified"})
        try:
            os.killpg(child.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        try:
            stdout, stderr = child.communicate(timeout=10)
        except subprocess.TimeoutExpired as drain:
            stdout, stderr = drain.stdout or b"", drain.stderr or b""
            child.stdout.close()
            child.stderr.close()
            try:
                child.wait(timeout=1)
            except subprocess.TimeoutExpired:
                pass  # retained uncertainty, never successful cleanup
        if log:
            Path(log).write_bytes(stdout + stderr)
        raise exc
    if log:
        Path(log).write_bytes(stdout + stderr)
    if child.returncode:
        raise ValueError(f"Docker {args[:2]} failed ({child.returncode}): " + stderr.decode(errors="replace"))
    return stdout


def inspect_container(config, identity):
    return json.loads(cli(config, ["container", "inspect", identity]))[0]


def same_daemon(config):
    if cli(config, ["info", "--format", "{{.ID}}" ]).decode().strip() != config["daemon_id"]:
        raise ValueError("Docker daemon identity changed")


def admitted(config):
    state = Path(config["state"])
    if (state / "drain-requested.json").exists():
        raise ValueError("MDK Docker lifecycle admissions revoked")
    if json.loads((state / "phase.json").read_text()) != {"run_id": config["run_id"], "phase": "active"}:
        raise ValueError("MDK Docker lifecycle is draining or closed")
    if file_identity(__file__) != config["helper"]:
        raise ValueError("MDK helper bytes changed during lifecycle")
    for key in ("docker_executable", "buildx_executable", "wrapper"):
        identity = config[key]
        if file_identity(identity["path"]) != identity:
            raise ValueError("MDK executable bytes changed: " + key)
    same_daemon(config)


def stream_cli(config, args, prefix, input_data=None):
    """Forward protocol stdout live, retaining raw bytes and requiring pipe EOF."""
    streams, failures = {}, []
    client_path = Path(config["state"]) / "clients" / (prefix.name + ".json")
    with lock(Path(config["state"]) / "admission.lock"):
        admitted(config)
        write(client_path, {"argv": args, "drained": False, "status": "admitting"})
        child = subprocess.Popen([config["docker"], *args], env=docker_env(config),
                                 stdin=subprocess.PIPE if input_data is not None else None,
                                 stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        write(client_path, {"pid": child.pid, "argv": args, "drained": False})
    def drain(name, source, destination):
        path = Path(str(prefix) + "." + name)
        try:
            with path.open("wb") as output:
                while True:
                    block = source.read1(65536)
                    if not block:
                        break
                    output.write(block)
                    output.flush()
                    try:
                        destination.write(block)
                        destination.flush()
                    except BrokenPipeError:
                        pass
            streams[name] = file_identity(path)
        except (OSError, ValueError) as exc:
            failures.append(str(exc))
        finally:
            source.close()
    readers = [threading.Thread(target=drain, args=(name, source, destination), daemon=True)
               for name, source, destination in (("stdout", child.stdout, sys.stdout.buffer),
                                                 ("stderr", child.stderr, sys.stderr.buffer))]
    for reader in readers:
        reader.start()
    if input_data is not None:
        try:
            child.stdin.write(input_data)
            child.stdin.close()
        except BrokenPipeError:
            child.stdin.close()
    code = child.wait()
    for reader in readers:
        reader.join(timeout=20)
    drained = not failures and all(not reader.is_alive() for reader in readers)
    write(client_path, {"pid": child.pid, "argv": args, "exit_code": code,
                                               "drained": drained, "streams": streams, "errors": failures})
    if not drained:
        raise ValueError("Docker client output did not drain; direct-child exit is insufficient")
    return code


class OwnedBuilder:
    """One private BuildKit namespace; never a native arbitrary-tree authority."""
    def __init__(self, config):
        self.config = config
        self.state = Path(config["state"])
        self.name = "nostrc-mdk-" + config["run_id"]
        self.node = self.name + "-node"
        self.container_name = "buildx_buildkit_" + self.node
        self.volume = self.container_name + "_state"
        self.identity = None
        self.started = False
        self.removal_verified = False
        self.result = {"scope": "original_docker_build_and_recorded_peers_only", "cleanup_complete": False,
                       "builder": self.name, "node": self.node, "container_name": self.container_name,
                       "volume": self.volume, "errors": [], "images": "retained_immutable_outputs_not_running_resources"}

    def list_container(self, name):
        return cli(self.config, ["ps", "-aq", "--no-trunc", "--filter", "name=^/" + name + "$"]).decode().split()

    def list_volume(self):
        return cli(self.config, ["volume", "ls", "-q", "--filter", "name=^" + self.volume + "$"]).decode().split()

    def verify_instance(self):
        path = self.state / "buildx/instances" / self.name
        data = json.loads(path.read_text())
        nodes = data.get("Nodes", [])
        if (data.get("Name") != self.name or data.get("Driver") != "docker-container" or len(nodes) != 1 or
            nodes[0].get("Name") != self.node or nodes[0].get("Endpoint") != self.config["docker_endpoint"] or
            nodes[0].get("DriverOpts", {}).get("env.NOSTRC_MDK_RUN") != self.config["run_id"] or
            nodes[0].get("DriverOpts", {}).get("image") != self.config["builder"]["image_reference"]):
            raise ValueError("private Buildx instance ownership mismatch")
        return file_identity(path)

    def verify_builder(self, current):
        if (current["Name"] != "/" + self.container_name or
            "NOSTRC_MDK_RUN=" + self.config["run_id"] not in current["Config"]["Env"] or
            current["HostConfig"]["RestartPolicy"]["Name"] != "no" or
            current["Image"] != self.config["builder"]["image_id"] or
            current["HostConfig"].get("PidMode", "") not in {"", "private"} or
            current["HostConfig"].get("Binds")):
            raise ValueError("BuildKit container ownership mismatch; refusing name-only cleanup")
        mounts = [m for m in current["Mounts"] if m.get("Destination") == "/var/lib/buildkit"]
        if len(mounts) != 1 or mounts[0].get("Type") != "volume" or mounts[0].get("Name") != self.volume:
            raise ValueError("BuildKit state volume ownership mismatch")
        if self.identity and any(current[key] != self.identity[key] for key in ("Id", "Image")):
            raise ValueError("BuildKit immutable identity changed")
        if self.identity and current["Config"]["Labels"] != self.identity["Config"]["Labels"]:
            raise ValueError("BuildKit labels changed")

    def setup(self):
        config = self.config
        # No daemon/image allocation is authorized without the actual buildx plugin.
        config["buildx_version"] = cli(config, ["buildx", "version"]).decode().strip()
        plugin = next((p for p in json.loads(cli(config, ["info", "--format", "{{json .ClientInfo.Plugins}}" ]))
                       if p.get("Name") == "buildx"), None)
        if plugin is None:
            raise ValueError("actual Buildx executable capability is missing")
        config["buildx_executable"] = file_identity(plugin["Path"])
        if self.list_container(self.container_name) or self.list_volume():
            raise ValueError("fresh BuildKit namespace unexpectedly already exists")
        image = "moby/buildkit:buildx-stable-1"
        found = subprocess.run([config["docker"], "image", "inspect", image], env=docker_env(config),
                               capture_output=True, timeout=30)
        if found.returncode:
            cli(config, ["pull", image], timeout=180, log=self.state / "buildkit-pull.log")
            found = subprocess.run([config["docker"], "image", "inspect", image], env=docker_env(config),
                                   capture_output=True, timeout=30, check=True)
        image_info = json.loads(found.stdout)[0]
        refs = [r for r in image_info["RepoDigests"] if r.startswith("moby/buildkit@sha256:")]
        if len(refs) != 1:
            raise ValueError("BuildKit image lacks an unambiguous immutable repository digest")
        config["builder"] = {"name": self.name, "node": self.node, "image_id": image_info["Id"],
                             "image_reference": refs[0], "container_name": self.container_name, "volume": self.volume}
        self.started = True  # before create/bootstrap, including partial allocation
        write(self.state / "builder.json", self.result)
        cli(config, ["buildx", "create", "--name", self.name, "--node", self.node,
                     "--driver", "docker-container", "--driver-opt", "image=" + refs[0],
                     "--driver-opt", "restart-policy=no", "--driver-opt", "env.NOSTRC_MDK_RUN=" + config["run_id"],
                     config["docker_endpoint"]], log=self.state / "builder-create.log")
        config["builder"]["instance"] = self.verify_instance()
        cli(config, ["buildx", "inspect", self.name, "--bootstrap", "--timeout", "120s"],
            timeout=150, log=self.state / "builder-bootstrap.log")
        # inspect --bootstrap can return zero while its text reports an error.
        current = inspect_container(config, self.container_name)
        self.verify_builder(current)
        if not current["State"]["Running"] or current["Image"] != image_info["Id"]:
            raise ValueError("BuildKit did not start the exact admitted image")
        self.identity = current
        write(self.state / "builder-container.json", current)
        workers = cli(config, ["exec", current["Id"], "buildctl", "debug", "workers"])
        if len(workers.splitlines()) < 2:
            raise ValueError("BuildKit has no ready worker")
        (self.state / "builder-workers.log").write_bytes(workers)
        config["builder"]["container_id"] = current["Id"]

    def cleanup(self):
        config = self.config
        def attempt(action):
            try:
                action()
            except (ValueError, OSError, subprocess.SubprocessError, KeyError) as exc:
                self.result["errors"].append(str(exc))
        def fence():
            # Revoke new work BEFORE waiting for the admitted info/create/inspect
            # sequence to settle. Each API call is bounded; never scan a moving
            # resource inventory after an admission-lock timeout.
            write(self.state / "drain-requested.json", {"run_id": config["run_id"]})
            with lock(self.state / "admission.lock", timeout=240):
                write(self.state / "phase.json", {"run_id": config["run_id"], "phase": "draining"})
        attempt(fence)
        if self.result["errors"]:
            self.result["errors"].append("admissions did not settle; resource removal refused")
            write(self.state / "builder.json", self.result)
            return self.result
        def stop_builder():
            ids = self.list_container(self.container_name)
            if not ids:
                self.removal_verified = self.identity is not None or not self.list_volume()
                return
            if len(ids) != 1:
                raise ValueError("ambiguous BuildKit container")
            current = inspect_container(config, ids[0])
            self.verify_builder(current)
            self.removal_verified = True
            if self.identity is None:
                self.identity = current
                write(self.state / "builder-container.json", current)
            if current["State"]["Running"]:
                cli(config, ["stop", "--time", "1", current["Id"]])
            stopped = inspect_container(config, current["Id"])
            self.verify_builder(stopped)
            if stopped["State"]["Running"] or stopped["State"]["Pid"]:
                raise ValueError("BuildKit process namespace did not stop")
            cli(config, ["logs", "--timestamps", current["Id"]], log=self.state / "builder-final.log")
            self.result["builder_stopped"] = True
        if self.started:
            attempt(stop_builder)
        # Stop/remove peers by immutable CID only after creation admissions drain.
        for path in (self.state / "containers").glob("*.json"):
            def remove_peer(path=path):
                entry = json.loads(path.read_text())
                cid = entry.get("container_id")
                if not cid:
                    raise ValueError("peer create has no confirmed CID: " + entry["name"])
                current = inspect_container(config, cid)
                expected = {RUN_LABEL: config["run_id"], CONTEXT_LABEL: config["images"][entry["version"]]["digest"],
                            VERSION_LABEL: entry["version"]}
                if (current["Id"] != cid or current["Image"] != entry["image_id"] or
                    current["Config"]["Labels"] != entry.get("labels") or any(
                        current["Config"]["Labels"].get(k) != v for k, v in expected.items())):
                    raise ValueError("peer CID/image/labels mismatch; refusing cleanup")
                cli(config, ["rm", "-f", cid])
                with lock(path.with_suffix(".client.lock"), timeout=30):
                    entry = json.loads(path.read_text())
                    if not entry.get("logs_drained"):
                        raise ValueError("peer output writers did not drain")
                    write(path, preserve_artifact(self.state, dict(entry, removed=True)))
                if self.list_container(entry["name"]):
                    raise ValueError("owned peer name remains after removal")
            attempt(remove_peer)
        # A build client may outlive the native parent; require its complete
        # journal and pipe EOF after the owned BuildKit namespace has stopped.
        for version in config["images"]:
            def drain_build(version=version):
                with lock(self.state / (version + ".lock"), timeout=30):
                    path = self.state / (version + "-image.json")
                    if path.exists() and not json.loads(path.read_text()).get("logs_drained"):
                        raise ValueError("build client output did not drain: " + version)
            attempt(drain_build)
        # Every attempt matters. A later successful rebuild must not overwrite
        # an earlier client's missing EOF or corrupt retained stream evidence.
        for path in (self.state / "clients").glob("*.json"):
            def verify_client(path=path):
                entry = json.loads(path.read_text())
                if not entry.get("drained") or set(entry.get("streams", {})) != {"stdout", "stderr"}:
                    raise ValueError("retained Docker client did not drain: " + str(path))
                for identity in entry["streams"].values():
                    if file_identity(identity["path"]) != identity:
                        raise ValueError("retained Docker client stream changed: " + str(path))
            attempt(verify_client)
        if self.started:
            def remove_builder():
                # Recheck identity before letting buildx remove its exact node/state.
                ids = self.list_container(self.container_name)
                if ids:
                    self.verify_builder(inspect_container(config, ids[0]))
                self.verify_instance()
                cli(config, ["buildx", "rm", "--force", self.name], log=self.state / "builder-remove.log")
                if self.list_container(self.container_name) or self.list_volume():
                    raise ValueError("owned BuildKit container/state volume remains")
                if (self.state / "buildx/instances" / self.name).exists():
                    raise ValueError("private Buildx builder registration remains")
            # Never permit buildx's name-only removal after failed identity checks.
            if self.removal_verified:
                attempt(remove_builder)
            else:
                self.result["errors"].append("BuildKit ownership unconfirmed; refusing removal")
        attempt(lambda: same_daemon(config))
        for path in self.state.glob("uncertain-*.json"):
            self.result["errors"].append("unresolved Docker API request: " + str(path))
        def retain_logs():
            self.result["logs"] = [file_identity(path) for path in self.state.glob("builder-*.log")]
        attempt(retain_logs)
        self.result["cleanup_complete"] = not self.result["errors"]
        write(self.state / "phase.json", {"run_id": config["run_id"], "phase": "closed"})
        write(self.state / "builder.json", self.result)
        return self.result


def docker(argv):
    state = Path(os.environ["NOSTRC_MDK_STATE"])
    config = json.loads((state / "config.json").read_text())
    real = config["docker"]
    admitted(config)
    if len(argv) == 4 and argv[:2] == ["build", "-t"]:
        tag, context = argv[2:]
        matches = [(v, d) for v, d in config["images"].items() if d["tag"] == tag]
        if len(matches) != 1:
            raise ValueError(f"unguarded MDK build tag: {tag}")
        version, data = matches[0]
        if Path(context).resolve() != Path(data["context"]) or digest(Path(context)) != data["digest"]:
            raise ValueError("MDK build context differs from admitted snapshot")
        with lock(state / (version + ".lock")):
            OwnedBuilder(config).verify_instance()
            record = dict(data, version=version, build_complete=False)
            target = state / (version + "-image.json")
            write(target, record)  # a failed rebuild invalidates earlier success
            prefix = state / (version + "-build-" + uuid.uuid4().hex)
            iid, metadata = Path(str(prefix) + ".iid"), Path(str(prefix) + ".metadata.json")
            labels = {RUN_LABEL: config["run_id"], CONTEXT_LABEL: data["digest"], VERSION_LABEL: version}
            command = ["buildx", "build", "--builder", config["builder"]["name"], "--load", "--progress", "plain",
                       "--iidfile", str(iid), "--metadata-file", str(metadata), "-t", tag]
            for key, value in labels.items():
                command += ["--label", key + "=" + value]
            command.append(data["build_context"])
            record.update(build_argv=command, labels=labels, logs_drained=False)
            write(target, record)
            try:
                code = stream_cli(config, command, prefix)
                record["build_exit_code"] = code
                if code:
                    return code
                image = iid.read_text().strip()
                meta = json.loads(metadata.read_text())
                if (not re.fullmatch(r"sha256:[0-9a-f]{64}", image) or
                    meta.get("containerimage.digest") != image or
                    meta.get("containerimage.descriptor", {}).get("digest") != image or
                    not meta.get("buildx.build.ref", "").startswith(config["builder"]["name"] + "/" + config["builder"]["node"] + "/")):
                    raise ValueError("Buildx IID/metadata/builder identity mismatch")
                inspected = json.loads(cli(config, ["image", "inspect", image]))[0]
                inspection = Path(str(prefix) + ".inspect.json")
                write(inspection, inspected)
                if inspected["Config"].get("Volumes"):
                    raise ValueError("MDK images with implicit volumes require unsupported volume lifecycle proof")
                if inspected["Id"] not in {image, meta.get("containerimage.config.digest")} or any(
                        inspected["Config"]["Labels"].get(k) != v for k, v in labels.items()):
                    raise ValueError("Docker inspection does not match build ID/labels")
                if digest(Path(context)) != data["digest"] or digest(Path(data["build_context"])) != data["digest"]:
                    raise ValueError("MDK context changed during build")
                record.update(image_id=inspected["Id"], export_digest=image, metadata=file_identity(metadata),
                              iid=file_identity(iid), inspection=file_identity(inspection),
                              image_inspect_sha256=hashlib.sha256(canonical(inspected)).hexdigest(),
                              image_labels=inspected["Config"]["Labels"], build_complete=True)
            finally:
                client = state / "clients" / (prefix.name + ".json")
                record["logs_drained"] = not client.exists() or json.loads(client.read_text()).get("drained", False)
                write(target, record)
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
    # Retain exited peers until scoped cleanup so PID namespace, labels, logs and
    # CID absence can be checked; never infer removal from --rm/client exit.
    options = [arg for arg in argv[1:i] if arg != "--rm"]
    with lock(target.with_suffix(".client.lock")):
        try:
            with lock(state / "admission.lock"):
                admitted(config)
                write(target, record)  # before the synchronous create request
                command = ["create", "--init", "--name", name, "--label", RUN_LABEL + "=" + config["run_id"],
                           "--label", CONTEXT_LABEL + "=" + data["digest"],
                           "--cidfile", str(target.with_suffix(".cid")), *options, image["image_id"]]
                cid = cli(config, command).decode().strip()
                if not re.fullmatch(r"[0-9a-f]{64}", cid) or target.with_suffix(".cid").read_text().strip() != cid:
                    raise ValueError("Docker create CID/output mismatch")
                record.update(container_id=cid, create_argv=command, labels=image["image_labels"])
                write(target, record)  # retain immutable CID even when inspection refuses
                current = inspect_container(config, cid)
                if (current["Id"] != cid or current["Image"] != image["image_id"] or
                    current["Config"]["Labels"].get(RUN_LABEL) != config["run_id"] or
                    current["Config"]["Labels"].get(CONTEXT_LABEL) != data["digest"]):
                    raise ValueError("created peer image/label identity mismatch")
                if current["Config"]["Labels"] != image["image_labels"]:
                    raise ValueError("created peer inherited labels differ from built image")
                inspection = state / (name + ".inspect.json")
                write(inspection, current)
                record.update(container_id=cid, create_argv=command, labels=current["Config"]["Labels"],
                              inspection=file_identity(inspection))
                write(target, record)
            command = ["start", "--attach", *( ["--interactive"] if "-i" in options else []), cid]
            code = stream_cli(config, command, target.with_suffix(""))
            record.update(status="exited", exit_code=code)
            return code
        finally:
            client = state / "clients" / (name + ".json")
            record["logs_drained"] = not client.exists() or json.loads(client.read_text()).get("drained", False)
            write(target, preserve_artifact(state, record))


def retained_records(paths, errors):
    records = []
    for path in paths:
        try:
            records.append(json.loads(path.read_text()))
        except (ValueError, OSError) as exc:
            errors.append("invalid retained journal " + str(path) + ": " + str(exc))
            records.append({"invalid_journal": str(path)})
    return records


def run(args):
    source = Path(args.source_root).resolve(strict=True)
    parent = Path(args.evidence_dir).resolve()
    parent.mkdir(parents=True, exist_ok=True)
    state = parent / uuid.uuid4().hex
    state.mkdir(mode=0o700)
    for name in ("containers", "clients", "contexts"):
        (state / name).mkdir()
    real = shutil.which("docker")
    if not real:
        raise ValueError("Docker required; no unguarded fallback")
    real = str(Path(real).resolve(strict=True))
    daemon = subprocess.check_output([real, "info", "--format", "{{.ID}}"], timeout=30).decode().strip()
    endpoint = os.environ.get("DOCKER_HOST")
    if not endpoint:
        context_name = subprocess.check_output([real, "context", "show"], timeout=30).decode().strip()
        endpoint = json.loads(subprocess.check_output([real, "context", "inspect", context_name,
                            "--format", "{{json .Endpoints.docker}}"], timeout=30))["Host"]
    if not daemon or not endpoint.startswith("unix://"):
        raise ValueError("original MDK ownership requires an identified local Unix Docker endpoint")
    images = {}
    for version in sorted(set(args.version)):
        directory, tag = VERSIONS[version]
        context = source / "tests/interop/mdk" / directory
        if not (context / "Dockerfile").is_file():
            raise ValueError(f"missing MDK context: {context}")
        content = digest(context)
        images[version] = dict(context=str(context), digest=content,
                               tag=tag or "nostrc-mdk-interop:0.11.0-" + content)
    wrapper = state / "docker"
    wrapper.write_text("#!/bin/sh\nexec " + shlex.join([sys.executable, str(Path(__file__).resolve()), "docker"]) + ' "$@"\n')
    wrapper.chmod(0o700)
    configure = ["-DGROUNDHOG_DOCKER:FILEPATH=" + str(wrapper)]
    if "011" in images:
        configure += ["-DMDK011_INTEROP_IMAGE=" + images["011"]["tag"]]
    write(state / "configure-args.json", configure)
    config = dict(schema_version=1, run_id=state.name, docker=real, docker_executable=file_identity(real),
                  docker_endpoint=endpoint, daemon_id=daemon,
                  source=str(source), state=str(state), images=images, helper=file_identity(__file__),
                  wrapper=file_identity(wrapper), configure_args=configure,
                  execution_domain="original_docker_resource_lifecycle_v1" if args.operation == "images" else "native_unattested")
    write(state / "config.json", config)
    owner = OwnedBuilder(config)
    config_digest = hashlib.sha256(canonical(config)).hexdigest()
    print(f"mdk-gate: lifecycle evidence {state}", flush=True)
    status, code = "failed", 1
    child, child_pid, command_code = None, None, None
    critical, cancellation_requested = False, False
    cleanup_results = {"original_session": "not_started", "recorded_containers": "not_started",
                       "arbitrary_process_tree": "not_applicable_docker_only" if args.operation == "images" else "unverified"}
    def cancelled(signum, frame):
        nonlocal cancellation_requested
        cancellation_requested = True
        # Let bounded create/bootstrap API requests finish before cleanup. A
        # killed Docker client alone does not prove the daemon finished creating.
        if not critical:
            raise KeyboardInterrupt()
    handlers = {s: signal.signal(s, cancelled) for s in (signal.SIGTERM, signal.SIGINT)}
    try:
        locks = Path("/tmp") / ("nostrc-mdk-gate-" + str(os.getuid()))
        locks.mkdir(mode=0o700, exist_ok=True)
        info = locks.lstat()
        if not stat.S_ISDIR(info.st_mode) or info.st_uid != os.getuid() or info.st_mode & 0o077:
            raise ValueError("unsafe machine-wide MDK lock directory")
        with contextlib.ExitStack() as stack:
            for version in sorted(set(images) & {"08", "09"}):
                stack.enter_context(lock(locks / (version + ".lock"), state / (version + "-waiting.json")))
            try:
                critical = True
                for version, data in images.items():
                    snapshot = state / "contexts" / version
                    shutil.copytree(data["context"], snapshot)
                    if digest(snapshot) != data["digest"] or digest(Path(data["context"])) != data["digest"]:
                        raise ValueError("MDK context changed while snapshotting")
                    data["build_context"] = str(snapshot)
                same_daemon(config)
                owner.setup()
                write(state / "config.json", config)
                config_digest = hashlib.sha256(canonical(config)).hexdigest()
                write(state / "phase.json", {"run_id": state.name, "phase": "active"})
                critical = False
                if cancellation_requested:
                    raise KeyboardInterrupt()
                if args.operation == "images":
                    command = [sys.executable, str(Path(__file__).resolve()), "images-command"]
                    if args.probe:
                        command.append("--probe")
                else:
                    command = args.command[1:] if args.command[:1] == ["--"] else args.command
                if not command:
                    raise ValueError("missing guarded command")
                env = dict(os.environ, NOSTRC_MDK_STATE=str(state), NOSTRC_MDK_DOCKER=str(wrapper),
                           NOSTRC_MDK_CONFIGURE_ARGS=json.dumps(configure),
                           NOSTRC_MDK011_IMAGE=images.get("011", {}).get("tag", ""))
                child = subprocess.Popen(command, env=env, start_new_session=True)
                child_pid = child.pid
                code = command_code = wait_child(child)
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
                for s in handlers:
                    signal.signal(s, signal.SIG_IGN)
                critical = False
                # Fence admissions, stop Docker namespaces, then drain all owned
                # create/build/attach writers BEFORE terminating the host command.
                docker_result = owner.cleanup()
                if docker_result["errors"]:
                    config.setdefault("cleanup_errors", []).extend(docker_result["errors"])
                entries = retained_records((state / "containers").glob("*.json"), config.setdefault("cleanup_errors", []))
                cleanup_results["recorded_containers"] = "removed" if all(e.get("removed") for e in entries) else "failed"
                if child is not None:
                    cleanup_results["original_session"] = "unverified"
                    try:
                        stop_original_session(child)
                        cleanup_results["original_session"] = "quiescent"
                    except (ValueError, OSError, subprocess.SubprocessError) as exc:
                        config.setdefault("cleanup_errors", []).append(str(exc))
                    if command_code is None:
                        command_code = child.returncode
                if status == "command_succeeded":
                    if config.get("cleanup_errors"):
                        status, code = "cleanup_failed", 1
                    elif args.operation == "images":
                        status, code = "docker_lifecycle_succeeded", 0
                    else:
                        status, code = "cleanup_unverified", 1
                        print("mdk-gate: arbitrary process-tree cleanup unverified; refusing lifecycle success", file=sys.stderr)
    except KeyboardInterrupt:
        status, code = "cancelled", 130
    except (ValueError, OSError, KeyError, subprocess.SubprocessError) as exc:
        config["error"] = str(exc)
        status, code = ("cancelled", 130) if cancellation_requested else ("failed", 1)
    finally:
        for s, handler in handlers.items():
            signal.signal(s, handler)
        errors = config.setdefault("cleanup_errors", [])
        image_results = retained_records(state.glob("*-image.json"), errors)
        containers = retained_records((state / "containers").glob("*.json"), errors)
        config.update(config_digest=config_digest, supervisor_pid=os.getpid(), child_pid=child_pid,
                      command_exit_code=command_code, cancellation_requested=cancellation_requested,
                      command_status=("not_started" if child_pid is None else "cancelled" if status == "cancelled" else
                                      "succeeded" if command_code == 0 else "failed"),
                      docker_lifecycle=owner.result,
                      cleanup_complete=(args.operation == "images" and owner.result["cleanup_complete"] and
                                        not config.get("cleanup_errors")), cleanup_results=cleanup_results,
                      cleanup_limitation=("Docker resource scope only; never selector/MDK parity acceptance" if args.operation == "images" else
                                          "No arbitrary-tree ownership proof across session changes/reparenting"),
                      status=status, exit_code=code, complete=False,
                      image_results=image_results, containers=containers)
        write(state / "receipt.json", config)
    return code


def main():
    if sys.argv[1:2] == ["docker"]:
        return docker(sys.argv[2:])
    if sys.argv[1:2] == ["images-command"]:
        # Fixed trusted coordinator: no caller-supplied native command. The
        # optional hello is a process smoke, NOT MDK interoperability/parity.
        config = json.loads((Path(os.environ["NOSTRC_MDK_STATE"]) / "config.json").read_text())
        for data in config["images"].values():
            code = docker(["build", "-t", data["tag"], data["context"]])
            if code:
                return code
            if sys.argv[2:] == ["--probe"]:
                result = subprocess.run([os.environ["NOSTRC_MDK_DOCKER"], "run", "--rm", "-i", "--network", "none", data["tag"]],
                                        input=b'{"id":1,"cmd":"hello"}\n')
                if result.returncode:
                    return result.returncode
        return 0
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="operation", required=True)
    for operation in ("run", "images"):
        gate = sub.add_parser(operation)
        gate.add_argument("--source-root", required=True)
        gate.add_argument("--evidence-dir", required=True)
        gate.add_argument("--version", choices=VERSIONS, action="append", required=True)
        if operation == "run":
            gate.add_argument("command", nargs=argparse.REMAINDER)
        else:
            gate.add_argument("--probe", action="store_true", help="Run network-disabled hello process smoke; never parity acceptance")
    return run(parser.parse_args())


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (ValueError, OSError, KeyError, subprocess.SubprocessError) as exc:
        print(f"mdk-gate: {exc}", file=sys.stderr)
        sys.exit(1)