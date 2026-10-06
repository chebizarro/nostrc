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

affected: exit 0 when one of the changed paths (one per line on stdin) is
under SANITIZER_PATHS, 1 when none is; print why either way.

check-inputs: every source file the job's build reads (ninja's inputs of its
targets, the deps log's headers, the configure inputs of build.ninja) must be
under SANITIZER_BUILD_PATHS, or `affected` could skip a push that changes the
job's result; exit 1 naming each one that is not. Run by the gate after each
sanitizer build and by the job itself.

No PyYAML: the macOS host that runs the pre-push hook has no yaml module, so
this reads the subset of YAML the workflows use (block mappings, sequences,
|/> block scalars, plain and quoted scalars).
"""

import argparse
import os
import re
import shlex
import subprocess
import sys

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
# ... and what is not a build input but changes how the job runs here.
SANITIZER_PATHS = SANITIZER_BUILD_PATHS + (
    WORKFLOW,
    "scripts/linux-ci.Dockerfile",    # the compiler, sanitizer runtime and libraries
    "scripts/linux-gate.sh",          # the stage itself
    "scripts/linux-gate-smoke.sh",
    "scripts/sanitizer-gate-ci.py",
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


def affected(paths):
    """(True, why) when a path is under SANITIZER_PATHS, else (False, why)."""
    hits = {}
    for path in paths:
        for prefix in SANITIZER_PATHS:
            if _under(path, prefix):
                hits.setdefault(prefix, []).append(path)
    if hits:
        return True, "the range touches " + ", ".join(
            "%s (%d: %s%s)" % (p, len(v), v[0], ", ..." if len(v) > 1 else "")
            for p, v in sorted(hits.items()))
    if not paths:
        return False, "the range changes no files"
    return False, "none of its %d changed file(s) is under %s" % (
        len(paths), " ".join(SANITIZER_PATHS))


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


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = parser.add_subparsers(dest="command", required=True)
    cfg = sub.add_parser("config")
    cfg.add_argument("--workflow", default=WORKFLOW)
    cfg.add_argument("--source-root", default=".")
    sub.add_parser("affected")
    chk = sub.add_parser("check-inputs")
    chk.add_argument("--build-dir", required=True)
    chk.add_argument("--source-root", default=".")
    chk.add_argument("--workflow")
    args = parser.parse_args(argv)
    if args.command == "check-inputs":
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
        run, why = affected(paths)
        print(why)
        return 0 if run else 1
    try:
        with open(args.workflow, encoding="utf-8") as f:
            sys.stdout.write(as_bash(job_config(f.read(), args.source_root)))
    except (OSError, WorkflowError, ValueError) as e:
        print("sanitizer-gate-ci: %s: %s" % (args.workflow, e), file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
