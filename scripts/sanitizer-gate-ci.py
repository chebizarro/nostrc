#!/usr/bin/env python3
"""The pre-push sanitizer stage's view of hosted CI (nostrc-3han).

usage: sanitizer-gate-ci.py config [--workflow FILE] [--source-root DIR]
       sanitizer-gate-ci.py affected < CHANGED_PATHS
       sanitizer-gate-ci.py check-inputs --build-dir DIR [--source-root DIR]
                                         [--workflow FILE]

config: read the `groundhog-sanitizers` job of groundhog-ci.yml and print, as
bash assignments, what the gate needs to run that job exactly:
  GH_SAN_TESTS          the job's GROUNDHOG_SANITIZER_TESTS
  GH_SAN_TARGETS        what its build step builds for them
  GH_SAN_CONFIGURE      its configure step's cmake arguments after -S/-B
  GH_SAN_CONFIGURE_ENV  NAME=VALUE the configure step runs with: the
                        workflow's, the job's and the step's env (CC, CFLAGS;
                        not the test list, which only the step scripts read)
  GH_SAN_BUILD_ENV      the same for the build step
  GH_SAN_ENV            the same for the test step, then its run script's
                        exports (ASAN/UBSAN/LSAN options; $PWD becomes
                        --source-root)
  GH_SAN_SKIP_PATTERN   the ERE its test step rejects in the ctest log
  GH_SAN_TEST_JOBS      its ctest --parallel (1 without one): ASAN tests
                        time out and race under more load than CI gives them
The gate reads these at run time, so CI and the gate cannot drift apart. When
a step changes in a way this reader does not understand (another command in
the configure step, a new ctest option, a different target mapping, a step
key such as working-directory, `defaults`, a job container or services, an
expression in an env value, a step writing $GITHUB_ENV), it fails instead of
guessing: update the reader and scripts/linux-gate.sh with the job.

affected: delegate impact classification to the shared inventory; unknown
paths and incomplete sanitizer profile coverage broaden. Exit 0 to run, 1 to
skip, 2 for an invalid/unreadable contract (callers must run or fail, not skip).

check-inputs: every source file the job's build reads (ninja's inputs of its
targets, the deps log's headers, the configure inputs of build.ninja) must be
under SANITIZER_BUILD_PATHS in legacy mode; exit 1 naming each one that is
not. --shared-profile delegates to the shared realized input audit, without
fallback. Legacy audit remains until profile parity and realization are met.

No PyYAML: the macOS host that runs the pre-push hook has no yaml module, so
this reads the subset of YAML the workflows use (block mappings, sequences,
|/> block scalars, plain and quoted scalars).
"""

import argparse
import contextlib
import json
from pathlib import Path
import os
import re
import runpy
import shlex
import shutil
import signal
import subprocess
import sys
import time

WORKFLOW = ".github/workflows/groundhog-ci.yml"
JOB = "groundhog-sanitizers"
TESTS_VAR = "GROUNDHOG_SANITIZER_TESTS"

# Every source input of the job's build: what its targets compile, include
# and link, and what the configure reads. `check-inputs` proves the list
# against the build's ninja graph (a directory ends in "/"; third_party/ also
# matches a submodule pointer change, which git reports as the bare path).
SANITIZER_BUILD_PATHS = (
    "CMakeLists.txt",                 # the sanitizer switches, apply_sanitizers
    "NipOptions.cmake",
    "cmake/",                         # NostrcTestBus.cmake defines a test of the set
    "Testing/CMakeLists.txt",         # configure-only: the root adds these
    "benchmark/CMakeLists.txt",
    "libhanami/CMakeLists.txt",
    "libhanami/hanami.pc.in",
    "tools/CMakeLists.txt",
    "gnome/groundhog/",
    "gnome/common/",                  # gn-status-notifier, compiled into groundhog (W32)
    "nostr-gtk/",                     # nostr_gtk_portable: Markdown, references, issue fields (alpha 6)
    "gnome/seahorse/",                # gnostr-secret, linked by groundhog-identity
    "apps/gnostr/data/schemas/org.gnostr.gnostr.gschema.xml",  # Groundhog's test schemas
    "libnostr/",
    "libjson/",
    "libgo/",
    "nostr-gobject/",
    "libmarmot/",
    "marmot-gobject/",
    "nips/",
    "components/nostrdb/",
    "apps/relayd/include/relayd_async_storage.h",  # included by nostrdb storage
    "third_party/",
    "tests/",
)
# Keys the gate reproduces: of the job, and of its configure, build and test
# steps. Anything else fails `config`.
JOB_KEYS = {"name", "runs-on", "timeout-minutes", "env", "steps", "needs", "permissions", "if"}
STEP_KEYS = {"name", "run", "env"}
RUNS_ON = "ubuntu-24.04"              # scripts/linux-ci.Dockerfile's base image

