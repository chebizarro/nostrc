"""Build-local realization and exact execution for the selection contract.

Public functions take/return JSON-compatible objects; they do not change policy,
materialize trees, provision platforms, or silently recover dependency drift.
RealizationError means a failed validation (exit 1); malformed calls use ValueError
(exit 2). Every unresolved registration is retained as an explicit blocking gap.
"""
from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
import platform
import re
import shutil
import signal
import subprocess
import tempfile
import threading
import time
import uuid
import xml.etree.ElementTree as ET


class RealizationError(RuntimeError):
    pass


def _hash(value):
    return hashlib.sha256(json.dumps(value, sort_keys=True, separators=(",", ":"),
                                     ensure_ascii=True, allow_nan=False).encode()).hexdigest()


def _read(path):
    try:
        return json.loads(Path(path).read_text())
    except (OSError, ValueError) as exc:
        raise RealizationError(f"invalid/missing JSON {path}: {exc}") from exc


def _atomic(path, value):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, name = tempfile.mkstemp(dir=path.parent, prefix=path.name + ".")
    try:
        with os.fdopen(fd, "w") as stream:
            json.dump(value, stream, sort_keys=True, indent=2, allow_nan=False)
            stream.write("\n")
        os.replace(name, path)
    finally:
        if os.path.exists(name):
            os.unlink(name)


def _execute(argv, cwd=None, env=None, log_path=None, timeout=None):
    """Capture exact bytes; cleanup/evidence failures cannot mask the first error."""
    previous_handler, process, output, original = None, None, b"", None
    chunks, read_errors = [], []
    drained = threading.Event()
    capture_ready, pending_signal = False, None
    status = {"argv": [str(x) for x in argv], "cwd": str(Path(cwd or Path.cwd()).resolve()),
              "environment_hash": _hash(env if env is not None else dict(os.environ)), "started": time.time(),
              "output_encoding": "raw bytes", "text_decoding": "utf-8/surrogateescape"}
    def cancel(signum, frame):
        nonlocal pending_signal
        # The child can signal immediately after exec, before Popen ownership
        # and the pipe reader have been installed. Defer only that startup edge.
        if not capture_ready:
            pending_signal = signum
            return
        raise KeyboardInterrupt(f"command cancelled by signal {signum}")
    def capture():
        # Python signal handlers run on the main thread. Keeping raw reads here
        # prevents an exception between kernel pipe consumption and byte retention
        # from discarding output, with or without a communicate timeout.
        try:
            while data := os.read(process.stdout.fileno(), 65536):
                chunks.append(data)
        except Exception as error:
            read_errors.append(error)
        finally:
            try:
                process.stdout.close()
            except OSError as error:
                read_errors.append(error)
            drained.set()
    def drain(timeout):
        if not drained.wait(timeout):
            raise subprocess.TimeoutExpired(argv, timeout, output=b"".join(chunks))
        if read_errors:
            raise read_errors[0]
    def finish(timeout):
        deadline = None if timeout is None else time.monotonic() + timeout
        drain(timeout)
        process.wait(timeout=None if deadline is None else max(0, deadline - time.monotonic()))
    def cleanup_error(stage, error):
        status.setdefault("cleanup_errors", []).append({"stage": stage, "error": type(error).__name__ + ": " + str(error)})
    def signal_owned(force=False):
        # A known-reaped child no longer owns its PID. Do not poll/reap before
        # group cleanup: an exited leader can still have children holding the
        # output pipe. Group errors (including zombie-group EPERM) stay evidence.
        if process.returncode is not None:
            return
        sig = signal.SIGKILL if force else signal.SIGTERM
        try:
            if os.name != "nt":
                os.killpg(process.pid, sig)
            else:
                process.kill() if force else process.terminate()
        except ProcessLookupError:
            pass
        except OSError as error:
            cleanup_error("signal_group", error)
            # The unreaped direct child still belongs to this Popen. A group
            # failure must remain visible even if direct-child cleanup succeeds.
            try:
                process.kill() if force else process.terminate()
            except OSError as fallback:
                cleanup_error("signal_child", fallback)
    try:
        try:
            previous_handler = signal.signal(signal.SIGTERM, cancel)
        except ValueError:
            pass
        process = subprocess.Popen([str(x) for x in argv], cwd=cwd, env=env,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, bufsize=0,
            start_new_session=os.name != "nt")
        status["child_pid"] = process.pid
        reader = threading.Thread(target=capture, name="selection-command-output", daemon=True)
        reader.start()
        capture_ready = True
        if pending_signal is not None:
            cancel(pending_signal, None)
        finish(timeout)
        output = b"".join(chunks)
        status.update(outcome="passed" if process.returncode == 0 else "failed", exit_code=process.returncode)
        # Text consumers keep their existing interface without discarding any
        # byte. Logs below remain raw; no implicit locale/strict decoding occurs.
        return subprocess.CompletedProcess(argv, process.returncode, output.decode("utf-8", errors="surrogateescape"))
    except BaseException as exc:
        original = exc
        status.update(outcome="cancelled" if isinstance(exc, KeyboardInterrupt) else "failed",
                      error=type(exc).__name__ + ": " + str(exc))
        exc.execution_status = status
        if isinstance(exc, UnicodeDecodeError):
            output = bytes(exc.object)
        elif isinstance(getattr(exc, "output", None), bytes):
            output = exc.output
        if process is not None:
            try:
                signal.signal(signal.SIGTERM, signal.SIG_IGN)
            except ValueError:
                pass
            try:
                signal_owned()
                try:
                    finish(5)
                except subprocess.TimeoutExpired:
                    signal_owned(force=True)
                    finish(5)
                output = b"".join(chunks)
            except Exception as error:
                cleanup_error("drain_or_reap", error)
                if isinstance(getattr(error, "output", None), bytes):
                    output = error.output
                # Do not block forever on a descendant holding the pipe. The
                # attempt is failed and incomplete cleanup is explicit evidence.
                # Unbuffered FileIO.close cannot wait on a reader buffer lock.
                if process.stdout is not None:
                    try:
                        process.stdout.close()
                    except Exception as close_error:
                        cleanup_error("close_output", close_error)
                try:
                    signal_owned(force=True)
                    process.wait(timeout=5)
                except Exception as final_error:
                    cleanup_error("final_reap", final_error)
            try:
                reaped = process.poll() is not None
            except Exception as poll_error:
                cleanup_error("poll", poll_error)
                reaped = False
            status["cleanup_complete"] = reaped and not status.get("cleanup_errors")
        else:
            output = f"cannot execute {argv[0]}: {exc}\n".encode("utf-8", errors="surrogateescape")
        status["exit_code"] = process.returncode if process is not None else None
        if isinstance(exc, OSError):
            error = RealizationError(f"cannot execute {argv[0]}: {exc}")
            error.execution_status = status
            raise error from exc
        raise
    finally:
        if chunks:
            output = b"".join(chunks)
        if previous_handler is not None:
            signal.signal(signal.SIGTERM, previous_handler)
        if log_path:
            status.update(ended=time.time(), output_sha256=hashlib.sha256(output).hexdigest())
            evidence_error = None
            try:
                Path(log_path).write_bytes(output)
            except OSError as error:
                evidence_error = error
                status.setdefault("evidence_errors", []).append("log: " + str(error))
            if evidence_error is not None and original is None:
                status.update(outcome="failed", error="cannot retain command output: " + str(evidence_error))
            try:
                _atomic(str(log_path) + ".process.json", status)
            except OSError as error:
                evidence_error = error
                status.setdefault("evidence_errors", []).append("status: " + str(error))
            if evidence_error is not None and original is None:
                error = RealizationError("cannot retain command evidence: " + str(evidence_error))
                error.execution_status = status
                raise error from evidence_error


def _command(argv, cwd=None, env=None):
    result = _execute(argv, cwd=cwd, env=env)
    if result.returncode:
        raise RealizationError(f"command failed ({result.returncode}): {argv!r}\n{result.stdout}")
    return result.stdout


def _registration_hash(tests):
    # CTest omits command entirely until a freshly configured executable exists.
    # Commands already enumerable are bound separately; generated CTestTestfiles
    # are content-bound in load_realization, including commands not yet runnable.
    return _hash([{k: v for k, v in test.items() if k != "command"} for test in tests])


def _properties(test):
    return {p["name"]: p["value"] for p in test.get("properties", [])}


def _list(value):
    if value is None or value == "":
        return []
    return list(value) if isinstance(value, list) else str(value).split(";")


def _path(root, value):
    p = Path(value)
    return str(p if p.is_absolute() else Path(root) / p)


def _relative(path, root):
    try:
        return Path(os.path.abspath(path)).relative_to(Path(os.path.abspath(root))).as_posix()
    except ValueError:
        return None


def _identity(source_identity):
    if not isinstance(source_identity, dict) or not source_identity:
        raise ValueError("explicit execution source identity is required (including dirty digest)")
    # The materialization owner defines the identity shape. Preserve it byte for
    # byte and fingerprint the complete object, never just the committed SHA.
    return json.loads(json.dumps(source_identity, sort_keys=True))


def _git(root, *args):
    try:
        return subprocess.check_output(["git", "-c", "maintenance.autoDetach=false", "-c", "gc.autoDetach=false",
                                        "-C", str(root), *args], stderr=subprocess.PIPE)
    except (OSError, subprocess.CalledProcessError) as exc:
        raise RealizationError(f"cannot validate source checkout: {exc}") from exc


def _validate_source_identity(root, identity, build_dir):
    """Revalidate actual source, not the identity retained in generated JSON.

    Dirty materializations use test-gate's indexed mode/path/content digest. This
    reads the index and files only; it never stages or resets a caller's checkout.
    """
    root, build = Path(root).resolve(), Path(build_dir).resolve()
    if root.is_relative_to(build):
        raise RealizationError("build directory cannot contain the admitted source checkout")
    top = Path(os.fsdecode(_git(root, "rev-parse", "--show-toplevel")).strip()).resolve()
    if root != top:
        raise RealizationError("source checkout root is not the canonical Git root")
    head = _git(root, "rev-parse", "HEAD").decode().strip()
    if identity.get("kind") not in {"committed", "dirty"} or identity.get("sha") != head:
        raise RealizationError("source checkout HEAD does not match execution identity")
    if identity["kind"] == "committed" and _git(root, "status", "--porcelain", "--untracked-files=no"):
        raise RealizationError("committed source checkout has tracked changes")
    for raw in _git(root, "ls-files", "--others", "--exclude-standard", "-z").split(b"\0"):
        if raw and not (root / os.fsdecode(raw)).is_relative_to(build):
            raise RealizationError(f"source checkout has untracked input: {os.fsdecode(raw)}")
    digest = hashlib.sha256()
    committed = identity["kind"] == "committed"
    # Read committed membership from HEAD, not an index whose assume-unchanged
    # bits can hide working-file edits from git status.
    entries = _git(root, "ls-tree", "-rz", "HEAD") if committed else _git(root, "ls-files", "--stage", "-z")
    object_format = _git(root, "rev-parse", "--show-object-format").decode().strip()
    for entry in entries.split(b"\0"):
        if not entry:
            continue
        meta, raw_path = entry.split(b"\t", 1)
        if committed:
            mode, _, oid = meta.split()
            stage = b"0"
        else:
            mode, oid, stage = meta.split()
        if stage != b"0":
            raise RealizationError("source checkout has an unresolved index")
        path = root / os.fsdecode(raw_path)
        try:
            if mode == b"160000":
                content = oid
                if (path / ".git").exists():
                    if (_git(path, "rev-parse", "HEAD").strip() != oid
                            or _git(path, "status", "--porcelain", "--untracked-files=all")):
                        raise RealizationError(f"source checkout submodule changed: {path}")
                elif path.is_dir() and any(path.iterdir()):
                    raise RealizationError(f"source checkout has uninitialized submodule content: {path}")
            elif mode == b"120000":
                content = os.fsencode(os.readlink(path))
                if not path.resolve().is_relative_to(root):
                    raise RealizationError(f"source snapshot symlink escapes checkout: {path}")
            elif mode in (b"100644", b"100755"):
                if path.is_symlink() or bool(path.stat().st_mode & 0o111) != (mode == b"100755"):
                    raise RealizationError(f"source snapshot file mode changed: {path}")
                content = path.read_bytes()
            else:
                raise RealizationError(f"unsupported source snapshot mode: {mode!r}")
        except OSError as exc:
            raise RealizationError(f"source snapshot input missing/invalid: {path}") from exc
        if committed and mode != b"160000":
            blob = b"blob " + str(len(content)).encode() + b"\0" + content
            if hashlib.new(object_format, blob).hexdigest().encode() != oid:
                raise RealizationError(f"committed source input changed: {path}")
        digest.update(mode + b"\0" + raw_path + b"\0" + hashlib.sha256(content).digest())
    if identity["kind"] == "dirty" and identity.get("snapshot_digest") != digest.hexdigest():
        raise RealizationError("actual source snapshot digest changed or mismatches execution identity")


