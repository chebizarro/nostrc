"""Owned, unprivileged Linux containers; never arbitrary native-tree custody.

The daemon is an existing operator capability. Only immutable image/container
IDs are admitted; no workload receives its socket or a host PID/network domain.
Successful cleanup is independent of successful workload/coverage.
"""
import contextlib
import hashlib
import json
import os
from pathlib import Path
import re
import signal
import socket
import sys
import subprocess
import threading
import time
import uuid


class CustodyError(RuntimeError):
    pass


def canonical(value):
    return json.dumps(value, sort_keys=True, separators=(",", ":"), ensure_ascii=True).encode()


def digest(value):
    return hashlib.sha256(canonical(value)).hexdigest()


def file_hash(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def write_json(path, value):
    path = Path(path)
    temporary = path.with_name(path.name + "." + uuid.uuid4().hex)
    temporary.write_bytes(canonical(value) + b"\n")
    os.replace(temporary, path)


def immutable(value):
    if not re.fullmatch(r"sha256:[0-9a-f]{64}", value):
        raise CustodyError("immutable sha256 image ID required")
    return value


def context_hash(root):
    root = Path(root)
    entries = []
    for path in sorted(root.rglob("*")):
        if path.is_symlink() or not (path.is_file() or path.is_dir()):
            raise CustodyError("unsupported peer context entry: " + str(path))
        if path.is_file():
            entries.append([path.relative_to(root).as_posix(), path.stat().st_mode & 0o777, file_hash(path)])
    return digest(entries)


def verify_final_receipt(receipt, binding):
    """Offline structural verifier; producer alone performs actual daemon checks."""
    if not isinstance(receipt, dict) or receipt.get("binding") != binding or receipt.get("binding_hash") != digest(binding):
        raise CustodyError("stale contained execution binding")
    if (receipt.get("status") != "succeeded" or receipt.get("container_exit_code") != 0
            or receipt.get("cleanup_complete") is not True or receipt.get("rpc_indeterminate") is not False
            or receipt.get("cleanup_errors") or receipt.get("container_id") != receipt.get("removed_container_id")
            or not re.fullmatch(r"[0-9a-f]{64}", receipt.get("container_id", ""))):
        raise CustodyError("contained execution did not succeed and finalize")
    if receipt.get('inspect_hash') != digest(receipt['initial_inspect']):
        raise CustodyError('initial isolation inspection hash differs')
    owner = object.__new__(ContainerOwner)
    owner.receipt = receipt
    for key in ("initial_inspect", "cleanup_inspect"):
        value = receipt[key]
        if value["Id"] != receipt["container_id"] or value["Config"]["Labels"].get("org.nostrc.custody.run") != receipt["run_id"]:
            raise CustodyError("contained inspect identity mismatch")
        owner.validate_isolation(value)
    ended = receipt["cleanup_inspect"]["State"]
    if ended["Running"] or ended["Pid"] != 0 or ended["ExitCode"] != 0:
        raise CustodyError("namespace exit was not attested")
    if (not receipt.get("logs") or "attach.raw" not in receipt["logs"]
            or any(not re.fullmatch(r'[0-9a-f]{64}', h) for h in receipt['logs'].values())):
        raise CustodyError("contained raw log drain missing")


def validate_lifecycle(lifecycle, plan, profile_id, profile, fingerprint):
    if not isinstance(lifecycle, dict):
        raise CustodyError('final contained lifecycle missing')
    receipt = lifecycle['receipt']
    if lifecycle['receipt_sha256'] != hashlib.sha256(canonical(receipt) + b'\n').hexdigest():
        raise CustodyError('final contained lifecycle hash differs')
    binding = receipt['binding']
    for key, expected in {'source_identity': plan['source_identity'], 'plan_id': plan['plan_id'],
                          'inventory_hash': plan['inventory_hash'], 'profile_id': profile_id,
                          'profile_hash': digest(profile)}.items():
        if binding[key] != expected:
            raise CustodyError('final contained binding differs: ' + key)
    if sorted(binding['peers']) != sorted(profile.get('prepared_peers', [])) or receipt['network'] != 'none':
        raise CustodyError('final contained peer/runtime domain differs')
    verify_final_receipt(receipt, binding)
    state = lifecycle['runtime_state']
    if (state['status'] != 'succeeded' or state['container_id'] != receipt['container_id']
            or state['run_id'] != receipt['run_id'] or state['realization_fingerprint'] != fingerprint
            or state['effective_configure_args'] != profile['configure_args']
            or state['phase_context_hash'] != digest(state['phase_context'])):
        raise CustodyError('contained runtime/cache/profile execution did not finalize')


class PreparedContext:
    """Live runner-local admission, never an environment/config fallback.

    A final host-owned container receipt remains mandatory for aggregation.
    This object cannot confer original Docker fixture or native-host authority.
    """
    def __init__(self, control):
        if sys.platform != "linux" or os.getuid() == 0:
            raise CustodyError("prepared runtime requires a non-root Linux namespace")
        cid = control["container_id"]
        if not re.fullmatch(r"[0-9a-f]{64}", cid) or socket.gethostname() != cid[:12]:
            raise CustodyError("runtime container hostname/identity mismatch")
        if Path('/proc/1/comm').read_text().strip() not in {"docker-init", "tini"}:
            raise CustodyError("runtime private PID namespace init missing")
        owner = object.__new__(ContainerOwner)
        owner.receipt = control["owner"]
        if owner.receipt["container_id"] != cid or owner.receipt["network"] != "none":
            raise CustodyError("runtime owner/container binding mismatch")
        owner.validate_isolation(owner.receipt["initial_inspect"])
        self.control = json.loads(canonical(control))
        self.namespace = os.readlink('/proc/self/ns/pid')

    def validate(self, resolved):
        binding = self.control["binding"]
        if os.readlink('/proc/self/ns/pid') != self.namespace:
            raise CustodyError("runtime PID namespace changed")
        for key in ("source_identity", "inventory_hash", "profile_id", "plan_id"):
            if resolved[key] != binding[key]:
                raise CustodyError("runtime resolved binding differs: " + key)
        profile = self.control["profile"]
        if resolved["profile_hash"] != digest(profile):
            raise CustodyError("runtime effective profile hash differs")
        if profile.get("execution_domain") != "prepared_peer_linux_v1":
            raise CustodyError("contained authority cannot authorize an original profile")
        versions = profile.get("prepared_peers", [])
        if sorted(versions) != sorted(self.control["peers"]):
            raise CustodyError("runtime peer projection differs")
        if 'docker' in resolved.get('required_capabilities', []):
            raise CustodyError('prepared runtime has no Docker capability; native peers are external_peer, not Docker')
        if {'external_peer', 'prepared_peer'} & set(resolved.get('required_capabilities', [])) and not versions:
            raise CustodyError('selected peer capability has no actual prepared peer')
        image_tests = {'groundhog-mdk-interop-image', 'groundhog-mdk011-interop-image', 'groundhog-mdk011-interop-image-mdk09'}
        if image_tests & set(resolved.get('tests', [])):
            raise CustodyError('original Docker image fixture selected in prepared route')
        self.cache_values = {}
        if versions:
            cache = Path(resolved['build_dir']) / 'CMakeCache.txt'
            lines = cache.read_text().splitlines()
            names = {'08': 'MDK_INTEROP_DRIVER', '09': 'MDK09_PROBE_DRIVER', '011': 'MDK011_INTEROP_DRIVER'}
            for version in versions:
                expected = names[version] + ':STRING=/peers/' + version + '/driver'
                if lines.count(expected) != 1:
                    raise CustodyError('prepared native driver cache differs: ' + version)
                self.cache_values[names[version]] = '/peers/' + version + '/driver'
        for version, peer in self.control["peers"].items():
            if file_hash('/peers/' + version + '/driver') != peer["binary_sha256"]:
                raise CustodyError("prepared runtime peer bytes changed")

    def audit_inputs(self, realization):
        """Admit only inspected read-only native peer bytes, not arbitrary externals."""
        profile = self.control['profile']
        if realization['profile'] != profile:
            raise CustodyError('runtime audit profile differs')
        self.validate({**{key: realization[key] for key in ('source_identity', 'inventory_hash', 'profile_id', 'build_dir')},
            'plan_id': self.control['binding']['plan_id'], 'profile_hash': digest(profile),
            'required_capabilities': profile.get('capabilities', []),
            'tests': [test['name'] for test in realization['tests']]})
        return {'/peers/' + version + '/driver': peer for version, peer in self.control['peers'].items()}


class Cancel:
    """Signals request cancellation; they never interrupt ownership acquisition."""
    def __init__(self):
        self.requested = threading.Event()
        self.wakeup = threading.Event()

    @contextlib.contextmanager
    def handlers(self):
        def request(signum, frame):
            self.requested.set()
            self.wakeup.set()
        previous = {s: signal.signal(s, request) for s in (signal.SIGINT, signal.SIGTERM)}
        try:
            yield self
        finally:
            for sig, handler in previous.items():
                signal.signal(sig, handler)


class ContainerOwner:
    """Single-use owner. Container exit, log drain and removal are all required.

    Control RPC failure is indeterminate (a daemon request may still complete).
    It cannot produce a positive cleanup attestation, even after a later absence
    check. Workload cancellation is delivered only through the owned container.
    """
    def __init__(self, directory, *, docker="docker", cancel=None, grace=15, rpc_timeout=60):
        self.directory = Path(directory).resolve()
        self.directory.mkdir(parents=True, mode=0o700, exist_ok=False)
        self.docker = docker
        self.cancel = cancel or Cancel()
        self.grace = grace
        self.rpc_timeout = rpc_timeout
        self.cid = None
        self.attach = None
        self.on_running = None
        self.running = threading.Event()
        self.indeterminate = False
        self.receipt = {"schema_version": 1, "run_id": uuid.uuid4().hex,
                        "scope": "linux_container_v1", "status": "not_started",
                        "cleanup_complete": False, "complete": False,
                        "diagnostic_eligible": False, "complete_gate_eligible": False}
        self.name = "nostrc-contained-" + self.receipt["run_id"]

    def rpc(self, *argv):
        try:
            result = subprocess.run([self.docker, *argv], capture_output=True, timeout=self.rpc_timeout)
            if result.returncode:
                raise CustodyError("Docker RPC failed: " + repr(argv) + ": " +
                                   result.stderr.decode("utf-8", "surrogateescape"))
            return result.stdout
        except (OSError, subprocess.TimeoutExpired, CustodyError):
            self.indeterminate = True
            raise

    def inspect(self):
        value = json.loads(self.rpc("inspect", self.cid))[0]
        if value["Id"] != self.cid or value["Config"]["Labels"].get("org.nostrc.custody.run") != self.receipt["run_id"]:
            raise CustodyError("container identity/ownership mismatch")
        return value

    def create(self, image, argv, *, mounts=(), network="none", environment=None, workdir="/work"):
        immutable(image)
        if os.getuid() == 0:
            raise CustodyError("run this bounded route as a non-root operator")
        if network not in ("none", "bridge"):
            raise CustodyError("only private runtime none / explicit preparation bridge networks supported")
        if not argv or not all(isinstance(x, str) and "\0" not in x for x in argv):
            raise CustodyError("explicit argv required")
        inspected_image = json.loads(self.rpc("image", "inspect", image))[0]
        if inspected_image["Id"] != image or inspected_image["Os"] != "linux":
            raise CustodyError("image must be the exact immutable Linux image ID")
        args = ["create", "--init", "--name", self.name, "--label",
                "org.nostrc.custody.run=" + self.receipt["run_id"],
                "--network", network, "--cap-drop", "ALL", "--security-opt", "no-new-privileges=true",
                "--read-only", "--pids-limit", "512", "--user", f"{os.getuid()}:{os.getgid()}",
                "--tmpfs", "/tmp:rw,nosuid,nodev,size=1g", "--workdir", workdir,
                "--entrypoint", argv[0]]
        # A numeric non-root UID needs an NSS identity for dbus-run-session.
        # These are generated container-only files, never the developer's /etc.
        identity = self.directory / "identity"
        identity.mkdir()
        (identity / "passwd").write_text(f"root:x:0:0:root:/root:/bin/sh\nrunner:x:{os.getuid()}:{os.getgid()}:runner:/tmp/home:/bin/sh\nnobody:x:65534:65534:nobody:/nonexistent:/usr/sbin/nologin\n")
        (identity / "group").write_text(f"root:x:0:\nrunner:x:{os.getgid()}:\nnogroup:x:65534:\n")
        mounts = [*mounts, (identity / "passwd", "/etc/passwd", True), (identity / "group", "/etc/group", True)]
        normalized = []
        for source, target, readonly in mounts:
            source = Path(source).resolve(strict=True)
            if ":" in str(source) or ":" in target or not target.startswith("/"):
                raise CustodyError("unsupported mount path")
            normalized.append({"Source": str(source), "Destination": target, "RW": not readonly})
            args += ["--mount", f"type=bind,source={source},target={target},readonly={'true' if readonly else 'false'}"]
        for key, value in sorted((environment or {}).items()):
            if not re.fullmatch(r"[A-Z_][A-Z0-9_]*", key) or not isinstance(value, str) or "\0" in value:
                raise CustodyError("invalid explicit environment")
            args += ["--env", key + "=" + value]
        args += [image, *argv[1:]]
        self.receipt.update(image_id=image, image_architecture=inspected_image["Architecture"],
                            argv=argv, environment=environment or {}, mounts=normalized,
                            network=network, status="creating", create_argv=args)
        write_json(self.directory / "intent.json", self.receipt)
        self.cid = self.rpc(*args).decode().strip()
        if not re.fullmatch(r"[0-9a-f]{64}", self.cid):
            self.indeterminate = True
            raise CustodyError("Docker create did not return a full container ID")
        self.receipt["container_id"] = self.cid
        value = self.inspect()
        self.validate_isolation(value)
        self.receipt["initial_inspect"] = value
        self.receipt["inspect_hash"] = digest(value)
        write_json(self.directory / "created.json", self.receipt)
        return self.cid

    def validate_isolation(self, value):
        host, config = value["HostConfig"], value["Config"]
        actual = sorted((m["Source"], m["Destination"], m["RW"]) for m in value["Mounts"])
        expected = sorted((m["Source"], m["Destination"], m["RW"]) for m in self.receipt["mounts"])
        checks = [value["Image"] == self.receipt["image_id"],
                  host["NetworkMode"] == self.receipt["network"], host["PidMode"] == "",
                  host["IpcMode"] == "private", not host.get('GroupAdd'),
                  set(value.get('NetworkSettings', {}).get('Networks', {})) == {self.receipt['network']},
                  not host["Privileged"], host["ReadonlyRootfs"],
                  set(host["CapDrop"] or []) == {"ALL"}, not host["CapAdd"],
                  set(host["SecurityOpt"] or []) == {"no-new-privileges=true"},
                  config["User"] == f"{os.getuid()}:{os.getgid()}", host["Init"] is True,
                  not host["Devices"], not host["PortBindings"], actual == expected,
                  config["Entrypoint"] == [self.receipt["argv"][0]],
                  config["Cmd"] == (self.receipt["argv"][1:] or None)]
        if not all(checks):
            raise CustodyError("actual container isolation differs from admitted configuration")

    def execute(self, *, timeout):
        if not self.cid:
            raise CustodyError("container not created")
        finished = threading.Event()
        with (self.directory / "attach.raw").open("xb") as output:
            if self.cancel.requested.is_set():
                raise InterruptedError("cancelled before start")
            self.rpc("start", self.cid)
            self.attach = subprocess.Popen([self.docker, "logs", "--follow", self.cid], stdout=output, stderr=subprocess.STDOUT)
            self.running.set()
            if self.on_running is not None:
                self.on_running(self)
            def wait():
                self.attach.wait()
                finished.set()
                self.cancel.wakeup.set()
            waiter = threading.Thread(target=wait, daemon=True)
            waiter.start()
            # Event waits are readiness/cancellation boundaries, never success by delay.
            self.cancel.wakeup.wait(timeout)
            cancelled = self.cancel.requested.is_set()
            timed_out = not cancelled and not finished.is_set()
            if cancelled or timed_out:
                current = self.inspect()
                if current["State"]["Running"]:
                    self.rpc("kill", "--signal", "TERM", self.cid)
                    if not finished.wait(self.grace):
                        self.rpc("kill", "--signal", "KILL", self.cid)
            if not finished.wait(self.rpc_timeout):
                self.indeterminate = True
                raise CustodyError("container attach did not drain")
            waiter.join()
        value = self.inspect()
        self.validate_isolation(value)
        if value["State"]["Running"] or value["State"]["Pid"] != 0:
            raise CustodyError("container has not reached namespace exit")
        code = int(self.rpc("wait", self.cid).strip())
        if code != value["State"]["ExitCode"] or self.attach.returncode != 0:
            raise CustodyError("container wait/inspect exit mismatch or log drain failed")
        self.receipt.update(final_inspect=value, container_exit_code=code, attach_exit_code=self.attach.returncode,
                            status="cancelled" if cancelled else "timed_out" if timed_out else "succeeded" if code == 0 else "failed")
        (self.directory / "daemon.raw").write_bytes(self.rpc("logs", self.cid))
        # attach merges stderr; daemon logs stdout is separate evidence, never its replacement.
        self.receipt["logs"] = {p.name: file_hash(p) for p in self.directory.glob("*.raw")}
        return 130 if cancelled else 124 if timed_out else code

    def finalize(self):
        errors = []
        if self.cid:
            try:
                current = self.inspect()
                self.validate_isolation(current)
                if current["State"]["Running"]:
                    self.rpc("kill", "--signal", "KILL", self.cid)
                code = int(self.rpc("wait", self.cid).strip())
                ended = self.inspect()
                if ended["State"]["Running"] or ended["State"]["Pid"] != 0 or code != ended["State"]["ExitCode"]:
                    raise CustodyError("namespace exit unconfirmed")
                if self.attach is not None:
                    self.attach.wait(timeout=self.rpc_timeout)
                self.receipt["cleanup_inspect"] = ended
                self.rpc("rm", self.cid)
                remaining = self.rpc("ps", "--all", "--quiet", "--no-trunc", "--filter", "id=" + self.cid)
                if remaining.strip():
                    raise CustodyError("owned container still present after removal")
                self.receipt["removed_container_id"] = self.cid
            except (OSError, ValueError, KeyError, CustodyError, subprocess.SubprocessError) as exc:
                errors.append(str(exc))
        elif self.receipt["status"] != "not_started":
            errors.append("container creation identity unconfirmed")
        self.receipt["cleanup_complete"] = not errors and not self.indeterminate
        self.receipt["cleanup_errors"] = errors
        self.receipt["rpc_indeterminate"] = self.indeterminate
        self.receipt["ended"] = time.time()
        write_json(self.directory / "custody.json", self.receipt)
        return self.receipt

    def run(self, image, argv, *, timeout=3600, binding=None, before_start=None, on_running=None, **kwargs):
        code = 1
        self.on_running = on_running
        self.receipt["binding"] = json.loads(canonical(binding or {}))
        self.receipt["binding_hash"] = digest(self.receipt["binding"])
        try:
            self.create(image, argv, **kwargs)
            if before_start is not None:
                before_start(self)
            code = self.execute(timeout=timeout)
        except InterruptedError as exc:
            self.receipt.update(status="cancelled", error=str(exc))
            code = 130
        except (OSError, ValueError, KeyError, CustodyError, subprocess.SubprocessError) as exc:
            self.receipt.update(status="failed", error=str(exc))
        finally:
            receipt = self.finalize()
        return code if receipt["cleanup_complete"] else 1