# ctest options of the job's test step that the gate reproduces (the gate
# passes --output-on-failure, --no-tests=error and -R itself, and its own
# parallelism); an option outside this set fails `config`.
KNOWN_CTEST_OPTIONS = {"--test-dir": 1, "--parallel": 1, "-j": 1, "--output-on-failure": 0,
                       "--no-tests=error": 0, "--output-junit": 1, "-R": 1}


class WorkflowError(Exception):
    pass


# ---- a YAML subset --------------------------------------------------------

def _indent(line):
    return len(line) - len(line.lstrip(" "))


def _scalar(text):
    text = text.strip()
    if text[:1] in ("'", '"'):
        quote = text[0]
        end = text.find(quote, 1)
        while quote == "'" and end != -1 and text[end + 1:end + 2] == "'":
            end = text.find(quote, end + 2)
        if end == -1:
            raise WorkflowError("unterminated quoted scalar: " + text)
        body = text[1:end]
        return body.replace("''", "'") if quote == "'" else body.encode().decode("unicode_escape")
    return re.sub(r"\s+#.*$", "", text)


class _Reader:
    KEY_RE = re.compile(r"^([^\s#'\"][^:#]*?|'[^']*'|\"[^\"]*\"):(?:\s+(.*))?$")

    def __init__(self, text):
        self.lines = text.expandtabs().splitlines()
        self.i = 0

    def _skip(self):
        while self.i < len(self.lines):
            stripped = self.lines[self.i].strip()
            if stripped and not stripped.startswith("#"):
                return True
            self.i += 1
        return False

    def node(self, min_indent):
        if not self._skip():
            return None
        line = self.lines[self.i]
        k = _indent(line)
        if k < min_indent:
            return None
        body = line[k:]
        if body == "-" or body.startswith("- "):
            return self.sequence(k)
        if self.KEY_RE.match(body):
            return self.mapping(k)
        self.i += 1
        return _scalar(body)

    def sequence(self, k):
        items = []
        while self._skip():
            line = self.lines[self.i]
            body = line[k:]
            if _indent(line) != k or not (body == "-" or body.startswith("- ")):
                break
            # The item is what follows "- ", as if indented by two more.
            self.lines[self.i] = " " * (k + 2) + body[2:]
            items.append(self.node(k + 1))
        return items

    def mapping(self, k):
        result = {}
        while self._skip():
            line = self.lines[self.i]
            if _indent(line) != k:
                if _indent(line) > k:
                    raise WorkflowError("line %d: unexpected indentation" % (self.i + 1))
                break
            m = self.KEY_RE.match(line[k:])
            if not m:
                break
            key, rest = _scalar(m.group(1)), (m.group(2) or "").strip()
            self.i += 1
            if rest[:1] in ("|", ">"):
                result[key] = self.block(k, rest)
            elif rest == "" or rest.startswith("#"):
                # A nested node, or a sequence at the key's own indentation.
                nxt = self.node(k + 1)
                if nxt is None and self._skip() and self.lines[self.i][k:].startswith("- ") \
                        and _indent(self.lines[self.i]) == k:
                    nxt = self.sequence(k)
                result[key] = nxt
            else:
                result[key] = _scalar(rest)
        return result

    def block(self, k, header):
        style, chomp = header[0], ""
        if "-" in header[1:].split("#")[0]:
            chomp = "-"
        elif "+" in header[1:].split("#")[0]:
            chomp = "+"
        raw = []
        content_indent = None
        while self.i < len(self.lines):
            line = self.lines[self.i]
            if line.strip():
                if content_indent is None:
                    content_indent = _indent(line)
                if _indent(line) < content_indent or _indent(line) <= k:
                    break
                raw.append(line[content_indent:])
            else:
                raw.append("")
            self.i += 1
        while raw and raw[-1] == "" and chomp != "+":
            raw.pop()
        if style == "|":
            text = "\n".join(raw)
        else:
            text, pending = "", ""
            for line in raw:
                if line == "":
                    pending += "\n"
                    continue
                if text and not pending:
                    text += " "
                text += pending + line
                pending = ""
        return text if chomp == "-" else text + "\n"


def parse_yaml(text):
    return _Reader(text).node(0)


# ---- the job ----------------------------------------------------------------