def _cmake_literal_arguments(text, position):
    """Read only literal arguments emitted in generated CTest add_test calls.

    This is not a source-CMake evaluator. Variable expansion, lists in unquoted
    arguments and unsupported escapes fail closed; no shell is involved.
    """
    arguments = []
    while position < len(text):
        while position < len(text) and text[position].isspace():
            position += 1
        if position >= len(text):
            break
        if text[position] == ")":
            return arguments
        bracket = re.match(r"\[(=*)\[", text[position:])
        if bracket:
            position += bracket.end()
            end = text.find("]" + bracket[1] + "]", position)
            if end < 0:
                raise RealizationError("unterminated generated CTest bracket argument")
            arguments.append(text[position:end])
            position = end + len(bracket[1]) + 2
        elif text[position] == '"':
            position += 1
            value = ""
            while position < len(text) and text[position] != '"':
                char = text[position]
                position += 1
                if char == "$":
                    raise RealizationError("nonliteral generated CTest command requires an explicit descriptor")
                if char == "\\":
                    if position >= len(text):
                        break
                    char = text[position]
                    position += 1
                    if char in "nrt":
                        char = {"n": "\n", "r": "\r", "t": "\t"}[char]
                    elif char == "\n":
                        continue
                    elif char not in '\\";$[]()# ':
                        raise RealizationError("unsupported escape in generated CTest command")
                value += char
            if position >= len(text):
                raise RealizationError("unterminated generated CTest quoted argument")
            arguments.append(value)
            position += 1
        else:
            end = position
            while end < len(text) and not text[end].isspace() and text[end] != ")":
                end += 1
            value = text[position:end]
            if not value or any(char in value for char in "$;\\(\"#"):
                raise RealizationError("nonliteral generated CTest argument")
            arguments.append(value)
            position = end
    raise RealizationError("unterminated generated CTest add_test")


def _generated_ctest_commands(build):
    commands = {}
    for path in sorted(Path(build).rglob("CTestTestfile.cmake")):
        text = path.read_text()
        for match in re.finditer(r"(?m)^add_test\(", text):
            args = _cmake_literal_arguments(text, match.end())
            if len(args) < 2:
                raise RealizationError(f"invalid generated CTest registration in {path}")
            name, command = args[0], args[1:]
            if name in commands and commands[name] != command:
                raise RealizationError(f"ambiguous generated CTest command: {name}")
            commands[name] = command
    return commands


PHASES = ("configure", "build", "test")


def _environment_map(value, label):
    if not isinstance(value, dict) or any(
            not isinstance(k, str) or not k or "=" in k or "\x00" in k
            or not isinstance(v, str) or "\x00" in v for k, v in value.items()):
        raise RealizationError(f"{label} must be a string environment map")
    for content in value.values():
        _expand_source(content, "/source-root")
    return value


def _expand_source(value, source_root):
    if not isinstance(value, str) or "\x00" in value:
        raise RealizationError("invalid profile string")
    # Validate the template BEFORE substitution: a literal brace in a checkout
    # path is not an authored template, and must not be interpreted a second time.
    remainder = value.replace("{source_root}", "")
    if "{" in remainder or "}" in remainder:
        raise RealizationError("unsupported profile substitution (only {source_root})")
    return value.replace("{source_root}", str(source_root))


def validate_phase_profile(profile):
    """Validate authored metadata without host checks or environment flattening."""
    common = _environment_map(profile.get("environment", {}), "environment")
    phases = profile.get("phase_environments", {p: {} for p in PHASES})
    if not isinstance(phases, dict) or set(phases) != set(PHASES):
        raise RealizationError("phase_environments requires exactly configure/build/test")
    for phase in PHASES:
        _environment_map(phases[phase], f"phase_environments.{phase}")
    args = profile.get("configure_args", [])
    if not isinstance(args, list) or any(not isinstance(x, str) or not x or "\x00" in x for x in args):
        raise RealizationError("configure_args must be an argument array")
    for value in args:
        _expand_source(value, "/source-root")
    compat = profile.get("sanitizer_compatibility")
    if "sanitizer_compatibility" not in profile:
        return
    fields = {"GH_SAN_CONFIGURE", "GH_SAN_CONFIGURE_ENV", "GH_SAN_BUILD_ENV", "GH_SAN_ENV",
              "GH_SAN_TARGETS", "GH_SAN_TESTS", "GH_SAN_TEST_JOBS", "GH_SAN_SKIP_PATTERN"}
    if not isinstance(compat, dict) or set(compat) != fields or "phase_environments" not in profile:
        raise RealizationError("invalid sanitizer_compatibility contract fields/phases")
    if compat["GH_SAN_CONFIGURE"] != args:
        raise RealizationError("sanitizer_compatibility configure disagreement")
    for phase, field in zip(PHASES, ("GH_SAN_CONFIGURE_ENV", "GH_SAN_BUILD_ENV", "GH_SAN_ENV")):
        values, mirror = compat[field], {}
        if not isinstance(values, list):
            raise RealizationError(f"invalid sanitizer_compatibility {field}")
        for value in values:
            if not isinstance(value, str) or "=" not in value:
                raise RealizationError(f"invalid sanitizer_compatibility {field}")
            key, content = value.split("=", 1)
            if key in mirror:
                raise RealizationError(f"duplicate sanitizer_compatibility environment: {key}")
            mirror[key] = content
        _environment_map(mirror, field)
        if mirror != dict(common, **phases[phase]):
            raise RealizationError(f"sanitizer_compatibility {phase} environment disagreement")
    for field in ("GH_SAN_TARGETS", "GH_SAN_TESTS"):
        values = compat[field]
        if (not isinstance(values, list) or not values
                or any(not isinstance(x, str) or not x or "\x00" in x for x in values)
                or len(values) != len(set(values))):
            raise RealizationError(f"invalid sanitizer_compatibility {field}")
    jobs = profile.get("parallelism")
    if type(jobs) is not int or jobs < 1 or compat["GH_SAN_TEST_JOBS"] != str(jobs):
        raise RealizationError("sanitizer_compatibility parallelism disagreement")
    if compat["GH_SAN_SKIP_PATTERN"] != r"\*\*\*Skipped|Not Run":
        raise RealizationError("unsupported sanitizer_compatibility skip policy")
    if dict(common, **phases["test"]).get("GROUNDHOG_SANITIZER_TESTS") != " ".join(compat["GH_SAN_TESTS"]):
        raise RealizationError("sanitizer_compatibility test environment/membership disagreement")


def phase_environment(profile, source_root, phase, base_environment=None):
    """Ambient + common + ONE phase overlay; source_root is the checkout root.

    Callers must not export test overlays into the parent process. Captured full
    environment hashes bind ambient inputs too, without persisting their values.
    """
    if phase not in PHASES:
        raise RealizationError(f"unknown environment phase: {phase}")
    validate_phase_profile(profile)
    overlay = dict(profile.get("environment", {}), **profile.get("phase_environments", {}).get(phase, {}))
    return dict(os.environ if base_environment is None else base_environment,
                **{k: _expand_source(v, source_root) for k, v in overlay.items()})


def _phase_context(profile, root, source, configuration):
    environments = {p: phase_environment(profile, root, p) for p in PHASES}
    # Template inputs are paths, optionally followed by colon/semicolon options
    # (e.g. LSAN suppressions). Reject non-path suffix syntax rather than guessing.
    inputs = set()
    values = list(profile.get("environment", {}).values()) + profile.get("configure_args", [])
    values += [v for m in profile.get("phase_environments", {}).values() for v in m.values()]
    for value in values:
        for suffix in re.findall(r"\{source_root\}([^:;{}]*)", value):
            path = Path(str(root) + suffix).resolve()
            if not path.is_relative_to(root) or not path.exists():
                raise RealizationError(f"profile template input escapes/misses source root: {path}")
            inputs.add(str(path))
    return {"source_root": str(root), "source_dir": str(source), "configuration": configuration,
            "environment_overlays": {p: {k: _expand_source(v, root) for k, v in
                dict(profile.get("environment", {}), **profile.get("phase_environments", {}).get(p, {})).items()} for p in PHASES},
            "environment_hashes": {p: _hash(env) for p, env in environments.items()},
            "template_inputs": sorted(inputs)}


def _profile_fingerprint(realization):
    keys = ("profile_id", "profile", "source_identity", "inventory_hash", "cmake", "toolchains",
            "compiled_languages", "target_settings", "environment_hash", "tool_versions", "phase_context")
    return _hash({k: realization.get(k) for k in keys})


def _validate_phase_context(realization):
    root = Path(realization["repo_root"]).resolve()
    source = (root / _expand_source(realization["profile"].get("source_dir", "."), root)).resolve()
    if not source.is_relative_to(root) or not source.is_dir() or str(source) != realization["source_dir"]:
        raise RealizationError("profile source directory changed/escapes checkout")
    current = _phase_context(realization["profile"], root, source, realization["configuration"])
    if (current != realization.get("phase_context")
            or _hash(current) != realization.get("phase_context_hash")
            or current["environment_hashes"]["configure"] != realization.get("environment_hash")):
        raise RealizationError("effective phase environment changed or phase/source/configuration identity mismatch; configure again")
    if _profile_fingerprint(realization) != realization["profile_fingerprint"]:
        raise RealizationError("cached profile fingerprint mismatch; configure again")
    return current


def _validate_resolved_phase(resolved, realization):
    for key in ("profile_hash", "configuration", "phase_context", "phase_context_hash"):
        expected = _hash(realization["profile"]) if key == "profile_hash" else realization[key]
        if resolved.get(key) != expected:
            raise RealizationError(f"resolved phase identity mismatch: {key}")
    compatibility_targets = sorted(realization["profile"].get("sanitizer_compatibility", {}).get("GH_SAN_TARGETS", []))
    if (resolved.get("compatibility_build_targets", []) != compatibility_targets
            or not set(compatibility_targets).issubset(resolved["build_targets"])):
        raise RealizationError("resolved sanitizer_compatibility build targets changed")


def _validate_native_profile(profile, toolchains=None, configuration=None, compiled_languages=None):
    validate_phase_profile(profile)
    system = {"Darwin": "macos", "Linux": "linux", "Windows": "windows"}.get(platform.system(), platform.system().lower())
    requested = profile.get("platform")
    if requested and {"darwin": "macos"}.get(requested, requested) != system:
        raise RealizationError(f"profile platform {requested} does not match actual native host {system}")
    aliases = {"aarch64": "arm64", "amd64": "x86_64", "AMD64": "x86_64"}
    architecture = aliases.get(platform.machine(), platform.machine())
    expected = profile.get("architecture")
    if expected and expected != "native" and aliases.get(expected, expected) != architecture:
        raise RealizationError(f"profile architecture {expected} does not match actual native host {architecture}")
    if configuration is not None and profile.get("build_type") and profile["build_type"] != configuration:
        raise RealizationError(f"profile build type {profile['build_type']} differs from realized {configuration!r}")
    if toolchains is not None and profile.get("compiler"):
        wanted = {"gcc": {"GNU"}, "clang": {"Clang", "AppleClang"}, "msvc": {"MSVC"}}.get(profile["compiler"], {profile["compiler"]})
        native = {"C", "CXX", "OBJC", "OBJCXX"}
        entries = toolchains.get("toolchains", [])
        if not isinstance(entries, list) or any(not isinstance(t, dict) for t in entries):
            raise RealizationError("invalid native toolchain entries")
        def compiler_id(entry):
            value = entry.get("compiler")
            return value.get("id") if isinstance(value, dict) else None
        if compiled_languages is None:
            # Legacy cache/direct caller: absent identities cannot establish that
            # a language was unused. Fresh codemodel evidence is required.
            relevant = [t for t in entries if t.get("language") in native]
            if any(not isinstance(compiler_id(t), str) or not compiler_id(t) for t in relevant):
                raise RealizationError("compiled-language evidence required for incomplete toolchain identities; configure again")
            required = {t["language"] for t in relevant}
        else:
            if not isinstance(compiled_languages, list) or any(not isinstance(x, str) or not x for x in compiled_languages):
                raise RealizationError("invalid compiled-language evidence")
            required = set(compiled_languages) & native
        if not required:
            raise RealizationError("no compiled native language establishes the requested compiler identity")
        actual = set()
        for language in sorted(required):
            matching = [t for t in entries if t.get("language") == language]
            if len(matching) != 1:
                raise RealizationError(f"missing/ambiguous active toolchain: {language}")
            compiler = compiler_id(matching[0])
            if not isinstance(compiler, str) or not compiler:
                raise RealizationError(f"missing compiler identity for active language: {language}")
            actual.add(compiler)
        if not actual.issubset(wanted):
            raise RealizationError(f"profile compiler {profile['compiler']} differs from realized {sorted(actual)}")


