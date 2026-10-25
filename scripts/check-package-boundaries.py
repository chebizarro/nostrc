#!/usr/bin/env python3
"""Enforce shared-vault-composition.md §9.2/§9.7 without configuring a build.

Source definitions provide the symbol index in static CI. --build-dir supplements
it with nm exports from CMake objects (required to exist if requested). This
catches generated/macro-defined symbols too. Git submodules are not traversed;
tracked third_party files and Layer-0 target dependencies are checked. CMake checks
cover literal targets, aliases and set() lists, including conditional branches;
this is a source guard, not a replacement for the configured linker graph.

The allowlist contains one rule|path|detail line per occurrence (no line numbers,
so moving code cannot grow the budget). Entries, not just the total count, must
be a subset of each available historical baseline. There is no update mode.
"""
import argparse
from collections import Counter, defaultdict
import os
import json
from pathlib import Path, PurePosixPath
import re
import shlex
import subprocess
import sys

ALLOWLIST = "scripts/package-boundaries.allowlist"
LAYERS = {
    "libgo": 0, "libjson": 0, "third_party": 0,
    "libnostr": 1, "libblossom": 3, "libmarmot": 3,
    "libporthome": 4, "libvault": 5, "libhanami": 5,
    "nostr-gobject": 6, "marmot-gobject": 6, "bindings/rust": 6,
}
SOURCE_SUFFIXES = {".c", ".h", ".cc", ".cpp", ".hpp", ".m", ".mm"}


def package(path):
    parts = PurePosixPath(path).parts
    if not parts:
        return ""
    if parts[0] in {"nips", "gnome", "apps", "tools"} and len(parts) > 1:
        return "/".join(parts[:2])
    if parts[:2] == ("bindings", "rust"):
        return "bindings/rust"
    return parts[0]


def layer(owner):
    if owner.startswith("nips/"):
        return 2
    if owner.startswith(("gnome/", "apps/", "tools/")):
        return 7
    return LAYERS.get(owner)


def is_test(path):
    return "tests" in PurePosixPath(path).parts


def uncomment(text):
    # Preserve strings and newlines; fake directives/definitions in comments
    # must not become either findings or symbol owners.
    return re.sub(r'"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|/\*.*?\*/|//[^\n]*',
                  lambda m: re.sub(r"[^\n]", " ", m[0])
                  if m[0].startswith(("/*", "//")) else m[0], text, flags=re.S)


# Deliberately recognizes declarations/definitions, not call expressions.
FUNCTION = re.compile(
    r"(?m)^[ \t]*(?P<type>(?:[A-Za-z_]\w*[ \t\n*]+)+)"
    r"(?P<name>[A-Za-z_]\w*)\s*\([^;{}]*?\)\s*(?P<end>[;{])")


def source_findings(sources, symbols):
    findings = Counter()
    for path, text in sources.items():
        owner = package(path)
        for match in re.finditer(r'^\s*#\s*include\s*[<"]([^>"\n]+)[>"]', text, re.M):
            include = match[1]
            if "../" not in include and "/src/" not in "/" + include:
                continue
            # Only a package's own tests may reach its own private sources.
            resolved = os.path.normpath(str(PurePosixPath(path).parent / include))
            if is_test(path) and package(resolved) == owner:
                continue
            findings[f"include|{path}|{include}"] += 1
        for match in FUNCTION.finditer(text):
            if match["end"] != ";" or "extern" not in match["type"].split():
                continue
            if owner in symbols[match["name"]]:
                continue
            siblings = sorted(symbols[match["name"]])
            if siblings:
                findings[f"extern|{path}|{match['name']} -> {','.join(siblings)}"] += 1
    return findings


def source_symbols(sources):
    symbols = defaultdict(set)
    for path, text in sources.items():
        if is_test(path) or PurePosixPath(path).suffix not in {".c", ".cc", ".cpp", ".m", ".mm"}:
            continue
        for match in FUNCTION.finditer(text):
            if match["end"] == "{" and "static" not in match["type"].split():
                symbols[match["name"]].add(package(path))
    return symbols