def _commands(script):
    """Shell commands of a run script: continuations joined, comments and
    blank lines dropped."""
    commands, current = [], ""
    for line in script.splitlines():
        if not current and (not line.strip() or line.strip().startswith("#")):
            continue
        if line.rstrip().endswith("\\"):
            current += line.rstrip()[:-1] + " "
            continue
        commands.append((current + line).strip())
        current = ""
    if current:
        commands.append(current.strip())
    return commands


def _step(steps, needle, what):
    found = [s for s in steps if isinstance(s, dict) and needle in str(s.get("run", ""))]
    if len(found) != 1:
        raise WorkflowError("expected one %s step (a run: containing %r) in job %s, found %d"
                            % (what, needle, JOB, len(found)))
    return found[0]


BUILD_SHAPE = [
    r"targets=\((?P<initial>[^)]*)\)",
    r'for test in \$%s; do' % TESTS_VAR,
    r'case "\$test" in groundhog-\*\) targets\+=\("test-\$test"\) ;; \*\) targets\+=\("\$test"\) ;; esac',
    r"done",
    r'cmake --build (?P<dir>\S+) --parallel \d+ --target "\$\{targets\[@\]\}"',
]


def _env(mapping, where):
    """(name, value) pairs of an env: mapping; expressions are not reproduced."""
    if mapping is None:
        return []
    if not isinstance(mapping, dict):
        raise WorkflowError("%s env is not a mapping" % where)
    pairs = []
    for name, value in mapping.items():
        value = "" if value is None else str(value)
        if not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", str(name)):
            raise WorkflowError("%s env name %r" % (where, name))
        if "${{" in value or "\n" in value:
            raise WorkflowError("%s env %s is not a plain value: %r" % (where, name, value))
        pairs.append((str(name), value))
    return pairs


def _merge(*envs):
    """Later envs override earlier ones, as a step's env overrides its job's."""
    merged = {}
    for env in envs:
        for name, value in env:
            merged.pop(name, None)
            merged[name] = value
    return list(merged.items())


def _check_step(step, what):
    extra = sorted(set(step) - STEP_KEYS)
    if extra:
        raise WorkflowError("the %s step has %s, which the gate does not reproduce"
                            % (what, ", ".join(extra)))