def configure_profile(repo_root, build_dir, profile_id, profile, source_identity,
                      inventory_hash):
    """Query BEFORE configure; consume replies only AFTER successful generate.

    profile: source_dir (repo-relative), configure_args (argv), environment (map),
    configuration (optional multi-config name), capabilities (list). Extra profile
    fields are included in the fingerprint. Builds must be dedicated per profile.
    """
    root, build = Path(repo_root).resolve(), Path(build_dir).resolve()
    identity = _identity(source_identity)
    _validate_source_identity(root, identity, build)
    build.mkdir(parents=True, exist_ok=True)
    record = build / "nostrc-test-realization.json"
    record.unlink(missing_ok=True)  # failed configure must invalidate old success
    _validate_native_profile(profile)
    query = build / ".cmake/api/v1/query/client-nostrc-test-selection"
    query.mkdir(parents=True, exist_ok=True)
    (query / "codemodel-v2").touch()
    (query / "toolchains-v1").touch()
    token = uuid.uuid4().hex
    env = phase_environment(profile, root, "configure")
    args = [_expand_source(x, root) for x in profile.get("configure_args", [])]
    source = (root / _expand_source(profile.get("source_dir", "."), root)).resolve()
    if not source.is_relative_to(root) or not source.is_dir():
        raise RealizationError("profile source directory escapes/misses checkout")
    initial_context = _phase_context(profile, root, source, profile.get("configuration", ""))
    cmd = ["cmake", "-S", str(source), "-B", str(build), *args,
           f"-DNOSTRC_TEST_SELECTION_TOKEN={token}"]
    before = {p.name: p.read_bytes() for p in (build / ".cmake/api/v1/reply").glob("index-*.json")}
    started = time.monotonic()
    output = _command(cmd, env=env)
    (build / "nostrc-selection-configure.log").write_text(output, errors="surrogateescape")
    reply_dir = build / ".cmake/api/v1/reply"
    indexes = sorted(reply_dir.glob("index-*.json"), key=lambda p: p.stat().st_mtime_ns)
    if not indexes or (indexes[-1].name in before and before[indexes[-1].name] == indexes[-1].read_bytes()):
        raise RealizationError("missing/stale File API generation reply")
    index = _read(indexes[-1])
    client = index.get("reply", {}).get("client-nostrc-test-selection", {})
    response = client.get("codemodel-v2", {})
    if "error" in response or "jsonFile" not in response:
        raise RealizationError(f"missing/errored File API codemodel reply: {response}")
    codemodel = _read(reply_dir / response["jsonFile"])
    config_name = profile.get("configuration", "")
    configs = codemodel.get("configurations", [])
    if len(configs) > 1:
        configs = [c for c in configs if c["name"] == config_name]
    if len(configs) != 1:
        raise RealizationError("ambiguous/missing codemodel configuration; specify configuration")
    config_name = configs[0]["name"]
    if profile.get("configuration") and profile["configuration"] != config_name:
        raise RealizationError("profile configuration differs from realized configuration")
    context = dict(initial_context, configuration=config_name)
    descriptor_path = build / f"nostrc-test-descriptors-{config_name}.json"
    descriptors = _read(descriptor_path)
    if descriptors.get("schema_version") != 1 or descriptors.get("generation_token") != token:
        raise RealizationError("stale/missing descriptor export")
    ctest_cmd = ["ctest", "--test-dir", str(build), "--show-only=json-v1"]
    if config_name:
        ctest_cmd += ["-C", config_name]
    ctest = json.loads(_command(ctest_cmd, env=phase_environment(profile, root, "test")))
    # CTest suppresses command argv when a fresh executable is absent. Recover
    # the literal generated registration, not an executable guessed from its name.
    literal_commands = _generated_ctest_commands(build) if index.get("cmake", {}).get("generator", {}).get("name") == "Ninja" else {}
    for test in ctest.get("tests", []):
        if "command" not in test and test["name"] in literal_commands:
            test["command"] = literal_commands[test["name"]]
    targets = {}
    target_settings = {}
    compiled_languages = set()
    for ref in configs[0].get("targets", []):
        target = _read(reply_dir / ref["jsonFile"])
        target_settings[target["id"]] = _hash(target)
        for group in target.get("compileGroups", []):
            language = group.get("language")
            if not isinstance(language, str) or not language:
                raise RealizationError(f"missing compile-group language for {target['name']}")
            compiled_languages.add(language)
        targets[target["id"]] = {
            "id": target["id"], "name": target["name"], "type": target["type"],
            "artifacts": [_path(build, a["path"]) for a in target.get("artifacts", [])],
            "sources": [{"path": _path(build if s.get("isGenerated") else source, s["path"]),
                         "generated": s.get("isGenerated", False),
                         "compiled": "compileGroupIndex" in s} for s in target.get("sources", [])],
            "dependencies": [d["id"] for d in target.get("dependencies", [])],
            "compile_fragments": [f["fragment"] for group in target.get("compileGroups", [])
                                  for f in group.get("compileCommandFragments", [])],
        }
    toolchains = client.get("toolchains-v1", {})
    tools = _read(reply_dir / toolchains["jsonFile"]) if "jsonFile" in toolchains else {}
    compiled_languages = sorted(compiled_languages)
    _validate_native_profile(profile, tools, config_name, compiled_languages)
    runtime_manifest = {}
    for desc in descriptors["tests"]:
        files = []
        for value in desc.get("inputs", []):
            path = Path(value)
            if path.is_dir() and _relative(path.resolve(), root) is not None:
                files.extend(str(p) for p in path.rglob("*") if p.is_file() or p.is_symlink())
            else:
                files.append(value)
        runtime_manifest[desc["name"]] = sorted(set(files))
    realization = {
        "schema_version": 1, "repo_root": str(root), "source_dir": str(source),
        "build_dir": str(build), "profile_id": profile_id, "profile": profile,
        "source_identity": identity, "inventory_hash": inventory_hash,
        "configuration": config_name, "targets": targets,
        "tests": ctest.get("tests", []), "descriptors": descriptors["tests"],
        "runtime_file_manifest": runtime_manifest,
        "toolchains": tools, "compiled_languages": compiled_languages, "cmake": index.get("cmake", {}),
        "target_settings": target_settings, "environment_hash": _hash(env),
        "phase_context": context, "phase_context_hash": _hash(context),
        "tool_versions": {"ctest": _command(["ctest", "--version"], env=phase_environment(profile, root, "test")).strip(),
                          "ninja": _command(["ninja", "--version"], env=phase_environment(profile, root, "build")).strip()
                          if index.get("cmake", {}).get("generator", {}).get("name") == "Ninja" else None},
        "commands": [cmd, ctest_cmd], "generation_token": token,
        "configure_seconds": time.monotonic() - started,
    }
    realization["profile_fingerprint"] = _profile_fingerprint(realization)
    compat = profile.get("sanitizer_compatibility")
    if compat:
        if not set(compat["GH_SAN_TARGETS"]).issubset({t["name"] for t in targets.values()}):
            raise RealizationError("sanitizer_compatibility target missing from codemodel")
        if not set(compat["GH_SAN_TESTS"]).issubset({t["name"] for t in realization["tests"]}):
            raise RealizationError("sanitizer_compatibility test missing from CTest")
    # Bind all generated metadata. A later native configure invalidates this file.
    bound = [descriptor_path, indexes[-1], reply_dir / response["jsonFile"]]
    bound += [reply_dir / t["jsonFile"] for t in configs[0].get("targets", [])]
    bound += list(build.rglob("CTestTestfile.cmake"))
    realization["generation_files"] = {str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in bound}
    _validate_source_identity(root, identity, build)
    _validate_phase_context(realization)
    _atomic(record, realization)
    return realization


def load_realization(build_dir, expected_identity=None):
    realization = _read(Path(build_dir) / "nostrc-test-realization.json")
    if realization.get("schema_version") != 1:
        raise RealizationError("unsupported realization schema")
    if expected_identity is not None and realization["source_identity"] != _identity(expected_identity):
        raise RealizationError("execution source identity changed (including dirty snapshot digest)")
    _validate_source_identity(realization["repo_root"], realization["source_identity"], build_dir)
    for name, digest in realization.get("generation_files", {}).items():
        p = Path(name)
        if not p.is_file() or hashlib.sha256(p.read_bytes()).hexdigest() != digest:
            raise RealizationError(f"stale realization metadata: {name}")
    if not realization.get("generation_files"):
        raise RealizationError("unbound generation metadata")
    if Path(realization["build_dir"]).resolve() != Path(build_dir).resolve():
        raise RealizationError("realization build directory mismatch")
    if realization.get("realization_kind") == "command":
        _validate_native_profile(realization["profile"])
    else:
        _validate_native_profile(realization["profile"], realization["toolchains"], realization["configuration"], realization.get("compiled_languages"))
    _validate_phase_context(realization)
    return realization


def _test_maps(realization):
    tests = {}
    for test in realization["tests"]:
        if test["name"] in tests:
            raise RealizationError(f"duplicate CTest registration: {test['name']}")
        tests[test["name"]] = test
    descriptors = {}
    for desc in realization["descriptors"]:
        name = desc["name"]
        if name not in tests or name in descriptors:
            raise RealizationError(f"missing test or duplicate descriptor: {name}")
        descriptors[name] = desc
    return tests, descriptors


def _membership(check, profile_id):
    value = check.get("ctest_tests", {})
    if isinstance(value, dict):
        return value.get(profile_id, [])
    return value


def _eligible(check, profile_id):
    return profile_id in check.get("profiles", check.get("eligible_profiles", []))


def _metadata(inventory, realization):
    tests, descriptors = _test_maps(realization)
    for override in inventory.get("test_overrides", []):
        if override.get("profile") not in (None, realization["profile_id"]):
            continue
        name = override.get("name", override.get("test"))
        if name not in tests:
            continue
        existing = descriptors.setdefault(name, {"name": name})
        for key in ("owner", "targets", "inputs", "requires", "expected"):
            if key in override:
                if existing.get(key):
                    raise RealizationError(f"duplicate descriptor/override authority: {name}.{key}")
                existing[key] = _list(override[key])
    return tests, descriptors


def _mapped_targets(test, desc, realization):
    by_name = {t["name"]: t["id"] for t in realization["targets"].values()}
    explicit = desc.get("targets", [])
    result = set()
    for name in explicit:
        if name not in by_name and name not in realization["targets"]:
            raise RealizationError(f"unknown descriptor target {name} for {test['name']}")
        result.add(by_name.get(name, name))
    artifacts = {}
    for target in realization["targets"].values():
        if target["type"] == "EXECUTABLE":
            for path in target["artifacts"]:
                artifacts.setdefault(os.path.normpath(path), []).append(target["id"])
    command = test.get("command", [])
    matches = artifacts.get(os.path.normpath(command[0]), []) if command else []
    if not explicit:
        if len(matches) != 1:
            # Script commands and wrappers need explicit descriptors, even when
            # they mention an executable later in argv.
            if not desc:
                raise RealizationError(f"ambiguous/unmapped command: {test['name']}")
            if not desc.get("inputs") and desc.get("expected") != ["fixture"]:
                raise RealizationError(f"script/wrapper lacks explicit inputs/targets: {test['name']}")
        result.update(matches)
    elif matches and not set(matches).issubset(result):
        raise RealizationError(f"command/descriptor target disagreement: {test['name']}")
    return sorted(result)


def _target_closure(ids, realization):
    found, todo = set(), list(ids)
    while todo:
        ident = todo.pop()
        if ident in found:
            continue
        if ident not in realization["targets"]:
            raise RealizationError(f"unresolved target dependency: {ident}")
        found.add(ident)
        todo.extend(realization["targets"][ident]["dependencies"])
    return sorted(found)


def _nonmember_binding(realization, descriptors):
    generation = dict(realization["generation_files"])
    build = Path(realization["build_dir"])
    for path in (build / "CMakeCache.txt", build / "build.ninja", build / "CMakeFiles/rules.ninja"):
        if not path.is_file():
            raise RealizationError("diagnostic nonmember generation input missing: " + str(path))
        generation[str(path)] = hashlib.sha256(path.read_bytes()).hexdigest()
    return {"source_identity": realization["source_identity"],
            "inventory_hash": realization["inventory_hash"], "profile_id": realization["profile_id"],
            "profile_hash": _hash(realization["profile"]),
            "profile_fingerprint": realization["profile_fingerprint"],
            "phase_context_hash": realization["phase_context_hash"], "configuration": realization["configuration"],
            "registration_hash": _registration_hash(realization["tests"]),
            "descriptors_hash": _hash(descriptors), "generation_hash": _hash(generation),
            "toolchain_hash": _hash({k: realization[k] for k in ("toolchains", "tool_versions", "target_settings")})}


def _nonmember_partition(contract, tests, descriptors, authored):
    required = _fixture_closure(authored, tests, descriptors)
    if required != contract["required_closure"]:
        raise RealizationError("diagnostic nonmember required fixture closure changed")
    if (set(contract["registrations"]) & set(required)
            or sorted(set(tests) - set(required)) != contract["registrations"]):
        raise RealizationError("diagnostic nonmember fresh registration partition differs")


