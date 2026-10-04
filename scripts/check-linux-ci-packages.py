#!/usr/bin/env python3
"""The local Linux image must install what hosted Linux CI installs (nostrc-y9xg).

usage: check-linux-ci-packages.py [--root DIR] [--self-test]

scripts/linux-ci.Dockerfile is the image of the pre-push gate's Linux stage
(scripts/linux-gate.sh) and of scripts/groundhog-linux-ci.sh. It mirrors the
apt packages of the workflows in MIRRORED, which build the targets that stage
builds (every default target plus Groundhog). This check fails when one of
those workflows installs a package the image lacks, so the two cannot drift
apart unnoticed: a dependency added to CI but not to the image would make the
local build configure differently (or not at all) and stop predicting CI.

A package that CI needs only for packaging or reporting, not for building, is
listed in NOT_BUILD_DEPS with its reason.
"""

import argparse
from pathlib import Path
import re
import sys
import tempfile

DOCKERFILE = "scripts/linux-ci.Dockerfile"
MIRRORED = (
    ".github/workflows/groundhog-ci.yml",
    ".github/workflows/gnostr-appimage.yml",
    ".github/workflows/signet-ci.yml",
    ".github/workflows/nostrc-ci.yml",
)
NOT_BUILD_DEPS = {
    "wget": "gnostr-appimage downloads appimagetool",
    "file": "gnostr-appimage inspects the bundle",
    "patchelf": "gnostr-appimage rewrites the bundle's RPATHs",
    "python3-pip": "gnostr-appimage installs blueprint-compiler 0.22 for its .ui "
                   "regeneration, which the gate disables (BLUEPRINT_COMPILER=OFF)",
}

# Development headers alone do not provide decodebin, AAC/MP4 demux/decoder,
# Opus/Ogg codec or the audio sinks. Both the gate image and hosted Groundhog
# job must install the runtime plugins; the image also runs gst-inspect.
REQUIRED_GSTREAMER_RUNTIME = {
    "gstreamer1.0-tools",
    "gstreamer1.0-plugins-base",
    "gstreamer1.0-plugins-good",
    "gstreamer1.0-plugins-bad",
    "gstreamer1.0-libav",
}

PACKAGE_RE = re.compile(r"^[a-z0-9][a-z0-9.+-]+$")


def apt_packages(text):
    """Package names from every `apt-get install` command in shell-ish text,
    following backslash continuations and skipping comment lines."""
    packages = set()
    lines = text.splitlines()
    i = 0
    while i < len(lines):
        m = re.search(r"\bapt-get\s+install\b(.*)", lines[i])
        i += 1
        if not m:
            continue
        command, continued = m.group(1), m.group(1).rstrip().endswith("\\")
        while continued and i < len(lines):
            line = lines[i]
            i += 1
            if line.lstrip().startswith("#"):
                continue  # Docker drops comment lines inside a continued RUN
            command = command.rstrip()[:-1] + " " + line
            continued = line.rstrip().endswith("\\")
        for token in command.split():
            if token in ("&&", "||", ";", "|"):
                break
            if not token.startswith("-") and PACKAGE_RE.match(token):
                packages.add(token)
    return packages


def check(root):
    root = Path(root)
    image = apt_packages((root / DOCKERFILE).read_text())
    if not image:
        return [f"{DOCKERFILE}: no apt-get install packages found"]
    problems = []
    for package in sorted(REQUIRED_GSTREAMER_RUNTIME - image):
        problems.append(f"{DOCKERFILE}: missing required voice runtime package {package}")
    for workflow in MIRRORED:
        path = root / workflow
        if not path.is_file():
            problems.append(f"{workflow}: mirrored workflow is missing (update MIRRORED)")
            continue
        workflow_packages = apt_packages(path.read_text())
        if workflow == ".github/workflows/groundhog-ci.yml":
            for package in sorted(REQUIRED_GSTREAMER_RUNTIME - workflow_packages):
                problems.append(f"{workflow}: missing required voice runtime package {package}")
        missing = workflow_packages - image - set(NOT_BUILD_DEPS)
        for package in sorted(missing):
            problems.append(f"{workflow}: installs {package}, which {DOCKERFILE} does not")
    return problems


def self_test():
    runtime = " ".join(sorted(REQUIRED_GSTREAMER_RUNTIME))
    dockerfile = ("RUN apt-get update && apt-get install -y --no-install-recommends \\\n"
                  "    # a comment line inside the command\n"
                  "    build-essential libfoo-dev \\\n"
                  f"    libbar2.0-dev {runtime} && rm -rf /var/lib/apt/lists/*\n")
    good = ("      run: |\n"
            "        sudo apt-get install -y --no-install-recommends \\\n"
            "          build-essential libfoo-dev \\\n"
            f"          libbar2.0-dev {runtime} wget\n"
            "        sudo apt-get install -y libfoo-dev\n")
    bad = good + "        sudo apt-get install -y libnew-dev\n"
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        (root / "scripts").mkdir()
        (root / ".github/workflows").mkdir(parents=True)
        (root / DOCKERFILE).write_text(dockerfile)
        for workflow in MIRRORED:
            (root / workflow).write_text(good)
        assert check(root) == [], check(root)
        (root / MIRRORED[0]).write_text(bad)
        found = check(root)
        assert found == [f"{MIRRORED[0]}: installs libnew-dev, which {DOCKERFILE} does not"], found
        (root / MIRRORED[0]).write_text(good.replace("gstreamer1.0-libav", ""))
        found = check(root)
        assert found == [f"{MIRRORED[0]}: missing required voice runtime package gstreamer1.0-libav"], found
        (root / MIRRORED[0]).write_text(good)
        (root / MIRRORED[1]).unlink()
        assert any("missing" in p for p in check(root))
    print("check-linux-ci-packages self-test passed")


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--root", default=Path(__file__).resolve().parent.parent)
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        self_test()
    problems = check(args.root)
    for problem in problems:
        print(problem, file=sys.stderr)
    if problems:
        print(f"Add the packages to {DOCKERFILE} (or, if CI needs one only for packaging, "
              "to NOT_BUILD_DEPS with the reason)", file=sys.stderr)
        return 1
    print(f"{DOCKERFILE} installs every build package of: {', '.join(MIRRORED)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