def job_config(text, source_root):
    doc = parse_yaml(text)
    if not isinstance(doc, dict):
        raise WorkflowError("not a workflow")
    if "defaults" in doc:
        raise WorkflowError("workflow-level defaults, which the gate does not reproduce")
    try:
        job = doc["jobs"][JOB]
    except (KeyError, TypeError):
        raise WorkflowError("no job %s" % JOB)
    extra = sorted(set(job) - JOB_KEYS)
    if extra:
        raise WorkflowError("job %s has %s, which the gate does not reproduce"
                            % (JOB, ", ".join(extra)))
    if job.get("runs-on") != RUNS_ON:
        raise WorkflowError("job %s runs on %r; the gate's image is %s"
                            % (JOB, job.get("runs-on"), RUNS_ON))
    common = _merge(_env(doc.get("env"), "workflow"), _env(job.get("env"), "job"))
    tests = str((job.get("env") or {}).get(TESTS_VAR, "")).split()
    if not tests:
        raise WorkflowError("job %s has no %s" % (JOB, TESTS_VAR))
    if len(set(tests)) != len(tests):
        raise WorkflowError("%s lists a test twice" % TESTS_VAR)
    steps = job.get("steps") or []
    for step in steps:
        if isinstance(step, dict) and re.search(r"GITHUB_(ENV|PATH)", str(step.get("run", ""))):
            raise WorkflowError("step %r writes $GITHUB_ENV/$GITHUB_PATH, which the gate "
                                "does not reproduce" % step.get("name"))

    # Configure: one cmake command, whose arguments the gate passes as they are.
    configure_step = _step(steps, "cmake -S", "configure")
    _check_step(configure_step, "configure")
    configure = _commands(configure_step["run"])
    if len(configure) != 1:
        raise WorkflowError("the configure step runs %d commands, not one cmake: %r"
                            % (len(configure), configure))
    argv = shlex.split(configure[0])
    if argv[:2] != ["cmake", "-S"] or len(argv) < 5 or argv[3] != "-B":
        raise WorkflowError("unexpected configure command: " + configure[0])
    build_dir = argv[4]
    configure_args = argv[5:]
    if any("$" in a for a in configure_args):
        raise WorkflowError("configure arguments expand variables: %r" % configure_args)

    # Build: groundhog plus one target per test, mapped as the step maps them.
    build_step = _step(steps, "cmake --build", "build")
    _check_step(build_step, "build")
    build = _commands(build_step["run"])
    if len(build) != len(BUILD_SHAPE):
        raise WorkflowError("the build step changed shape: %r" % build)
    initial = None
    for command, shape in zip(build, BUILD_SHAPE):
        m = re.fullmatch(shape, command)
        if not m:
            raise WorkflowError("the build step changed shape at %r" % command)
        if "initial" in m.groupdict():
            initial = m.group("initial").split()
        if "dir" in m.groupdict() and m.group("dir") != build_dir:
            raise WorkflowError("the build step builds %s, configure makes %s"
                                % (m.group("dir"), build_dir))
    targets = initial + [("test-" + t) if t.startswith("groundhog-") else t for t in tests]

    # Test run: the step's env, its exports, its ctest options, its skip check.
    run_step = _step(steps, "ctest", "test")
    _check_step(run_step, "test")
    env = []
    script = run_step["run"]
    for command in _commands(script):
        m = re.fullmatch(r"export (\w+)=(.*)", command)
        if m:
            (value,) = shlex.split(m.group(2))
            value = value.replace("${PWD}", source_root).replace("$PWD", source_root)
            if "$" in value:
                raise WorkflowError("export %s expands more than $PWD: %s" % (m.group(1), value))
            env.append((m.group(1), value))
        elif re.match(r"export\b|[A-Za-z_]\w*=\S*\s+ctest\b|[A-Z_][A-Z0-9_]*=", command):
            raise WorkflowError("the test step sets a variable the gate does not read: " + command)
    if 'read -ra tests <<< "$%s"' % TESTS_VAR not in script or \
            "regex=\"^($(IFS='|'; echo \"${tests[*]}\"))\\$\"" not in script:
        raise WorkflowError("the test step no longer selects exactly %s" % TESTS_VAR)
    ctest = [c for c in _commands(script) if c.startswith("ctest --test-dir %s " % build_dir)
             and " -N " not in c]
    if len(ctest) != 1:
        raise WorkflowError("expected one ctest run of %s in the test step, found %r"
                            % (build_dir, ctest))
    words = shlex.split(ctest[0].split("|")[0], posix=True)[1:]
    test_jobs = "1"
    i = 0
    while i < len(words):
        option = words[i]
        if option not in KNOWN_CTEST_OPTIONS:
            raise WorkflowError("the test step passes ctest %r, which the gate does not "
                                "reproduce" % option)
        if option in ("--parallel", "-j"):
            test_jobs = words[i + 1] if i + 1 < len(words) else ""
            if not test_jobs.isdigit() or int(test_jobs) < 1:
                raise WorkflowError("ctest %s %r is not a job count" % (option, test_jobs))
        i += 1 + KNOWN_CTEST_OPTIONS[option]
    skip = re.search(r"grep -Eq '([^']+)' \S*ctest\.log", script)
    if not skip:
        raise WorkflowError("the test step no longer rejects skipped tests (grep -Eq '...')")
    env = _merge(common, _env(run_step.get("env"), "test step"), env)
    pairs = lambda e: ["%s=%s" % pair for pair in e]
    # The test list reaches CMake only through the build step's script, which
    # GH_SAN_TARGETS reproduces; kept out of the configure record, a change to
    # the list does not start the build tree afresh.
    common = [(n, v) for n, v in common if n != TESTS_VAR]
    return {
        "GH_SAN_TESTS": tests,
        "GH_SAN_TARGETS": targets,
        "GH_SAN_CONFIGURE": configure_args,
        "GH_SAN_CONFIGURE_ENV": pairs(_merge(common, _env(configure_step.get("env"), "configure step"))),
        "GH_SAN_BUILD_ENV": pairs(_merge(common, _env(build_step.get("env"), "build step"))),
        "GH_SAN_ENV": pairs(env),
        "GH_SAN_SKIP_PATTERN": skip.group(1),
        "GH_SAN_TEST_JOBS": test_jobs,
    }


def as_bash(config):
    out = []
    for name, value in config.items():
        if isinstance(value, list):
            out.append("%s=(%s)" % (name, " ".join(shlex.quote(v) for v in value)))
        else:
            out.append("%s=%s" % (name, shlex.quote(value)))
    return "\n".join(out) + "\n"


# ---- the path filter ----------------------------------------------------------

def _under(path, prefix):
    return path == prefix or (prefix.endswith("/") and
                              (path.startswith(prefix) or path == prefix[:-1]))