def _diagnostic_nonmembers(inventory, realization, tests, descriptors):
    if not inventory.get("diagnostic_nonmembers"):
        return []
    from test_selection import NONMEMBER_AUTHORITY, NONMEMBER_ID, diagnostic_nonmember_contracts
    try:
        contract = diagnostic_nonmember_contracts(inventory).get(realization["profile_id"])
    except ValueError as exc:
        raise RealizationError(str(exc)) from exc
    if contract is None:
        return []
    if descriptors != {d["name"]: d for d in realization["descriptors"]}:
        raise RealizationError("diagnostic nonmember descriptors require exact generated authority")
    if (inventory["profiles"][realization["profile_id"]] != realization["profile"]
            or _hash(inventory) != realization["inventory_hash"]):
        raise RealizationError("diagnostic nonmember inventory/profile identity mismatch")
    # No architecture override: the reviewed arm64 witness is provenance only.
    current = load_realization(realization["build_dir"], realization["source_identity"])
    if current != realization:
        raise RealizationError("diagnostic nonmember realization changed")
    config = ["-C", realization["configuration"]] if realization["configuration"] else []
    fresh = json.loads(_command(["ctest", "--test-dir", realization["build_dir"], *config, "--show-only=json-v1"],
        env=phase_environment(realization["profile"], realization["repo_root"], "test")))["tests"]
    if _registration_hash(fresh) != _registration_hash(realization["tests"]):
        raise RealizationError("diagnostic nonmember fresh CTest export changed")
    if len({t["name"] for t in fresh}) != len(fresh):
        raise RealizationError("duplicate diagnostic CTest registration")
    authored = [name for check in inventory["checks"].values()
                if check.get("kind") == "ctest" and _eligible(check, realization["profile_id"])
                for name in _membership(check, realization["profile_id"])]
    if len(set(authored)) != len(authored):
        raise RealizationError("multiply claimed diagnostic primary registration")
    for name in authored:
        if name not in tests or descriptors.get(name, {}).get("expected") == ["fixture"] or any(
                _properties(tests[name]).get(k) for k in ("FIXTURES_SETUP", "FIXTURES_CLEANUP")):
            raise RealizationError("missing/fixture diagnostic primary registration")
    _nonmember_partition(contract, tests, descriptors, authored)
    ids = _target_closure({ident for name in contract["registrations"]
                          for ident in _mapped_targets(tests[name], descriptors.get(name, {}), realization)}, realization)
    return [dict(contract, id=NONMEMBER_ID, profile=realization["profile_id"], status="not_executed", authority=NONMEMBER_AUTHORITY,
                 target_ids=sorted(ids), audit_build_targets=sorted({realization["targets"][i]["name"] for i in ids}),
                 binding=_nonmember_binding(realization, descriptors))]


def _baseline_exclusions(inventory, realization, tests, descriptors):
    """Account the native baseline gap and exact reviewed diagnostic nonmembers.

    coverage_gaps remains deliberately closed; diagnostic_nonmembers admits only
    its reviewed profile partition, never source/header/runtime audit gaps.
    """
    ident = "libgo_macos_asan_stress_exclusion"
    profile_id = "libgo.macos.asan"
    registration = "GoChannelStressTest"
    records = inventory.get("coverage_gaps", [])
    if not isinstance(records, list):
        raise RealizationError("coverage_gaps must be a list")
    applicable = []
    seen = set()
    for record in records:
        if not isinstance(record, dict):
            raise RealizationError("malformed coverage gap record")
        if record.get("id") != ident:
            if "registrations" in record:
                raise RealizationError("unsupported registration exclusion coverage gap")
            continue
        if ident in seen:
            raise RealizationError("duplicate baseline exclusion authority")
        seen.add(ident)
        if (set(record) != {"id", "profile", "registrations", "reason", "tracking"}
                or record.get("profile") != profile_id
                or record.get("registrations") != [registration]
                or not isinstance(record.get("reason"), str) or not record["reason"].strip()
                or not isinstance(record.get("tracking"), str) or not record["tracking"].strip()):
            raise RealizationError("malformed/mismatched native baseline exclusion")
        profile = inventory.get("profiles", {}).get(profile_id)
        required = {"platform": "macos", "architecture": "arm64", "compiler": "clang",
                    "build_type": "Debug", "instrumentation": "asan_ubsan", "source_dir": "libgo",
                    "setup": ".github/workflows/libgo-ci.yml:build-and-test"}
        options = {}
        if isinstance(profile, dict):
            for arg in profile.get("configure_args", []):
                if isinstance(arg, str) and arg.startswith("-D") and "=" in arg:
                    key, value = arg[2:].split("=", 1)
                    options[key.split(":", 1)[0]] = value.upper()
        if (not isinstance(profile, dict) or any(profile.get(k) != v for k, v in required.items())
                or options.get("GO_ENABLE_ASAN") != "ON" or options.get("GO_ENABLE_UBSAN") != "ON"
                or options.get("GO_ENABLE_TSAN") not in (None, "OFF")):
            raise RealizationError("baseline exclusion profile does not match the established native variant")
        if realization["profile_id"] != profile_id:
            continue
        if realization["profile"] != profile:
            raise RealizationError("baseline exclusion realization/profile mismatch")
        if registration not in tests:
            raise RealizationError("baseline exclusion registration is missing")
        for check in inventory["checks"].values():
            if _eligible(check, profile_id) and registration in _membership(check, profile_id):
                raise RealizationError("baseline-excluded registration is also claimed by a check")
        props = _properties(tests[registration])
        desc = descriptors.get(registration, {})
        if (any(props.get(k) for k in ("DISABLED", "FIXTURES_SETUP", "FIXTURES_CLEANUP", "FIXTURES_REQUIRED", "DEPENDS"))
                or desc.get("owner") != ["libgo"] or desc.get("requires")
                or desc.get("expected") not in (None, [], ["pass"])):
            raise RealizationError("baseline exclusion registration metadata changed")
        target_ids = _mapped_targets(tests[registration], desc, realization)
        targets = [realization["targets"][key] for key in target_ids]
        if (len(targets) != 1 or targets[0]["name"] != "go_channel_stress_test"
                or targets[0]["type"] != "EXECUTABLE"
                or "libgo/tests/go_channel_stress_test.c" not in
                {_relative(s["path"], realization["repo_root"]) for s in targets[0]["sources"]}):
            raise RealizationError("baseline exclusion executable/source mapping changed")
        fragments = " ".join(targets[0].get("compile_fragments", []))
        sanitizers = {value for match in re.finditer(r"(?:^|\s)-fsanitize=([^\s]+)", fragments)
                      for value in match[1].split(",")}
        if not {"address", "undefined"}.issubset(sanitizers) or "thread" in sanitizers or "-fno-sanitize=" in fragments:
            raise RealizationError("baseline exclusion actual sanitizer instrumentation differs")
        applicable.append(dict(record, status="not_executed", authority="existing_workflow_baseline",
                               target_ids=target_ids, audit_build_targets=[targets[0]["name"]]))
    return applicable + _diagnostic_nonmembers(inventory, realization, tests, descriptors)


def _fixture_closure(names, tests, descriptors):
    selected, todo = set(), list(names)
    while todo:
        name = todo.pop()
        if name in selected:
            continue
        if name not in tests:
            raise RealizationError(f"missing CTest registration/dependency: {name}")
        selected.add(name)
        props = _properties(tests[name])
        # CTest JSON synthesizes fixture cleanup DEPENDS on *all* consumers,
        # including unselected ones. Exported raw DEPENDS is ordering authority.
        todo.extend(descriptors.get(name, {}).get("declared_depends", _list(props.get("DEPENDS"))))
        fixtures = _list(props.get("FIXTURES_REQUIRED"))
        # Selecting a setup directly also retains its cleanup.
        fixtures += _list(props.get("FIXTURES_SETUP"))
        for fixture in fixtures:
            setups = [n for n, t in tests.items() if fixture in _list(_properties(t).get("FIXTURES_SETUP"))]
            cleanups = [n for n, t in tests.items() if fixture in _list(_properties(t).get("FIXTURES_CLEANUP"))]
            if not setups:
                raise RealizationError(f"missing setup for fixture {fixture}")
            todo.extend(setups + cleanups)
    return sorted(selected)


def configure_command_profile(repo_root, build_dir, profile_id, profile, source_identity, inventory_hash):
    """Source-only command realization; no compiler/build/CTest success claim."""
    root, build = Path(repo_root).resolve(), Path(build_dir).resolve()
    identity = _identity(source_identity)
    _validate_source_identity(root, identity, build)
    _validate_native_profile(profile)
    source = (root / _expand_source(profile["source_dir"], root)).resolve()
    if not source.is_relative_to(root) or not source.is_dir():
        raise RealizationError("command source directory escapes/misses checkout")
    if profile.get("configure_args") or profile.get("instrumentation", "none") != "none":
        raise RealizationError("source-only commands cannot attest configure/instrumentation")
    build.mkdir(parents=True, exist_ok=True)
    marker = build / "nostrc-command-generation.json"
    _atomic(marker, {"token": uuid.uuid4().hex})
    result = {"schema_version": 1, "realization_kind": "command", "repo_root": str(root),
              "source_dir": str(source), "build_dir": str(build), "profile_id": profile_id,
              "profile": profile, "source_identity": identity, "inventory_hash": inventory_hash,
              "configuration": "", "targets": {}, "tests": [], "descriptors": [],
              "cmake": {}, "toolchains": {}, "commands": [], "configure_seconds": 0,
              "environment_hash": _hash(phase_environment(profile, root, "configure")),
              "generation_files": {str(marker): hashlib.sha256(marker.read_bytes()).hexdigest()}}
    if profile.get("phase_environments") or profile.get("sanitizer_compatibility"):
        raise RealizationError("source-only commands cannot attest configure/build phase overlays")
    result["phase_context"] = _phase_context(profile, root, source, "")
    result["phase_context_hash"] = _hash(result["phase_context"])
    result["profile_fingerprint"] = _profile_fingerprint(result)
    _validate_phase_context(result)
    _atomic(build / "nostrc-test-realization.json", result)
    return result


def _substitute(value, realization):
    if not isinstance(value, str) or not value or "\x00" in value:
        raise RealizationError("invalid command contract string")
    value = value.replace("{source}", realization["repo_root"]).replace("{build}", realization["build_dir"])
    if "{" in value or "}" in value:
        raise RealizationError("unsupported command substitution (only {source}/{build})")
    return value


def _command_specs(inventory, realization):
    """Bind authored non-CTest commands to this checkout, targets and executables.

    {source} is the repository root (also for standalone CMake profiles). No
    implicit cwd, shell interpolation, PATH fallback after resolution, or wildcard.
    """
    result = {}
    targets = {t["name"]: t for t in realization["targets"].values()}
    env_bases = {p: phase_environment(realization["profile"], realization["repo_root"], p) for p in ("build", "test")}
    for ident, check in inventory["checks"].items():
        if check.get("kind") == "ctest" or not _eligible(check, realization["profile_id"]):
            continue
        argv = [_substitute(x, realization) for x in check.get("command", [])]
        if not argv:
            raise RealizationError(f"empty command: {ident}")
        cwd = Path(_substitute(check.get("working_directory"), realization))
        if (not cwd.is_absolute() or not cwd.is_dir()
                or not any(cwd.resolve().is_relative_to(Path(realization[k])) for k in ("repo_root", "build_dir"))):
            raise RealizationError(f"command cwd must be inside admitted source/build checkout: {ident}")
        environment = check.get("environment")
        if not isinstance(environment, dict) or any(not isinstance(k, str) or not k or "=" in k or "\x00" in k for k in environment):
            raise RealizationError(f"invalid command environment: {ident}")
        phase = "build" if check["kind"] == "build_contract" else "test"
        env = dict(env_bases[phase], **{k: _substitute(v, realization) if v else "" for k, v in environment.items()})
        required = check.get("requires", [])
        if not set(required).issubset(realization["profile"].get("capabilities", [])):
            raise RealizationError(f"unavailable command capabilities: {ident}")
        names = list(check.get("build_targets", []))
        if check["kind"] == "build_contract":
            # Build contracts must identify an exact codemodel target set. Keep
            # arbitrary commands in kind=command, never interpret shell flags.
            tail = argv[3:]
            if realization["configuration"] and tail[:2] == ["--config", realization["configuration"]]:
                tail = tail[2:]
            if (argv[:3] != ["cmake", "--build", realization["build_dir"]]
                    or not tail or tail[0] != "--target" or len(tail) < 2
                    or any(x.startswith("-") for x in tail[1:])):
                raise RealizationError(f"build_contract requires exact cmake --build/--target contract: {ident}")
            declared = tail[1:]
            if len(declared) != len(set(declared)) or (names and sorted(names) != sorted(declared)):
                raise RealizationError(f"build_contract target disagreement: {ident}")
            names = declared
        if any(n not in targets for n in names):
            raise RealizationError(f"unknown command build target: {ident}")
        target_ids = _target_closure([targets[n]["id"] for n in names], realization)
        executable = argv[0]
        candidate = Path(executable)
        if candidate.is_absolute():
            if not candidate.resolve().is_relative_to(Path(realization["repo_root"])) and not candidate.resolve().is_relative_to(Path(realization["build_dir"])):
                raise RealizationError(f"absolute command executable outside admitted roots: {ident}")
            executable = str(candidate.resolve())
            if candidate.is_relative_to(Path(realization["build_dir"])) and executable not in {p for t in target_ids for p in realization["targets"][t]["artifacts"]}:
                raise RealizationError(f"command executable has no declared producer: {ident}")
        elif "/" in executable or "\\" in executable:
            raise RealizationError(f"use explicit {{source}}/{{build}} executable path: {ident}")
        else:
            executable = shutil.which(executable, path=env.get("PATH"))
            if not executable:
                raise RealizationError(f"command executable unavailable: {ident}")
        argv[0] = executable
        inputs = [_substitute(x, realization) for x in check.get("runtime_inputs", [])]
        # Literal file arguments (not interpreter imports or shell syntax) are
        # execution inputs too. Never let an argv script evade impact auditing.
        for argument in argv[1:]:
            path = Path(argument) if Path(argument).is_absolute() else cwd / argument
            if (path.is_file() or path.is_symlink()) and path.is_relative_to(Path(realization["repo_root"])):
                inputs.append(str(path))
        if check["kind"] == "command" and not check.get("runtime_inputs"):
            raise RealizationError(f"command requires explicit source runtime_inputs: {ident}")
        if any(not Path(p).is_absolute() for p in inputs):
            raise RealizationError(f"command runtime_inputs require root substitutions: {ident}")
        if Path(executable).is_relative_to(Path(realization["repo_root"])):
            inputs.append(executable)
        evidence = check.get("required_evidence", [])
        if any(not isinstance(p, str) or Path(p).is_absolute() or any(x in ("", ".", "..") for x in p.split("/")) for p in evidence):
            raise RealizationError(f"required_evidence must be normalized build-relative file paths: {ident}")
        result[ident] = {"kind": check["kind"], "phase": phase, "argv": argv, "cwd": str(cwd.resolve()),
                         "environment": {k: env[k] for k in environment}, "environment_hash": _hash(env), "inputs": sorted(set(inputs)),
                         "target_ids": target_ids, "build_targets": sorted(names),
                         "requires": sorted(required), "required_evidence": evidence}
    return result