def object_symbols(build_dir, symbols, root=None):
    # A consumer can compile a sibling's source directly (porthome does this
    # during R0). Attribute that object's exports to the source package, not
    # to the consumer's build directory.
    owners = {}
    if root is not None:
        root = root.resolve()
    commands = build_dir / "compile_commands.json"
    if root is not None and commands.is_file():
        for entry in json.loads(commands.read_text()):
            source = Path(entry["file"])
            if not source.is_absolute():
                source = Path(entry["directory"]) / source
            try:
                source = source.resolve().relative_to(root).as_posix()
            except ValueError:
                continue
            args = entry.get("arguments") or shlex.split(entry["command"])
            output = entry.get("output")
            if not output and "-o" in args:
                output = args[args.index("-o") + 1]
            if output:
                obj = Path(entry["directory"]) / output
                owners[obj.resolve()] = package(source)
    objects = sorted(build_dir.rglob("*.o")) + sorted(build_dir.rglob("*.obj"))
    checked = 0
    for obj in objects:
        relative = obj.relative_to(build_dir).as_posix()
        if "CMakeFiles/" not in relative or is_test(relative) or re.search(r"CMakeFiles/test[^/]*\.dir/", relative):
            continue
        owner = owners.get(obj.resolve(), package(relative.split("CMakeFiles/", 1)[0].rstrip("/")))
        if not owner:
            continue
        result = subprocess.run(["nm", "-g", str(obj)], check=True,
                                capture_output=True, text=True)
        checked += 1
        for line in result.stdout.splitlines():
            fields = line.split()
            if len(fields) >= 3 and fields[-2] in {"T", "D", "B", "R", "S", "W"}:
                name = fields[-1]
                if sys.platform == "darwin" and name.startswith("_"):
                    name = name[1:]
                symbols[name].add(owner)
    if not checked:
        raise ValueError(f"no package CMake objects to inspect in {build_dir}")
    print(f"package boundaries: nm checked {checked} objects", file=sys.stderr)


def cmake_commands(text):
    text = re.sub(r"#\[\[.*?\]\]", "", text, flags=re.S)
    text = re.sub(r'"(?:\\.|[^"\\])*"|#[^\n]*',
                  lambda m: "" if m[0].startswith("#") else m[0], text)
    pattern = re.compile(r"\b(set|add_library|add_executable|target_link_libraries)\s*\(", re.I)
    pos = 0
    while match := pattern.search(text, pos):
        start = pos = match.end()
        depth = 1
        quoted = False
        while pos < len(text) and depth:
            char = text[pos]
            if char == '"' and (pos == 0 or text[pos - 1] != "\\"):
                quoted = not quoted
            if not quoted:
                depth += (char == "(") - (char == ")")
            pos += 1
        if depth:
            raise ValueError("unterminated CMake command")
        lexer = shlex.shlex(text[start:pos - 1], posix=True)
        lexer.whitespace_split = True
        lexer.commenters = ""
        yield match[1].lower(), list(lexer)


def expand(tokens, variables):
    result = []
    for token in tokens:
        for _ in range(10):
            value = re.sub(r"\$\{([^}]+)\}", lambda m: variables.get(m[1], m[0]), token)
            if value == token:
                break
            token = value
        result.extend(token.split(";"))
    return result