def affected(paths, source_root=None, inventory=None):
    """Shared impact selection; unknown paths broaden, never a legacy skip."""
    root = Path(source_root or Path(__file__).resolve().parents[1])
    sys.path.insert(0, str(root / "scripts"))
    import test_selection as shared
    policy = shared.load_inventory(inventory or root / "tests/selection/inventory.json")
    if not paths:
        return False, "the range changes no files"
    changes = [{"status": "M", "old_path": p, "new_path": p} for p in paths]
    plan = shared.make_plan(policy, changes, head="0" * 40, context="local")
    if plan["broadening_reasons"]:
        return True, "shared selection broadened: " + "; ".join(plan["broadening_reasons"])
    profiles = [name for name, p in policy["profiles"].items() if "sanitizer_compatibility" in p]
    if not profiles:
        if any(shared.classify(policy, p)["effect"] != "non_execution" for p in paths):
            return True, "shared sanitizer profile incomplete: conservative broad sanitizer coverage"
    else:
        for profile in profiles:
            expected = shared_config(policy, profile, str(root))
            observed = job_config((root / WORKFLOW).read_text(), str(root))
            if expected != observed:
                raise WorkflowError("shared/workflow sanitizer parity mismatch: " + profile)
    if (not policy.get("enforcement_ready", False) or policy.get("coverage_gaps")) and any(
            shared.classify(policy, p)["effect"] != "non_execution" for p in paths):
        return True, "shared sanitizer coverage incomplete: conservative broad sanitizer coverage"
    # During staged migration the existing workflow remains a parity oracle;
    # compare logical registrations rather than retaining a second path filter.
    config = job_config((root / WORKFLOW).read_text(), str(root))
    tests = set(config["GH_SAN_TESTS"])
    for ident in plan["selected_checks"]:
        check = policy["checks"][ident]
        registrations = {name for names in check.get("ctest_tests", {}).values() for name in names}
        if registrations & tests:
            return True, "shared selection affects sanitizer check " + ident
    return False, "shared selection contains no sanitizer execution"


def shared_config(policy, profile_id, source_root):
    """Compatibility metadata API for the inventory owner (no guessed defaults).

    profile.sanitizer_compatibility contains the GH_SAN_* config object, with
    {source_root} substitutions only. configure_args/environment must agree
    with it; missing metadata is failure, never fallback to the workflow.
    The parser remains a strict migration parity oracle until W11 cutover.
    """
    try:
        profile = policy["profiles"][profile_id]
        config = profile["sanitizer_compatibility"]
    except KeyError as exc:
        raise WorkflowError(f"missing shared sanitizer metadata: {profile_id}: {exc}") from exc
    def expand(value):
        if isinstance(value, str):
            return value.replace("{source_root}", source_root)
        if isinstance(value, list):
            return [expand(v) for v in value]
        if isinstance(value, dict):
            return {k: expand(v) for k, v in value.items()}
        raise WorkflowError("unsupported shared sanitizer metadata type")
    config = expand(config)
    arrays = {"GH_SAN_TESTS", "GH_SAN_TARGETS", "GH_SAN_CONFIGURE", "GH_SAN_CONFIGURE_ENV", "GH_SAN_BUILD_ENV", "GH_SAN_ENV"}
    if not isinstance(config, dict) or set(config) != arrays | {"GH_SAN_SKIP_PATTERN", "GH_SAN_TEST_JOBS"}:
        raise WorkflowError("shared sanitizer config has missing/unsupported fields")
    for name in arrays:
        if not isinstance(config[name], list) or not all(isinstance(x, str) and x for x in config[name]):
            raise WorkflowError("shared sanitizer config requires string argv: " + name)
    if not config["GH_SAN_TESTS"] or not config["GH_SAN_TARGETS"]:
        raise WorkflowError("shared sanitizer test/target selection is empty")
    if not isinstance(config["GH_SAN_TEST_JOBS"], str) or not config["GH_SAN_TEST_JOBS"].isdigit() or int(config["GH_SAN_TEST_JOBS"]) < 1:
        raise WorkflowError("shared sanitizer test parallelism is invalid")
    if not isinstance(config["GH_SAN_SKIP_PATTERN"], str) or not config["GH_SAN_SKIP_PATTERN"]:
        raise WorkflowError("shared sanitizer skip policy is absent")
    if config.get("GH_SAN_CONFIGURE") != expand(profile.get("configure_args")):
        raise WorkflowError("shared sanitizer configure_args disagree with compatibility config")
    base = profile.get("environment")
    phases = profile.get("phase_environments")
    if not isinstance(base, dict) or not isinstance(phases, dict) or set(phases) != {"configure", "build", "test"}:
        raise WorkflowError("shared sanitizer requires explicit configure/build/test phase environments")
    for phase, field in (("configure", "GH_SAN_CONFIGURE_ENV"), ("build", "GH_SAN_BUILD_ENV"), ("test", "GH_SAN_ENV")):
        overlay = phases[phase]
        if not isinstance(overlay, dict) or any(not isinstance(k, str) or not isinstance(v, str) for env in (base, overlay) for k, v in env.items()):
            raise WorkflowError("invalid shared sanitizer phase environment: " + phase)
        environment = dict(item.split("=", 1) for item in config[field])
        if len(environment) != len(config[field]) or environment != expand(dict(base, **overlay)):
            raise WorkflowError("shared sanitizer " + phase + " environment disagrees with compatibility config")
    registrations = [name for check in policy["checks"].values()
                     for name in check.get("ctest_tests", {}).get(profile_id, [])]
    if len(registrations) != len(set(registrations)) or set(registrations) != set(config.get("GH_SAN_TESTS", [])):
        raise WorkflowError("shared sanitizer exact check membership disagrees with compatibility config")
    return config