def _compatibility_target_ids(realization):
    names = realization["profile"].get("sanitizer_compatibility", {}).get("GH_SAN_TARGETS", [])
    targets = {t["name"]: t["id"] for t in realization["targets"].values()}
    if not set(names).issubset(targets):
        raise RealizationError("sanitizer_compatibility target missing from codemodel")
    return {targets[n] for n in names}


def _audit_compatibility_targets(inventory, realization, after_build):
    """Audit profile prerequisites (including the app), not fictitious checks."""
    ids = _target_closure(_compatibility_target_ids(realization), realization)
    if not ids:
        return [], {}
    covered = _impact_prerequisites(inventory, {n for c in inventory["checks"].values()
        if _eligible(c, realization["profile_id"]) for n in c.get("nodes", [])})
    headers = _ninja_headers(realization) if after_build else {}
    gaps, hashes = [], {}
    for key in ids:
        target = realization["targets"][key]
        paths = [(s["path"], "generated" if s["generated"] else "source") for s in target["sources"]]
        if after_build:
            output = _command(["ninja", "-C", realization["build_dir"], "-t", "inputs", target["name"]],
                              env=phase_environment(realization["profile"], realization["repo_root"], "build"))
            paths += [(_path(realization["build_dir"], p), "ninja") for p in output.splitlines()]
            discovered = [p for obj, values in headers.items() if f"CMakeFiles/{target['name']}.dir/" in obj.replace("\\", "/") for p in values]
            paths += [(_path(realization["build_dir"], p), "header") for p in discovered]
            if not discovered and any(s.get("compiled") for s in target["sources"]):
                gaps.append({"kind": "missing_discovered_header_index", "target": target["name"]})
            for artifact in target["artifacts"]:
                if not Path(artifact).is_file():
                    gaps.append({"kind": "missing_profile_build_artifact", "target": target["name"], "input": artifact})
        for value, kind in sorted(set(paths)):
            path = Path(value)
            if kind == "generated" or path.is_relative_to(Path(realization["build_dir"])):
                continue
            relative = _relative(value, realization["repo_root"])
            if not path.is_file():
                gaps.append({"kind": "missing_profile_input", "target": target["name"], "input": value})
                continue
            if relative is None and kind not in ("header", "ninja"):
                gaps.append({"kind": "external_profile_input", "target": target["name"], "input": value})
            elif relative is not None:
                owners, effect = _owners(inventory, relative)
                if not path.resolve().is_relative_to(Path(realization["repo_root"])) or (effect != "global" and (effect == "non_execution" or not owners or owners.isdisjoint(covered))):
                    gaps.append({"kind": "uncovered_profile_build_input", "target": target["name"], "input": relative})
            hashes[value] = hashlib.sha256(path.read_bytes()).hexdigest()
    return gaps, hashes


def _audit_commands(inventory, realization, specs, after_build):
    gaps, hashes = [], {}
    headers = _ninja_headers(realization) if after_build and realization["targets"] else {}
    for ident, spec in specs.items():
        check = inventory["checks"][ident]
        covered = _impact_prerequisites(inventory, check.get("nodes", []))
        paths = [(p, "runtime") for p in spec["inputs"]]
        for key in spec["target_ids"]:
            target = realization["targets"][key]
            paths += [(s["path"], "generated" if s["generated"] else "source") for s in target["sources"]]
            if after_build:
                paths += [(_path(realization["build_dir"], p), "ninja") for p in _command(["ninja", "-C", realization["build_dir"], "-t", "inputs", target["name"]],
                    env=phase_environment(realization["profile"], realization["repo_root"], "build")).splitlines()]
                discovered = [p for obj, values in headers.items() if f"CMakeFiles/{target['name']}.dir/" in obj.replace("\\", "/") for p in values]
                paths += [(_path(realization["build_dir"], p), "header") for p in discovered]
                if not discovered and any(s.get("compiled") for s in target["sources"]):
                    gaps.append({"kind": "missing_discovered_header_index", "check": ident, "target": target["name"]})
        expanded = []
        for value, kind in paths:
            path = Path(value)
            if path.is_dir() and kind == "runtime":
                files = [p for p in path.rglob("*") if p.is_file() or p.is_symlink()]
                if not files:
                    gaps.append({"kind": "empty_runtime_directory", "check": ident, "input": value})
                expanded += [(str(p), kind) for p in files]
            else:
                expanded.append((value, kind))
        for value, kind in sorted(set(expanded)):
            path = Path(value)
            if path.is_relative_to(Path(realization["build_dir"])):
                if kind == "runtime":
                    if (not spec["target_ids"] or not path.resolve().is_relative_to(Path(realization["build_dir"]))
                            or (after_build and not path.is_file())):
                        gaps.append({"kind": "generated_runtime_without_target_or_file", "check": ident, "input": value})
                    elif after_build:
                        hashes[value] = hashlib.sha256(path.read_bytes()).hexdigest()
                continue
            if not path.is_file():
                gaps.append({"kind": "missing_input", "check": ident, "input": value})
                continue
            relative = _relative(value, realization["repo_root"])
            if relative is None:
                if kind not in ("header", "ninja"):
                    gaps.append({"kind": "external_input", "check": ident, "input": value})
            elif not path.resolve().is_relative_to(Path(realization["repo_root"])):
                gaps.append({"kind": "external_symlink_input", "check": ident, "input": value})
            else:
                owners, effect = _owners(inventory, relative)
                if effect != "global" and (effect == "non_execution" or not owners or owners.isdisjoint(covered)):
                    gaps.append({"kind": "missing_impact_edge", "check": ident, "input": relative})
            hashes[value] = hashlib.sha256(path.read_bytes()).hexdigest()
        executable = Path(spec["argv"][0])
        if after_build and (not executable.is_file() or not os.access(executable, os.X_OK)):
            gaps.append({"kind": "missing_command_executable", "check": ident})
        if executable.is_file():
            hashes[str(executable)] = hashlib.sha256(executable.read_bytes()).hexdigest()
    return gaps, hashes


def resolve_checks(inventory, plan, realization, profile_id):
    """Resolve exact check membership plus fixture/order/runtime/build closure.

    plan.selected_checks is a list of stable IDs (or plan.checks). No name regex
    matching, alias expansion, profile projection or policy broadening happens here.
    """
    _validate_phase_context(realization)
    if realization["profile_id"] != profile_id:
        raise RealizationError("profile mismatch")
    if profile_id in inventory.get("profiles", {}) and inventory["profiles"][profile_id] != realization["profile"]:
        raise RealizationError("profile policy changed; configure again")
    compat = realization["profile"].get("sanitizer_compatibility")
    if compat and set(compat["GH_SAN_TESTS"]) != {n for check in inventory["checks"].values()
            if check.get("kind") == "ctest" and _eligible(check, profile_id) for n in _membership(check, profile_id)}:
        raise RealizationError("sanitizer_compatibility policy membership disagreement")
    if plan.get("source_identity", realization["source_identity"]) != realization["source_identity"]:
        raise RealizationError("plan/realization source identity mismatch")
    if plan.get("inventory_hash", realization["inventory_hash"]) != realization["inventory_hash"]:
        raise RealizationError("plan/realization inventory mismatch")
    tests, descriptors = _metadata(inventory, realization)
    exclusions = _baseline_exclusions(inventory, realization, tests, descriptors)
    excluded_names = {n for exclusion in exclusions for n in exclusion["registrations"]}
    claims = {}
    for ident, check in inventory["checks"].items():
        if check.get("kind") == "ctest" and _eligible(check, profile_id):
            for name in _membership(check, profile_id):
                if name in claims:
                    raise RealizationError(f"multiply claimed primary execution: {name}")
                claims[name] = ident
    command_specs = _command_specs(inventory, realization)
    selected_commands = {}
    checks, primary = {}, set()
    for ident in plan.get("selected_checks", plan.get("checks", [])):
        check = inventory["checks"][ident]
        if not _eligible(check, profile_id):
            continue
        if check.get("kind") != "ctest":
            selected_commands[ident] = command_specs[ident]
            checks[ident] = {"tests": [ident], "requirement": check.get("requirement", "blocking")}
            continue
        names = _membership(check, profile_id)
        if not names:
            raise RealizationError(f"empty selected check: {ident}")
        for name in names:
            if name not in tests:
                raise RealizationError(f"missing selected registration: {name}")
            props = _properties(tests[name])
            if props.get("FIXTURES_SETUP") or props.get("FIXTURES_CLEANUP") or descriptors.get(name, {}).get("expected") == ["fixture"]:
                raise RealizationError(f"fixture claimed as primary execution: {name}")
        checks[ident] = {"tests": sorted(names), "requirement": check.get("requirement", "blocking")}
        primary.update(names)
    selected = _fixture_closure(primary, tests, descriptors)
    if excluded_names.intersection(selected):
        raise RealizationError("baseline-excluded registration is required by selected execution closure")
    ids, inputs, capabilities = set(), set(), set()
    for name in selected:
        desc = descriptors.get(name, {})
        ids.update(_mapped_targets(tests[name], desc, realization))
        inputs.update(desc.get("inputs", []))
        capabilities.update(desc.get("requires", []))
    for spec in selected_commands.values():
        # A build contract also executes its authored command after targets are
        # built for the mandatory header audit; never synthesize its outcome.
        inputs.update(spec["inputs"])
        capabilities.update(spec["requires"])
        if spec["kind"] != "build_contract":
            ids.update(spec["target_ids"])
    contract_ids = {key for spec in selected_commands.values() if spec["kind"] == "build_contract" for key in spec["target_ids"]}
    audit_ids = {ident for exclusion in exclusions for ident in exclusion["target_ids"]}
    compatibility_ids = _compatibility_target_ids(realization)
    audit_ids.update(compatibility_ids)
    closure = _target_closure(ids | audit_ids | contract_ids, realization)
    available = set(realization["profile"].get("capabilities", []))
    if not capabilities.issubset(available):
        raise RealizationError(f"unavailable selected capabilities: {sorted(capabilities - available)}")
    result = {"schema_version": 1, "profile_id": profile_id,
        "profile_fingerprint": realization["profile_fingerprint"],
        "source_identity": realization["source_identity"], "inventory_hash": realization["inventory_hash"],
        "plan_hash": _hash(plan), "plan_id": plan.get("plan_id", _hash(plan)),
        "profile_hash": _hash(realization["profile"]), "configuration": realization["configuration"],
        "phase_context": realization["phase_context"], "phase_context_hash": realization["phase_context_hash"],
        "build_dir": realization["build_dir"], "checks": checks,
        "command_executions": selected_commands, "command_specs_hash": _hash(command_specs),
        "expected_executions": sorted(selected + list(selected_commands)),
        "contract_build_targets": sorted({realization["targets"][i]["name"] for i in contract_ids}),
        "tests": selected, "primary_tests": sorted(primary), "support_tests": sorted(set(selected) - primary),
        "target_ids": closure, "build_targets": sorted({realization["targets"][i]["name"] for i in ids | audit_ids | contract_ids}),
        "audit_build_targets": sorted({realization["targets"][i]["name"] for i in audit_ids}),
        "compatibility_build_targets": sorted(realization["profile"].get("sanitizer_compatibility", {}).get("GH_SAN_TARGETS", [])),
        "baseline_exclusions": exclusions, "coverage_gaps": inventory.get("coverage_gaps", []),
        "complete_gate_eligible": not bool(exclusions or inventory.get("coverage_gaps")),
        "runtime_inputs": sorted(inputs), "required_capabilities": sorted(capabilities),
        "expected_outcomes": {n: descriptors.get(n, {}).get("expected", []) for n in selected},
        "registration_hash": _registration_hash(realization["tests"]),
        "registered_commands": {t["name"]: t["command"] for t in realization["tests"] if "command" in t}}
    result["resolved_hash"] = _hash(result)
    return result


