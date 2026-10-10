#!/usr/bin/env python3
"""Fail when a source with translatable strings is missing from POTFILES.

usage: nostrc-check-potfiles.py SOURCE_ROOT POTFILES GLOB [GLOB...]

Every file under SOURCE_ROOT matching a GLOB that calls _(), N_(), C_(),
NC_(), ngettext() or g_dngettext() (C or Blueprint) must be listed, and every
listed file must exist. Files that only mention the markers in comments are
still required; list them anyway so extraction is never silently partial.
"""
import pathlib
import re
import sys

MARKER = re.compile(r'(?<![A-Za-z0-9_])(?:N?C?_|ngettext|g_dngettext)\s*\(\s*"')


def main(root, potfiles, globs):
    root = pathlib.Path(root)
    listed = set()
    for line in pathlib.Path(potfiles).read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if line and not line.startswith("#"):
            listed.add(line)
    errors = [f"POTFILES lists missing file {f}" for f in sorted(listed)
              if not (root / f).is_file()]
    found = set()
    for pattern in globs:
        for path in root.glob(pattern):
            if path.is_file() and MARKER.search(path.read_text(encoding="utf-8", errors="replace")):
                found.add(path.relative_to(root).as_posix())
    for f in sorted(found - listed):
        errors.append(f"{f} has translatable strings but is not in POTFILES")
    if errors:
        print("\n".join(errors))
        return 1
    print(f"POTFILES complete: {len(found)} translatable sources listed")
    return 0


if __name__ == "__main__":
    if len(sys.argv) < 4:
        sys.exit(__doc__)
    sys.exit(main(sys.argv[1], sys.argv[2], sys.argv[3:]))