def configuration(workflow, source_root, inventory=None, profile=None):
    observed = job_config(Path(workflow).read_text(), source_root)
    if profile is None:
        # Legacy behavior remains explicit until the parent provides/accepts the
        # shared profile. This branch is not evidence of W7 completion.
        return observed
    with open(inventory, encoding="utf-8") as stream:
        policy = json.load(stream)
    config = shared_config(policy, profile, source_root)
    if config != observed:
        changed = sorted(k for k in set(config) | set(observed) if config.get(k) != observed.get(k))
        raise WorkflowError("shared/workflow sanitizer parity mismatch: " + ", ".join(changed))
    return config


def build_inputs(build_dir, source_root, targets):
    """The source files (relative to source_root) that building targets in
    build_dir reads, by ninja: the targets' inputs, recursively; the headers
    of every object in the deps log; the configure's inputs (build.ninja's)."""
    def ninja(*args):
        return subprocess.run(["ninja", "-C", build_dir] + list(args), check=True,
                              stdout=subprocess.PIPE, universal_newlines=True).stdout
    raw = set()
    for out in (ninja("-t", "inputs", *targets), ninja("-t", "inputs", "build.ninja")):
        raw.update(line.strip() for line in out.splitlines() if line.strip())
    raw.update(line.strip() for line in ninja("-t", "deps").splitlines()
               if line[:1] in (" ", "\t") and line.strip())
    src = os.path.realpath(source_root)
    build = os.path.realpath(build_dir)
    found = set()
    for path in raw:
        path = os.path.realpath(path if os.path.isabs(path) else os.path.join(build, path))
        if path == build or path.startswith(build + os.sep):
            continue  # generated
        if path.startswith(src + os.sep):
            found.add(os.path.relpath(path, src))
    return sorted(found)


def check_inputs(inputs):
    """The inputs no SANITIZER_BUILD_PATHS entry covers."""
    return [p for p in inputs if not any(_under(p, prefix) for prefix in SANITIZER_BUILD_PATHS)]