def _owners(inventory, relative):
    if relative is None:
        return set(), "external"
    matches = []
    for rule in inventory.get("path_rules", []):
        exact, prefix = rule.get("exact"), rule.get("prefix", "").rstrip("/")
        if exact == relative or (prefix and (relative == prefix or relative.startswith(prefix + "/"))):
            matches.append((len(exact or prefix), rule))
    if not matches:
        return set(), "unclassified"
    longest = max(n for n, _ in matches)
    rules = [r for n, r in matches if n == longest]
    return {n for r in rules for n in r.get("nodes", [])}, rules[0].get("effect", "impact")


def _impact_prerequisites(inventory, owners):
    seen, todo = set(), list(owners)
    while todo:
        node = todo.pop()
        if node in seen:
            continue
        if node not in inventory["nodes"]:
            raise RealizationError(f"unknown impact owner {node}")
        seen.add(node)
        todo.extend(inventory["nodes"][node].get("depends_on", []))
    return seen


def _ninja_headers(realization):
    if realization.get("cmake", {}).get("generator", {}).get("name") != "Ninja":
        raise RealizationError("discovered-header audit requires existing Ninja profile")
    output = _command(["ninja", "-C", realization["build_dir"], "-t", "deps"],
                      env=phase_environment(realization["profile"], realization["repo_root"], "build"))
    headers, current = {}, None
    for line in output.splitlines():
        if line and not line[0].isspace():
            current = line.split(": #deps", 1)[0]
            headers[current] = []
        elif current is not None and line.startswith("    "):
            headers[current].append(line.strip())
    return headers


def check_inputs(inventory, realization, source_identity=None, *, after_build=False, custody=None):
    """Audit EVERY registered execution, including omitted tests; return gaps.

    A failed audit is never a passing selective receipt, even after broad recovery.
    after_build additionally checks Ninja inputs/discovered headers. An absent
    header index is an explicit gap, not evidence that a target uses no headers.
    """
    _validate_phase_context(realization)
    if source_identity is not None and _identity(source_identity) != realization["source_identity"]:
        raise RealizationError("input audit identity mismatch")
    prepared_inputs = {}
    if custody is not None:
        from test_selection_custody import PreparedContext
        if not isinstance(custody, PreparedContext):
            raise RealizationError('prepared audit requires live contained context')
        prepared_inputs = custody.audit_inputs(realization)
    tests, descriptors = _metadata(inventory, realization)
    exclusions = _baseline_exclusions(inventory, realization, tests, descriptors)
    excluded_names = {n for exclusion in exclusions for n in exclusion["registrations"]}
    gaps, fingerprints = [], {}
    def gap(kind, **detail):
        gaps.append({"kind": kind, **detail})
    root = Path(realization["repo_root"])
    for value in realization["phase_context"]["template_inputs"]:
        path = Path(value)
        files = sorted(p for p in path.rglob("*") if (p.is_file() or p.is_symlink()) and ".git" not in p.relative_to(path).parts) if path.is_dir() else [path]
        for file in files:
            relative = _relative(file.resolve(), root)
            owners, effect = _owners(inventory, relative)
            if not file.is_file() or relative is None:
                gap("invalid_profile_input", input=str(file))
                continue
            fingerprints[str(file)] = hashlib.sha256(file.read_bytes()).hexdigest()
            if effect == "non_execution" or (not owners and effect != "global"):
                gap("unowned_profile_input", input=relative)
            elif effect != "global":
                for ident, check in inventory["checks"].items():
                    if _eligible(check, realization["profile_id"]) and owners.isdisjoint(_impact_prerequisites(inventory, check.get("nodes", []))):
                        gap("missing_profile_impact_edge", check=ident, input=relative, owners=sorted(owners))
    headers = _ninja_headers(realization) if after_build and realization.get("realization_kind") != "command" else {}
    claims = {}
    for ident, check in inventory["checks"].items():
        if check.get("kind") != "ctest" or not _eligible(check, realization["profile_id"]):
            continue
        for name in _membership(check, realization["profile_id"]):
            if name not in tests:
                gap("missing_registration", check=ident, test=name)
            if name in claims:
                gap("multiply_claimed_registration", check=ident, test=name)
            claims[name] = ident
    header_count = 0
    audited_targets = set()
    ninja_inputs = {}
    execution_inputs = {}
    for name, test in sorted(tests.items()):
        desc = descriptors.get(name, {})
        owners = set(desc.get("owner", []))
        if not owners:
            gap("unowned_registration", test=name)
            continue
        covered = _impact_prerequisites(inventory, owners)
        check_owners = set()
        for check in inventory["checks"].values():
            if _eligible(check, realization["profile_id"]) and name in _membership(check, realization["profile_id"]):
                check_owners.update(check.get("nodes", check.get("impact_nodes", [])))
        expected = desc.get("expected", [])
        if expected not in ([], ["pass"], ["fixture"], ["mdk09_incompatibility"]):
            gap("unsupported_expected_outcome", test=name, expected=expected)
        if expected == ["mdk09_incompatibility"]:
            check = inventory["checks"].get(claims.get(name), {})
            spec = check.get("expected_outcome", {})
            if name != "groundhog-mdk011-interop-mdk09-probe" or spec != {"case": "mdk09_incompatibility", "exit_code": 77}:
                gap("unsupported_expected_outcome", test=name, expected=expected)
        elif name in claims and inventory["checks"][claims[name]].get("expected_outcome", "pass") != "pass":
            gap("expected_outcome_disagreement", test=name)
        # Logical checks may add test-only input nodes without making a
        # production component depend on its own fixtures/vectors. Audit that
        # actual selection coverage as well as the registration's owning node.
        covered.update(_impact_prerequisites(inventory, check_owners))
        props = _properties(test)
        support = bool(props.get("FIXTURES_SETUP") or props.get("FIXTURES_CLEANUP") or desc.get("expected") == ["fixture"])
        if not support and not check_owners and name not in excluded_names:
            gap("unclaimed_registration", test=name)
        elif not support and check_owners and not owners.issubset(_impact_prerequisites(inventory, check_owners)):
            gap("check_descriptor_owner_disagreement", test=name, owners=sorted(owners))
        try:
            ids = _target_closure(_mapped_targets(test, desc, realization), realization)
        except RealizationError as exc:
            gap("target_mapping", test=name, reason=str(exc))
            continue
        inputs = [(p, "runtime") for p in set(desc.get("inputs", []) + realization.get("runtime_file_manifest", {}).get(name, []))]
        for ident in ids:
            target = realization["targets"][ident]
            inputs += [(s["path"], "generated" if s["generated"] else "source") for s in target["sources"]]
            if after_build:
                if ident not in ninja_inputs:
                    output = _command(["ninja", "-C", realization["build_dir"], "-t", "inputs", target["name"]],
                                      env=phase_environment(realization["profile"], realization["repo_root"], "build"))
                    ninja_inputs[ident] = [(_path(realization["build_dir"], p), "ninja") for p in output.splitlines()]
                inputs += ninja_inputs[ident]
                target_headers = 0
                for obj, dependencies in headers.items():
                    if f"CMakeFiles/{target['name']}.dir/" in obj.replace("\\", "/"):
                        header_count += len(dependencies)
                        target_headers += len(dependencies)
                        inputs += [(_path(realization["build_dir"], p), "header") for p in dependencies]
                if not target_headers and ident not in audited_targets and any(s.get("compiled") for s in target["sources"]):
                    gap("missing_discovered_header_index", target=target["name"])
                audited_targets.add(ident)
        expanded = []
        for value, kind in inputs:
            path = Path(value)
            if kind == "runtime" and _relative(value, realization["repo_root"]) is not None and _relative(path.resolve(), realization["repo_root"]) is None:
                gap("external_symlink_input", test=name, input=value)
                continue
            if path.is_dir() and kind == "runtime":
                files = [p for p in sorted(path.rglob("*")) if p.is_file() or p.is_symlink()]
                if not files:
                    gap("empty_runtime_directory", test=name, input=value)
                expanded += [(str(p), kind) for p in files]
            else:
                expanded.append((value, kind))
        execution_inputs[name] = []
        for value, kind in sorted(set(expanded)):
            path = Path(value)
            relative = _relative(value, realization["repo_root"])
            build_relative = _relative(value, realization["build_dir"])
            if kind == "runtime" and not path.exists():
                if build_relative is not None and ids and not after_build:
                    continue  # declared build targets must produce this before execution
                gap("missing_input", test=name, input=value)
                continue
            if kind == "generated" or (build_relative is not None and kind != "header"):
                # Generated files are audited through producer target sources and
                # declared runtime source inputs. Unresolved provenance stays a gap.
                if kind == "runtime" and not ids and not support:
                    gap("generated_runtime_without_target", test=name, input=value)
                continue
            if relative is None:
                if kind == 'runtime' and value in prepared_inputs:
                    actual = hashlib.sha256(path.read_bytes()).hexdigest()
                    if path.is_symlink() or actual != prepared_inputs[value]['binary_sha256']:
                        raise RealizationError('prepared audit input bytes differ: ' + value)
                    fingerprints[value] = actual
                    continue
                if kind in ("runtime", "source"):
                    gap("external_input", test=name, input=value)
                if path.is_file() and kind in ("header", "ninja") and value not in fingerprints:
                    fingerprints[value] = hashlib.sha256(path.read_bytes()).hexdigest()
                continue  # bind system build inputs, but do not invent repository ownership
            if not path.exists():
                gap("missing_input", test=name, input=relative)
                continue
            if _relative(path.resolve(), realization["repo_root"]) is None:
                gap("external_symlink_input", test=name, input=relative)
                continue
            if value not in fingerprints:
                fingerprints[value] = hashlib.sha256(path.read_bytes()).hexdigest()
            input_owners, effect = _owners(inventory, relative)
            execution_inputs[name].append((relative, kind, input_owners, effect))
            if effect == "global":
                continue
            if effect == "non_execution" or not input_owners:
                gap("unowned_input", test=name, input=relative, input_kind=kind)
            elif input_owners.isdisjoint(covered):
                gap("missing_impact_edge", test=name, input=relative,
                    input_kind=kind, owners=sorted(owners), missing_nodes=sorted(input_owners - covered))
    # Selection must reach EACH consumer when ANY of its execution inputs
    # changes. A support registration's own owners cannot provide this proof:
    # they may be unrelated to the primary check that actually runs it.
    for primary, ident in sorted(claims.items()):
        if primary not in tests:
            continue
        check = inventory["checks"][ident]
        covered = _impact_prerequisites(inventory, check.get("nodes", check.get("impact_nodes", [])))
        try:
            closure = _fixture_closure([primary], tests, descriptors)
        except RealizationError as exc:
            gap("support_closure", check=ident, test=primary, reason=str(exc))
            continue
        for support_name in closure:
            for relative, kind, input_owners, effect in execution_inputs.get(support_name, []):
                if effect == "global" or not input_owners or effect == "non_execution":
                    continue  # unowned/non-execution inputs already block above
                if input_owners.isdisjoint(covered):
                    gap("missing_impact_edge", check=ident, test=primary,
                        support_test=support_name, input=relative, input_kind=kind,
                        owners=sorted(check.get("nodes", check.get("impact_nodes", []))),
                        missing_nodes=sorted(input_owners - covered))
    specs = _command_specs(inventory, realization)
    command_gaps, command_hashes = _audit_commands(inventory, realization, specs, after_build)
    gaps.extend(command_gaps)
    fingerprints.update(command_hashes)
    compatibility_gaps, compatibility_hashes = _audit_compatibility_targets(inventory, realization, after_build)
    gaps.extend(compatibility_gaps)
    fingerprints.update(compatibility_hashes)
    command_targets = {key for spec in specs.values() for key in spec["target_ids"]}
    if after_build and not header_count and not command_targets and any(t["type"] in ("EXECUTABLE", "STATIC_LIBRARY", "SHARED_LIBRARY") for t in realization["targets"].values()):
        gap("missing_discovered_header_index")
    result = {"schema_version": 1, "passed": not gaps, "requires_broad": bool(gaps),
        "selective_enforcement": False if gaps or not after_build or exclusions else True,
        "baseline_exclusions": exclusions, "coverage_gaps": inventory.get("coverage_gaps", []),
        "complete_gate_eligible": not bool(exclusions or inventory.get("coverage_gaps")),
        "source_identity": realization["source_identity"], "inventory_hash": realization["inventory_hash"],
        "profile_fingerprint": realization["profile_fingerprint"], "after_build": after_build,
        "phase_context": realization["phase_context"], "phase_context_hash": realization["phase_context_hash"],
        "gaps": gaps, "input_hashes": fingerprints, "header_entries": header_count,
        "command_specs_hash": _hash(specs),
        "compatibility_build_targets": sorted(realization["profile"].get("sanitizer_compatibility", {}).get("GH_SAN_TARGETS", [])),
        "header_index_hash": _hash(headers) if after_build else None}
    if any(e.get("authority") == "reviewed_diagnostic_nonmembers_v1" for e in exclusions):
        result["nonmember_binding"] = _nonmember_binding(realization, descriptors)
    if prepared_inputs:
        result['prepared_inputs'] = prepared_inputs
        result['complete_gate_eligible'] = False
    result["audit_hash"] = _hash(result)
    return result


