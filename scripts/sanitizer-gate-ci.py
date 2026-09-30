#!/usr/bin/env python3
"""The pre-push sanitizer stage's view of hosted CI (nostrc-3han).

usage: sanitizer-gate-ci.py config [--workflow FILE] [--source-root DIR]
       sanitizer-gate-ci.py affected < CHANGED_PATHS

config: read the `groundhog-sanitizers` job of groundhog-ci.yml and print, as
bash assignments, what the gate needs to run that job exactly:
  GH_SAN_TESTS          the job's GROUNDHOG_SANITIZER_TESTS
  GH_SAN_TARGETS        what its build step builds for them
  GH_SAN_CONFIGURE      its configure step's cmake arguments after -S/-B
  GH_SAN_ENV            NAME=VALUE for its test step's env and exports
                        (ASAN/UBSAN/LSAN options; $PWD becomes --source-root)
  GH_SAN_SKIP_PATTERN   the ERE its test step rejects in the ctest log
  GH_SAN_TEST_JOBS      its ctest --parallel (1 without one): ASAN tests
                        time out and race under more load than CI gives them
The gate reads these at run time, so CI and the gate cannot drift apart. When
a step changes in a way this reader does not understand (another command in
the configure step, a new ctest option, a different target mapping), it fails
instead of guessing: update the reader and scripts/linux-gate.sh with the job.

affected: exit 0 when one of the changed paths (one per line on stdin) is
under SANITIZER_PATHS, 1 when none is; print why either way.

No PyYAML: the macOS host that runs the pre-push hook has no yaml module, so
this reads the subset of YAML the workflows use (block mappings, sequences,
|/> block scalars, plain and quoted scalars).
"""

import argparse
import re
import shlex
import sys

WORKFLOW = ".github/workflows/groundhog-ci.yml"
JOB = "groundhog-sanitizers"
TESTS_VAR = "GROUNDHOG_SANITIZER_TESTS"

# What the sanitizer tests link or run, and the job itself. A push touching
# none of these cannot change the job's result.
SANITIZER_PATHS = (
    "gnome/groundhog/",
    "libnostr/",
    "nostr-gobject/",
    "libmarmot/",
    "marmot-gobject/",
    "libgo/",
    "nips/",
    "tests/",
    WORKFLOW,
)

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


def job_config(text, source_root):
    doc = parse_yaml(text)
    try:
        job = doc["jobs"][JOB]
    except (KeyError, TypeError):
        raise WorkflowError("no job %s" % JOB)
    tests = str((job.get("env") or {}).get(TESTS_VAR, "")).split()
    if not tests:
        raise WorkflowError("job %s has no %s" % (JOB, TESTS_VAR))
    if len(set(tests)) != len(tests):
        raise WorkflowError("%s lists a test twice" % TESTS_VAR)
    steps = job.get("steps") or []

    # Configure: one cmake command, whose arguments the gate passes as they are.
    configure = _commands(_step(steps, "cmake -S", "configure")["run"])
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
    build = _commands(_step(steps, "cmake --build", "build")["run"])
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
    env = []
    for name, value in (run_step.get("env") or {}).items():
        if "${{" in str(value):
            raise WorkflowError("test step env %s uses an expression: %s" % (name, value))
        env.append((name, str(value)))
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
    return {
        "GH_SAN_TESTS": tests,
        "GH_SAN_TARGETS": targets,
        "GH_SAN_CONFIGURE": configure_args,
        "GH_SAN_ENV": ["%s=%s" % pair for pair in env],
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

def affected(paths):
    """(True, why) when a path is under SANITIZER_PATHS, else (False, why)."""
    hits = {}
    for path in paths:
        for prefix in SANITIZER_PATHS:
            if path == prefix or (prefix.endswith("/") and path.startswith(prefix)):
                hits.setdefault(prefix, []).append(path)
    if hits:
        return True, "the range touches " + ", ".join(
            "%s (%d: %s%s)" % (p, len(v), v[0], ", ..." if len(v) > 1 else "")
            for p, v in sorted(hits.items()))
    if not paths:
        return False, "the range changes no files"
    return False, "none of its %d changed file(s) is under %s" % (
        len(paths), " ".join(SANITIZER_PATHS))


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = parser.add_subparsers(dest="command", required=True)
    cfg = sub.add_parser("config")
    cfg.add_argument("--workflow", default=WORKFLOW)
    cfg.add_argument("--source-root", default=".")
    sub.add_parser("affected")
    args = parser.parse_args(argv)
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