def run_shared(args):
    """Explicit single-attempt diagnostic; the shared core owns all execution."""
    import fcntl  # shared Linux/macOS execution only; legacy parsing stays portable
    root = Path(args.source_root).resolve()
    evidence = Path(args.evidence_dir).resolve()
    evidence.mkdir(parents=True, exist_ok=False)
    config = configuration(root / WORKFLOW, str(root), root / args.inventory, args.shared_profile)
    if os.environ.get("NOSTRC_CTEST_QUARANTINE_PLATFORM") or os.environ.get("RERUN_MAX"):
        raise WorkflowError("shared diagnostic does not support quarantine/retry overrides; use the unchanged legacy route")
    sys.path.insert(0, str(root / "scripts"))
    cli = runpy.run_path(str(root / "scripts/test-plan.py"))
    shared = cli["selection"]
    import test_selection_realization as backend
    if not callable(getattr(backend, "phase_environment", None)):
        raise WorkflowError("shared phase-aware core API is unsupported; no legacy Ninja fallback")
    policy = shared.load_inventory(root / args.inventory)
    profile = policy["profiles"][args.shared_profile]
    if "gnome_keyring" in profile.get("capabilities", []):
        raise WorkflowError("gnome_keyring requires attested private HOME/private bus/throwaway Secret Service custody; shared adapter not implemented")
    if "nak" in profile.get("capabilities", []) and not shutil.which("nak"):
        raise WorkflowError("selected nak capability requires the actual nak executable")
    unsupported = set(profile.get("capabilities", [])) - {"display", "private_bus", "nak"}
    if unsupported:
        raise WorkflowError("shared diagnostic requires an unimplemented guarded capability: " + str(sorted(unsupported)))
    for cap, variable in (("display", "DISPLAY"), ("private_bus", "DBUS_SESSION_BUS_ADDRESS")):
        if cap in profile.get("capabilities", []) and not os.environ.get(variable):
            raise WorkflowError("shared diagnostic is missing isolated " + cap)
    jobs = args.test_jobs if args.test_jobs is not None else int(config["GH_SAN_TEST_JOBS"])
    if args.jobs < 1 or jobs < 1:
        raise WorkflowError("shared diagnostic parallelism must be positive")
    head = shared.commit_sha(root, "HEAD")
    plan = shared.git_plan(root, policy, head=head, base=head, context="local", mode="broad",
                           event={"kind": "local", "base_sha": head}, profiles=[args.shared_profile])
    plan_path = evidence / "plan.json"
    resolved_path = evidence / "resolved.json"
    shared.write_json(plan_path, plan)
    common = ["--repo", str(root), "--inventory", args.inventory, "--profile", args.shared_profile,
              "--plan", str(plan_path), "--build-dir", args.build_dir, "--evidence-dir", str(evidence / "execution"),
              "--timeout", str(profile["timeout_seconds"])]

    def invoke(name, argv):
        with (evidence / (name + ".log")).open("x") as log, contextlib.redirect_stdout(log), contextlib.redirect_stderr(log):
            status = cli["main"](argv)
        if status:
            raise WorkflowError(f"shared core {name} failed ({status}); evidence={evidence}")

    def cancel(signum, frame):
        raise KeyboardInterrupt("shared diagnostic cancelled")
    signal.signal(signal.SIGTERM, cancel)
    # Same filesystem locks/ordering as linux-gate's legacy route. One process
    # keeps the ambient environment stable through all core phase invocations.
    locks = Path(args.lock_dir)
    with (locks / "build.lock").open("a") as build_lock, (locks / "tests.lock").open("a") as tests_lock:
        fcntl.flock(build_lock, fcntl.LOCK_SH)
        invoke("resolve", ["resolve", *common, "--configure", "--output", str(resolved_path)])
        resolved = shared.read_json(resolved_path)
        if not resolved.get("phase_context_hash"):
            raise WorkflowError("shared resolution did not retain phase context; no legacy fallback")
        if set(resolved["primary_tests"]) != set(config["GH_SAN_TESTS"]):
            raise WorkflowError("shared resolved sanitizer test membership differs from compatibility contract")
        if not set(config["GH_SAN_TARGETS"]).issubset(resolved["build_targets"]):
            raise WorkflowError("shared core does not resolve all sanitizer compatibility build targets")
        invoke("build", ["run", *common, "--resolved", str(resolved_path), "--phase", "build",
                         "--parallel", str(args.jobs), "--output", str(evidence / "build.json")])
        invoke("postbuild-inputs", ["check-inputs", "--repo", str(root), "--inventory", args.inventory,
              "--profile", args.shared_profile, "--build-dir", args.build_dir, "--after-build",
              "--output", str(evidence / "postbuild-inputs.json")])
        fcntl.flock(build_lock, fcntl.LOCK_UN)
        if args.host_signal:
            # Polling here is the existing host-build coordination protocol,
            # never a substitute for test/relay readiness or retry-as-success.
            while not Path(args.host_signal).exists():
                time.sleep(1)
        fcntl.flock(build_lock, fcntl.LOCK_EX)
        fcntl.flock(tests_lock, fcntl.LOCK_EX)
        invoke("test", ["run", *common, "--resolved", str(resolved_path), "--phase", "test",
                        "--parallel", str(jobs), "--output", str(evidence / "receipt.json")])
        forbidden = re.compile(config["GH_SAN_SKIP_PATTERN"].encode())
        for path in (evidence / "execution").rglob("ctest*.log"):
            if forbidden.search(path.read_bytes()):
                raise WorkflowError("shared sanitizer skip/not-run output is forbidden: " + str(path))
        invoke("aggregate", ["aggregate", "--repo", str(root), "--inventory", args.inventory,
              "--plan", str(plan_path), "--receipt", str(evidence / "receipt.json"), "--partial",
              "--output", str(evidence / "aggregate.json")])
    print(f"shared sanitizer diagnostic passed (single attempt); NOT complete CI/LSan acceptance; evidence={evidence}")
    return 0


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = parser.add_subparsers(dest="command", required=True)
    cfg = sub.add_parser("config")
    cfg.add_argument("--workflow", default=WORKFLOW)
    cfg.add_argument("--source-root", default=".")
    cfg.add_argument("--shared-profile", help="require shared config and exact workflow parity")
    cfg.add_argument("--inventory", default="tests/selection/inventory.json")
    cfg.add_argument("--json", action="store_true", help="emit config JSON for inventory-owner handoff")
    aff = sub.add_parser("affected")
    aff.add_argument("--source-root")
    aff.add_argument("--inventory")
    chk = sub.add_parser("check-inputs")
    chk.add_argument("--build-dir", required=True)
    chk.add_argument("--source-root", default=".")
    chk.add_argument("--workflow")
    chk.add_argument("--shared-profile", help="use shared realized input audit; no legacy fallback")
    chk.add_argument("--inventory", default="tests/selection/inventory.json")
    run = sub.add_parser("shared-run", help="explicit phase-aware diagnostic; no legacy retry/quarantine fallback")
    run.add_argument("--source-root", required=True)
    run.add_argument("--shared-profile", required=True)
    run.add_argument("--inventory", default="tests/selection/inventory.json")
    run.add_argument("--build-dir", required=True)
    run.add_argument("--evidence-dir", required=True)
    run.add_argument("--lock-dir", required=True)
    run.add_argument("--host-signal")
    run.add_argument("--jobs", type=int, default=1)
    run.add_argument("--test-jobs", type=int)
    args = parser.parse_args(argv)
    if args.command == "shared-run":
        try:
            return run_shared(args)
        except KeyboardInterrupt:
            print("sanitizer-gate-ci: shared diagnostic cancelled; retained incomplete evidence", file=sys.stderr)
            return 130
        except (OSError, ValueError, WorkflowError, subprocess.SubprocessError) as exc:
            print("sanitizer-gate-ci: shared diagnostic failed: " + str(exc), file=sys.stderr)
            return 2
    if args.command == "check-inputs":
        if args.shared_profile:
            # The realization/backend owns the audit. A legacy Ninja-only tree
            # without realization must visibly fail this explicit shared mode.
            return subprocess.call([sys.executable, str(Path(args.source_root) / "scripts/test-plan.py"),
                                    "check-inputs", "--repo", args.source_root,
                                    "--inventory", args.inventory, "--profile", args.shared_profile,
                                    "--build-dir", args.build_dir, "--after-build"])
        workflow = args.workflow or os.path.join(args.source_root, WORKFLOW)
        try:
            with open(workflow, encoding="utf-8") as f:
                targets = job_config(f.read(), args.source_root)["GH_SAN_TARGETS"]
            inputs = build_inputs(args.build_dir, args.source_root, targets)
        except (OSError, WorkflowError, ValueError, subprocess.CalledProcessError) as e:
            print("sanitizer-gate-ci: check-inputs: %s" % e, file=sys.stderr)
            return 2
        if not any(p.startswith("gnome/groundhog/") for p in inputs):
            print("sanitizer-gate-ci: check-inputs: no Groundhog input among %d: is %s the "
                  "job's build?" % (len(inputs), args.build_dir), file=sys.stderr)
            return 2
        missing = check_inputs(inputs)
        if missing:
            print("sanitizer-gate-ci: %d input(s) of the job's build are outside "
                  "SANITIZER_BUILD_PATHS, so a push changing only them would skip the "
                  "sanitizer stage; add them to scripts/sanitizer-gate-ci.py:" % len(missing),
                  file=sys.stderr)
            for path in missing:
                print("  " + path, file=sys.stderr)
            return 1
        print("check-inputs: all %d source inputs of the job's build are under "
              "SANITIZER_BUILD_PATHS" % len(inputs))
        return 0
    if args.command == "affected":
        paths = [line.rstrip("\n") for line in sys.stdin if line.strip()]
        try:
            run, why = affected(paths, args.source_root, args.inventory)
        except Exception as exc:
            print("sanitizer-gate-ci: shared selection failed: %s" % exc, file=sys.stderr)
            return 2
        print(why)
        return 0 if run else 1
    try:
        config = configuration(args.workflow, args.source_root, args.inventory, args.shared_profile)
        sys.stdout.write(json.dumps(config, sort_keys=True) + "\n" if args.json else as_bash(config))
    except (OSError, WorkflowError, ValueError) as e:
        print("sanitizer-gate-ci: %s: %s" % (args.workflow, e), file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