def _execution_outcomes(junit, names, expected_outcomes):
    outcomes = {}
    if Path(junit).exists():
        try:
            cases = ET.parse(junit).getroot().iter("testcase")
        except ET.ParseError as exc:
            raise RealizationError(f"malformed JUnit: {exc}") from exc
        for case in cases:
            name = case.get("name")
            if name in outcomes:
                raise RealizationError(f"duplicate JUnit execution: {name}")
            failure = case.find("failure") is not None or case.find("error") is not None
            skipped = case.find("skipped")
            outcomes[name] = ("failed" if failure else "unexpected_skip"
                if skipped is not None or case.get("status") in ("notrun", "disabled") else "passed" if case.get("status") == "run" else "failed")
            if expected_outcomes.get(name) == ["mdk09_incompatibility"]:
                output = case.findtext("system-out", "")
                expected = (name == "groundhog-mdk011-interop-mdk09-probe" and not failure
                    and skipped is not None and skipped.get("message") == "SKIP_RETURN_CODE=77"
                    and "XFAIL [unsupported]: MDK 0.9.0" in output
                    and "unsupported proof version 1" in output and "XPASS" not in output)
                outcomes[name] = "expected_incompatibility" if expected else "failed"
    for name in names:
        outcomes.setdefault(name, "missing")
    if set(outcomes) != set(names):
        raise RealizationError("unexpected JUnit executions")
    return outcomes


def _diagnostic_execution_options(resolved, realization, parallel, timeout=None):
    if not any(e.get("authority") == "reviewed_diagnostic_nonmembers_v1"
               for e in resolved.get("baseline_exclusions", [])):
        return None
    profile = realization["profile"]
    expected = {"parallel": profile.get("parallelism", 1), "timeout_seconds": profile.get("timeout_seconds")}
    effective_timeout = expected["timeout_seconds"] if timeout is None else timeout
    if (type(parallel) is not int or parallel != expected["parallel"]
            or isinstance(effective_timeout, bool) or effective_timeout != expected["timeout_seconds"]):
        raise RealizationError("diagnostic nonmember parallel/timeout override differs from authored profile")
    return expected


def build_resolved(resolved, build_dir, parallel=1):
    """Build selected prerequisites before the mandatory post-build audit."""
    realization = load_realization(build_dir, resolved["source_identity"])
    if realization["profile_fingerprint"] != resolved["profile_fingerprint"]:
        raise RealizationError("build profile identity mismatch")
    _validate_resolved_phase(resolved, realization)
    _diagnostic_execution_options(resolved, realization, parallel)
    if not resolved["build_targets"]:
        return {"commands": [], "build_seconds": 0}
    cmd = ["cmake", "--build", str(build_dir), "--parallel", str(parallel)]
    if resolved["configuration"]:
        cmd += ["--config", resolved["configuration"]]
    cmd += ["--target", *resolved["build_targets"]]
    start = time.monotonic()
    output = _command(cmd, env=phase_environment(realization["profile"], realization["repo_root"], "build"))
    load_realization(build_dir, resolved["source_identity"])
    return {"commands": [cmd], "build_seconds": time.monotonic() - start, "output": output,
            "phase_context_hash": realization["phase_context_hash"]}


def cli(args, policy):
    """Adapter used by test-plan.py; return its documented 0/1/2 exit status.

    This does not provision the runner or synthesize unavailable profile coverage.
    A non-CTest/live lane without an existing adapter fails explicitly.
    """
    import sys
    try:
        if args.command == "run" and args.phase not in (*PHASES, "all"):
            raise RealizationError(f"unknown execution phase: {args.phase}")
        profile_id = args.profile
        profile = policy["profiles"][profile_id]
        plan = _read(args.plan) if args.plan else None
        identity = plan["source_identity"] if plan else None
        if args.configure or (args.command == "run" and args.phase == "configure"):
            if identity is None:
                status = _command(["git", "-C", args.repo, "status", "--porcelain"])
                if status:
                    raise RealizationError("dirty configure requires an explicit snapshot plan/source identity")
                identity = {"kind": "committed", "sha": _command(["git", "-C", args.repo, "rev-parse", "HEAD"]).strip()}
            realized = configure_profile(args.repo, args.build_dir, profile_id, profile, identity, _hash(policy))
        else:
            realized = load_realization(args.build_dir, identity)
        if realized["inventory_hash"] != _hash(policy) or realized["profile_id"] != profile_id or realized["profile"] != profile:
            raise RealizationError("realization inventory/profile policy changed; configure again")
        if args.command in ("validate", "check-inputs"):
            result = check_inputs(policy, realized, identity,
                                  after_build=args.command == "check-inputs" and args.phase != "configure")
            code = 0 if result["passed"] else 1
        elif args.command == "run" and args.phase == "configure":
            result, code = realized, 0
        else:
            if plan is None:
                raise ValueError("resolve/run requires a logical plan")
            result = resolve_checks(policy, plan, realized, profile_id)
            if args.resolved:
                previous = _read(args.resolved)
                if previous != result:
                    raise RealizationError("provided resolved plan is stale or differs from current resolution")
            code = 0
            if args.command == "run":
                pre = check_inputs(policy, realized, identity)
                if not pre["passed"]:
                    result, code = pre, 1
                else:
                    build_result = {"commands": [], "build_seconds": 0}
                    if args.phase in ("build", "all"):
                        build_result = build_resolved(result, args.build_dir, profile.get("parallelism", 1))
                    audit = check_inputs(policy, realized, identity, after_build=True)
                    if not audit["passed"]:
                        result, code = audit, 1
                    elif args.phase == "build":
                        result = dict(audit, build=build_result)
                    else:
                        evidence = Path(args.output).absolute().parent / "selection-evidence" if args.output else Path(args.build_dir) / "selection-evidence"
                        result = run_resolved(result, args.build_dir, evidence, args.attempt,
                            input_audit=audit, build=False, parallel=profile.get("parallelism", 1),
                            timeout=profile.get("timeout_seconds"),
                            evidence_paths=[Path(args.build_dir) / "mdk011-interop-artifacts"])
                        result["build"] = build_result
                        # cli() emits an aggregate-facing receipt; run_resolved()
                        # retains the actual post-build audit object for adapters.
                        result["input_audit_details"] = result["input_audit"]
                        result["input_audit"] = result["input_audit_status"]
                        code = 0 if result["outcome"] in ("passed", "not_applicable") else 1
        if args.output:
            _atomic(args.output, result)
        else:
            print(json.dumps(result, sort_keys=True, indent=2))
        return code
    except RealizationError as exc:
        print(f"test-plan: {exc}", file=sys.stderr)
        return 1
    except (OSError, ValueError, KeyError, TypeError) as exc:
        print(f"test-plan: malformed realization invocation: {exc}", file=sys.stderr)
        return 2


def _attempt_directory(resolved, evidence_dir, attempt):
    return (Path(evidence_dir) / resolved["source_identity"].get("head", resolved["source_identity"].get("sha", resolved["source_identity"].get("tested_sha", "source")))
            / resolved["profile_id"] / resolved["resolved_hash"] / f"attempt-{attempt}")


def run_resolved(resolved, build_dir, evidence_dir, attempt=1, **kwargs):
    """Retain interrupted/error attempts without ever overwriting prior evidence."""
    audit = kwargs.get("input_audit")
    if audit is None or not audit.get("passed") or not audit.get("after_build"):
        load_realization(build_dir, resolved["source_identity"])
        raise RealizationError("passing post-build input audit required before execution")
    directory = _attempt_directory(resolved, evidence_dir, attempt)
    existed = directory.exists()
    previous_handler = None
    def cancel(signum, frame):
        raise KeyboardInterrupt(f"execution cancelled by signal {signum}")
    try:
        # signal handlers belong to the main thread; library callers in worker
        # threads retain their caller's cancellation mechanism.
        try:
            previous_handler = signal.signal(signal.SIGTERM, cancel)
        except ValueError:
            pass
        return _run_resolved(resolved, build_dir, evidence_dir, attempt, **kwargs)
    except (Exception, KeyboardInterrupt) as exc:
        if existed:
            raise
        if not directory.exists():
            directory.mkdir(parents=True, exist_ok=True)
            (directory / "preexecution-failure.txt").write_text(f"{type(exc).__name__}: {exc}\n", errors="backslashreplace")
            raise
        if not existed:
            directory.mkdir(parents=True, exist_ok=True)
            path = directory / "receipt.json"
            receipt = _read(path) if path.exists() else {}
            outcome = "cancelled" if isinstance(exc, KeyboardInterrupt) else "failed"
            observed = receipt.setdefault("observed_executions", {})
            checks = receipt.setdefault("checks", {})
            for ident, check in resolved["checks"].items():
                checks.setdefault(ident, {"outcome": outcome, "executions": check["tests"],
                    "expected_executions": check["tests"], "observed_executions": []})
                for name in check["tests"]:
                    observed.setdefault(name, "missing")
            receipt.update(outcome=outcome, execution_outcome=outcome, job_conclusion="failure",
                           diagnostic_eligible=False, complete_gate_eligible=False,
                           error=str(exc), ended=time.time())
            receipt.update({k: resolved[k] for k in ("source_identity", "inventory_hash", "profile_id", "profile_fingerprint", "plan_hash", "resolved_hash")})
            receipt["schema_version"] = 1
            if hasattr(exc, "execution_status"):
                receipt["process_error"] = exc.execution_status
            _atomic(path, receipt)
            (directory / "execution-error.txt").write_text(f"{type(exc).__name__}: {exc}\n", errors="backslashreplace")
        if isinstance(exc, (KeyboardInterrupt, subprocess.TimeoutExpired, OSError, UnicodeError)):
            raise RealizationError(f"execution interrupted/failed; retained {directory}: {exc}") from exc
        raise
    finally:
        if previous_handler is not None:
            signal.signal(signal.SIGTERM, previous_handler)