def link_findings(cmake_files):
    targets, aliases, links = {}, {}, []
    # Parent directory variables are inherited; sibling local variables are not.
    scopes = {}
    for path, text in sorted(cmake_files.items(), key=lambda item: (item[0].count("/"), item[0])):
        directory = str(PurePosixPath(path).parent)
        parent = PurePosixPath(directory).parent
        while str(parent) not in scopes and str(parent) != ".":
            parent = parent.parent
        variables = dict(scopes.get(str(parent), {}))
        for command, raw in cmake_commands(text):
            args = expand(raw, variables)
            if not args:
                continue
            if command == "set":
                variables[args[0]] = ";".join(args[1:])
            elif command in {"add_library", "add_executable"}:
                if "IMPORTED" in args:
                    continue
                if len(args) >= 3 and args[1] == "ALIAS":
                    aliases[args[0]] = args[2]
                else:
                    targets[args[0]] = package(path)
            elif command == "target_link_libraries":
                links.append((path, args[0], args[1:]))
        scopes[directory] = variables

    def resolve(target):
        seen = set()
        while target in aliases and target not in seen:
            seen.add(target)
            target = aliases[target]
        return targets.get(target)

    findings = Counter()
    for path, target, dependencies in links:
        owner = resolve(target)
        low = layer(owner or "")
        for dependency in dependencies:
            # Remove generator-expression operators, retaining named targets
            # (including namespaced aliases) in conditional link branches.
            dependency = re.sub(r"\$<[^<>:]+:", " ", dependency)
            for candidate in re.findall(r"[A-Za-z_][\w:+.-]*", dependency):
                other = resolve(candidate)
                high = layer(other or "")
                if low is not None and high is not None and high > low:
                    findings[f"link|{path}|{target} ({owner}:{low}) -> {candidate} ({other}:{high})"] += 1
    return findings


def parse_allowlist(text):
    return Counter(line for line in text.splitlines() if line and not line.startswith("#"))


def git(root, *args):
    return subprocess.run(["git", "-C", str(root), *args], capture_output=True, text=True)


def historical_baselines(root, base):
    # HEAD protects uncommitted edits; HEAD^ protects CI running on the commit.
    # The merge base protects against a grow-then-shrink series on a PR branch.
    refs = ["HEAD", "HEAD^"]
    if base:
        result = git(root, "rev-parse", "--verify", base)
        if result.returncode:
            raise ValueError(f"cannot resolve baseline ref {base}")
        refs.append(base)
    else:
        for upstream in ("origin/master", "origin/main", "master", "main"):
            result = git(root, "merge-base", "HEAD", upstream)
            if result.returncode == 0:
                refs.append(result.stdout.strip())
                break
    for ref in dict.fromkeys(refs):
        result = git(root, "show", f"{ref}:{ALLOWLIST}")
        if result.returncode == 0:
            yield ref, parse_allowlist(result.stdout)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parent.parent)
    parser.add_argument("--build-dir", type=Path, help="also compare externs with nm exports of built sibling objects")
    parser.add_argument("--base", help="additional git ref against which allowlist entries must only shrink")
    parser.add_argument("--list-violations", action="store_true", help="diagnostic only; does not enforce the allowlist")
    args = parser.parse_args()
    root = args.root.resolve()
    # Include new untracked source files, so local injections cannot evade the guard.
    result = git(root, "ls-files", "-z", "--cached", "--others", "--exclude-standard")
    result.check_returncode()
    sources, cmake_files = {}, {}
    for path in sorted(set(result.stdout.rstrip("\0").split("\0"))):
        file = root / path
        if not file.is_file():
            continue
        if file.suffix in SOURCE_SUFFIXES:
            sources[path] = uncomment(file.read_text(errors="replace"))
        elif file.name == "CMakeLists.txt" or file.suffix == ".cmake":
            cmake_files[path] = file.read_text()
    symbols = source_symbols(sources)
    if args.build_dir:
        object_symbols(args.build_dir.resolve(), symbols, root)
    findings = source_findings(sources, symbols) + link_findings(cmake_files)
    if args.list_violations:
        for finding in sorted(findings.elements()):
            print(finding)
        return 0
    allowed = parse_allowlist((root / ALLOWLIST).read_text())
    errors = []
    for ref, previous in historical_baselines(root, args.base):
        for entry, count in sorted((allowed - previous).items()):
            errors.append(f"allowlist grew against {ref}: {count} x {entry}")
    for entry, count in sorted((findings - allowed).items()):
        errors.append(f"new violation: {count} x {entry}")
    if errors:
        print("\n".join(errors), file=sys.stderr)
        return 1
    print(f"package boundaries: PASS ({sum(findings.values())} baselined occurrences; "
          f"{sum(allowed.values())} allowed; source" + (" + nm" if args.build_dir else "") + ")")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        print(f"package boundaries: ERROR: {error}", file=sys.stderr)
        sys.exit(1)