def _run_resolved(resolved, build_dir, evidence_dir, attempt=1, *, input_audit=None,
                 build=True, parallel=1, timeout=None, evidence_paths=(), custody=None):
    """One CTest invocation per profile/attempt; no retries or quarantine policy.

    Existing platform/MDK adapters still own isolation locks, images and process
    cleanup. This runner refuses Docker/external-peer profiles until that adapter
    attests/provisions them; it must not race fixed-tag image lifecycles.
    """
    if attempt < 1 or parallel < 1:
        raise ValueError("attempt and parallel must be positive")
    build_dir = Path(build_dir).resolve()
    realization = load_realization(build_dir, resolved["source_identity"])
    if realization["profile_fingerprint"] != resolved["profile_fingerprint"]:
        raise RealizationError("resolved profile fingerprint mismatch")
    _validate_resolved_phase(resolved, realization)
    if timeout is None:
        timeout = realization["profile"].get("timeout_seconds")
    if timeout is not None and (type(timeout) not in (int, float) or timeout <= 0):
        raise RealizationError("execution timeout must be positive")
    execution_options = _diagnostic_execution_options(resolved, realization, parallel, timeout)
    if realization["profile"].get("execution_domain") == "prepared_peer_linux_v1":
        from test_selection_custody import PreparedContext
        if not isinstance(custody, PreparedContext):
            raise RealizationError("prepared profile requires live contained runtime custody")
        custody.validate(resolved)
    elif custody is not None or set(resolved["required_capabilities"]) & {"docker", "external_peer", "prepared_peer"}:
        raise RealizationError("live peer/image lifecycle requires the existing guarded adapter; not implemented here")
    directory = _attempt_directory(resolved, evidence_dir, attempt)
    previous_attempts = []
    for previous in range(1, attempt):
        path = directory.parent / f"attempt-{previous}" / "receipt.json"
        earlier = _read(path)
        if any(earlier.get(key) != resolved[key] for key in
               ("source_identity", "inventory_hash", "profile_fingerprint", "resolved_hash")):
            raise RealizationError("retry evidence identity mismatch")
        previous_attempts.append({"receipt": str(path), "outcome": earlier["outcome"],
                                  "checks": earlier.get("checks", {})})
    directory.mkdir(parents=True, exist_ok=False)
    started = time.time()
    commands = []
    receipt = {"schema_version": 1, "realization_kind": realization.get("realization_kind", "cmake"),
        "source_identity": resolved["source_identity"],
        "inventory_hash": resolved["inventory_hash"], "profile_id": resolved["profile_id"],
        "profile_fingerprint": resolved["profile_fingerprint"], "plan_hash": resolved["plan_hash"],
        "resolved_hash": resolved["resolved_hash"], "attempt": attempt,
        "lane_id": resolved["profile_id"], "profile": resolved["profile_id"],
        "plan_id": resolved["plan_id"], "profile_hash": resolved["profile_hash"],
        "configuration": resolved["configuration"],
        "phase_context": realization["phase_context"], "phase_context_hash": realization["phase_context_hash"],
        "registration_audit": "passed", "job_conclusion": "running",
        "expected_executions": resolved.get("expected_executions", resolved["tests"]), "support_tests": resolved["support_tests"],
        "started": started, "commands": commands, "evidence_dir": str(directory),
        "baseline_exclusions": resolved.get("baseline_exclusions", []),
        "coverage_gaps": resolved.get("coverage_gaps", []),
        "complete_gate_eligible": resolved.get("complete_gate_eligible", True),
        "audit_build_targets": resolved.get("audit_build_targets", []),
        "compatibility_build_targets": resolved.get("compatibility_build_targets", []),
        "required_capabilities": resolved["required_capabilities"],
        "available_capabilities": realization["profile"].get("capabilities", []),
        "target_ids": resolved["target_ids"], "build_targets": resolved["build_targets"],
        "command_specs_hash": resolved.get("command_specs_hash")}
    if any(e.get("authority") == "reviewed_diagnostic_nonmembers_v1" for e in receipt["baseline_exclusions"]):
        receipt["complete"] = False
        receipt["execution_options"] = execution_options
        receipt["nonmember_binding"] = _nonmember_binding(realization, {d["name"]: d for d in realization["descriptors"]})
    _atomic(directory / "receipt.json", dict(receipt, outcome="running"))
    if input_audit is None or not input_audit.get("passed") or not input_audit.get("after_build"):
        raise RealizationError("passing post-build input audit required before execution")
    if "nonmember_binding" in receipt and input_audit.get("nonmember_binding") != receipt["nonmember_binding"]:
        raise RealizationError("diagnostic nonmember audited generation binding changed")
    if input_audit.get("compatibility_build_targets", []) != resolved.get("compatibility_build_targets", []):
        raise RealizationError("input audit sanitizer_compatibility targets mismatch")
    if input_audit.get("baseline_exclusions", []) != resolved.get("baseline_exclusions", []):
        raise RealizationError("input audit baseline exclusion accounting mismatch")
    for key in ("source_identity", "inventory_hash", "profile_fingerprint", "phase_context", "phase_context_hash"):
        if input_audit.get(key) != resolved[key]:
            raise RealizationError(f"input audit identity mismatch: {key}")
    for name, digest in input_audit.get("input_hashes", {}).items():
        if not Path(name).is_file() or hashlib.sha256(Path(name).read_bytes()).hexdigest() != digest:
            raise RealizationError(f"input changed since audit: {name}")
    if input_audit.get("audit_hash") != _hash({k: v for k, v in input_audit.items() if k != "audit_hash"}):
        raise RealizationError("input audit content hash mismatch")
    if input_audit.get("command_specs_hash") != resolved.get("command_specs_hash"):
        raise RealizationError("input audit command contract mismatch")
    receipt["input_audit"] = input_audit
    receipt["input_audit_status"] = "passed"
    env = phase_environment(realization["profile"], realization["repo_root"], "test")
    build_env = phase_environment(realization["profile"], realization["repo_root"], "build")
    config = ["-C", resolved["configuration"]] if resolved["configuration"] else []
    if build and resolved["build_targets"]:
        cmd = ["cmake", "--build", str(build_dir), "--parallel", str(parallel)]
        if resolved["configuration"]:
            cmd += ["--config", resolved["configuration"]]
        cmd += ["--target", *resolved["build_targets"]]
        commands.append(cmd)
        _atomic(directory / "receipt.json", dict(receipt, outcome="running"))
        begin = time.monotonic()
        result = _execute(cmd, env=build_env, log_path=directory / "build.log", timeout=timeout)
        if result.returncode:
            raise RealizationError(f"selected prerequisite build failed: {result.returncode}")
        receipt["build_seconds"] = time.monotonic() - begin
    _atomic(directory / "receipt.json", dict(receipt, outcome="running"))
    query = ["ctest", "--test-dir", str(build_dir), *config, "--show-only=json-v1"]
    if resolved["build_targets"]:
        if realization["cmake"].get("generator", {}).get("name") != "Ninja":
            raise RealizationError("execution build-freshness proof requires the existing Ninja profile")
        dry_run = _command(["ninja", "-C", str(build_dir), "-n", *resolved["build_targets"]], env=build_env)
        if "ninja: no work to do." not in dry_run:
            raise RealizationError("selected targets are not up to date; build and re-audit before execution")
    fresh = [] if realization.get("realization_kind") == "command" else json.loads(_command(query, env=env))["tests"]
    if _registration_hash(fresh) != resolved["registration_hash"]:
        raise RealizationError("CTest registrations/properties changed after resolution")
    tests = {t["name"]: t for t in fresh}
    for exclusion in resolved.get("baseline_exclusions", []):
        if exclusion.get("authority") == "reviewed_diagnostic_nonmembers_v1":
            from test_selection import validate_nonmember_record
            descriptors = {d["name"]: d for d in realization["descriptors"]}
            contract = {k: exclusion[k] for k in ("schema_version", "profile_hash", "registrations", "required_closure", "review")}
            try:
                validate_nonmember_record(exclusion, contract, _nonmember_binding(realization, descriptors))
            except ValueError as exc:
                raise RealizationError(str(exc)) from exc
            _nonmember_partition(contract, tests, descriptors, contract["required_closure"])
            if set(exclusion["registrations"]) & set(resolved["tests"]):
                raise RealizationError("diagnostic nonmember overlaps execution closure")
            ids = _target_closure({i for name in contract["registrations"]
                for i in _mapped_targets(tests[name], descriptors.get(name, {}), realization)}, realization)
            targets = sorted({realization["targets"][i]["name"] for i in ids})
            if (sorted(ids) != exclusion["target_ids"] or targets != exclusion["audit_build_targets"]
                    or not set(targets).issubset(resolved["build_targets"])):
                raise RealizationError("diagnostic nonmember audit target closure changed")
    for name, command in resolved["registered_commands"].items():
        if tests[name].get("command") != command:
            raise RealizationError(f"CTest command changed after resolution: {name}")
    for name in resolved["tests"]:
        if not tests[name].get("command"):
            raise RealizationError(f"selected executable/command still unavailable: {name}")
    load_realization(build_dir, resolved["source_identity"])
    headers = {} if realization.get("realization_kind") == "command" else _ninja_headers(realization)
    if input_audit.get("header_index_hash") != _hash(headers):
        raise RealizationError("discovered header index changed since audit; re-audit after build")
    if _fixture_closure(resolved["primary_tests"], tests, {d["name"]: d for d in realization["descriptors"]}) != resolved["tests"]:
        raise RealizationError("fixture/order closure changed")
    if not resolved["tests"]:
        receipt.update(outcome="not_applicable", observed_executions={}, checks={})
    else:
        indices = [str(i + 1) for i, test in enumerate(fresh) if test["name"] in resolved["tests"]]
        selection = directory / "ctest-indices.txt"
        selection.write_text("0,0,1," + ",".join(indices) + "\n")
        args = ["ctest", "--test-dir", str(build_dir), *config, "-I", str(selection)]
        selected = json.loads(_command([*args, "--show-only=json-v1"], env=env))["tests"]
        if sorted(t["name"] for t in selected) != resolved["tests"]:
            raise RealizationError("CTest indexed selection differs from resolved closure")
        junit = directory / "results.xml"
        cmd = [*args, "--output-on-failure", "--output-junit", str(junit), "--parallel", str(parallel)]
        if timeout is not None:
            cmd += ["--timeout", str(timeout)]
        commands.append(cmd)
        begin = time.monotonic()
        # A nonzero exit is retained alongside structured results, never discarded.
        _atomic(directory / "receipt.json", dict(receipt, outcome="running"))
        process = _execute(cmd, env=env, log_path=directory / "ctest.log", timeout=timeout)
        receipt["test_seconds"] = time.monotonic() - begin
        receipt["ctest_exit_code"] = process.returncode
        for value in evidence_paths:
            path = Path(value)
            if path.exists():
                destination = directory / "runtime-evidence" / path.name
                destination.parent.mkdir(parents=True, exist_ok=True)
                if path.is_dir():
                    shutil.copytree(path, destination)
                else:
                    shutil.copy2(path, destination)
        outcomes = _execution_outcomes(junit, resolved["tests"], resolved["expected_outcomes"])
        receipt["observed_executions"] = outcomes
        receipt["checks"] = {ident: {"outcome": next((outcomes[n] for n in check["tests"] if outcomes[n] not in ("passed", "expected_incompatibility")),
                        "expected_incompatibility" if any(outcomes[n] == "expected_incompatibility" for n in check["tests"]) else "passed"),
                                               "executions": check["tests"],
                                               "expected_executions": check["tests"],
                                               "observed_executions": [n for n in check["tests"] if outcomes[n] != "missing"],
                                               **({"exception_case": "mdk09_incompatibility", "exit_code": 77}
                                                  if any(outcomes[n] == "expected_incompatibility" for n in check["tests"]) else {})}
                             for ident, check in resolved["checks"].items() if ident not in resolved.get("command_executions", {})}
        receipt["outcome"] = "passed" if not process.returncode and all(o in ("passed", "expected_incompatibility") for o in outcomes.values()) else "failed"
    # Commands are additional physical executions, not CTest aliases. One failed
    # support execution still fails the lane even if every primary check passed.
    specs = resolved.get("command_executions", {})
    for ident, spec in specs.items():
        command_base = build_env if spec["kind"] == "build_contract" else env
        if spec.get("phase") != ("build" if spec["kind"] == "build_contract" else "test"):
            raise RealizationError(f"command phase changed: {ident}")
        env_command = dict(command_base, **spec["environment"])
        if _hash(env_command) != spec["environment_hash"]:
            raise RealizationError(f"command environment changed: {ident}")
        for relative in spec["required_evidence"]:
            path = build_dir / relative
            if path.exists() or path.is_symlink():
                raise RealizationError(f"required evidence already exists before command (stale): {ident}/{relative}")
        commands.append(spec["argv"])
        logfile = directory / f"command-{ident}.log"
        _atomic(directory / "receipt.json", dict(receipt, outcome="running", active_command=ident))
        begin = time.monotonic()
        process = _execute(spec["argv"], cwd=spec["cwd"], env=env_command, log_path=logfile, timeout=timeout)
        artifacts, evidence_missing = {}, []
        for relative in spec["required_evidence"]:
            path = build_dir / relative
            if not path.is_file() or not path.resolve().is_relative_to(build_dir):
                evidence_missing.append(relative)
                continue
            copy = directory / "command-evidence" / ident / relative
            copy.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(path, copy)
            artifacts[relative] = {"path": str(copy), "sha256": hashlib.sha256(copy.read_bytes()).hexdigest()}
        outcome = "passed" if process.returncode == 0 and not evidence_missing else "failed"
        receipt["observed_executions"][ident] = outcome
        receipt["checks"][ident] = {"outcome": outcome, "executions": [ident], "expected_executions": [ident],
            "observed_executions": [ident], "exit_code": process.returncode, "log": str(logfile),
            "log_sha256": hashlib.sha256(logfile.read_bytes()).hexdigest(), "argv": spec["argv"],
            "cwd": spec["cwd"], "environment_hash": spec["environment_hash"], "requires": spec["requires"],
            "seconds": time.monotonic() - begin, "artifacts": artifacts, "missing_evidence": evidence_missing}
        if receipt["outcome"] == "not_applicable":
            receipt["outcome"] = outcome
        elif outcome != "passed":
            receipt["outcome"] = "failed"
    # A command must not change the admitted source or any audited input. Evidence
    # belongs in the build/evidence directory, never in source fixtures.
    load_realization(build_dir, resolved["source_identity"])
    for name, digest in input_audit.get("input_hashes", {}).items():
        if not Path(name).is_file() or hashlib.sha256(Path(name).read_bytes()).hexdigest() != digest:
            raise RealizationError(f"input changed during execution: {name}")
    receipt["execution_outcome"] = receipt["outcome"]
    receipt["execution_clean_first_attempt"] = attempt == 1 and receipt["execution_outcome"] == "passed"
    if not receipt["complete_gate_eligible"]:
        receipt["incomplete_reasons"] = (["explicit baseline exclusion prevents complete gate acceptance"]
                                         if receipt["baseline_exclusions"] else [])
        # Execution success is diagnostic only. Normalization preserves these
        # fields and complete aggregation rejects them independently of outcomes.
        receipt["incomplete_reasons"] += ["inventory/profile union is not complete"] if receipt["coverage_gaps"] else []
    if (input_audit.get("complete_gate_eligible", True) != receipt["complete_gate_eligible"]
            or input_audit.get("coverage_gaps", []) != receipt["coverage_gaps"]):
        receipt["outcome"] = "failed"
        receipt["incomplete_reasons"] = ["unrecognized diagnostic eligibility: resolution/audit disagree"]
    receipt["diagnostic_eligible"] = receipt["outcome"] == "passed"
    receipt["previous_attempts"] = previous_attempts
    receipt["clean_first_attempt"] = attempt == 1 and receipt["outcome"] == "passed"
    for ident, result in receipt["checks"].items():
        result["initial_failure"] = any(
            earlier["checks"].get(ident, {}).get("outcome", "missing") not in ("passed", "expected_incompatibility")
            for earlier in previous_attempts)
    receipt["job_conclusion"] = "success" if receipt["outcome"] in ("passed", "not_applicable") else "failure"
    receipt["ended"] = time.time()
    _atomic(directory / "receipt.json", receipt)
    return receipt
