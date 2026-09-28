#!/usr/bin/env python3
"""Check that the committed data/ui/*.ui files match their Blueprint sources.

usage: check_blueprint.py BLUEPRINT_COMPILER UI_DIR

Each UI_DIR/*.blp is compiled afresh and compared with the committed .ui as
canonical GtkBuilder XML: the generated header comment and whitespace-only text
are ignored, and boolean spellings of `translatable` are unified, because
blueprint-compiler 0.12 (Ubuntu 24.04) writes translatable="true" where newer
releases write "yes". Any element, attribute, property value or string change
still fails. Regenerate with the groundhog-update-ui build target.
"""

import difflib
from pathlib import Path
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET

BOOLEAN_ATTRIBUTES = {"translatable"}
TRUE_SPELLINGS = {"true", "yes", "1", "t", "y"}


def canonical(element, depth=0):
    attrib = dict(element.attrib)
    for name in BOOLEAN_ATTRIBUTES & attrib.keys():
        attrib[name] = "true" if attrib[name].lower() in TRUE_SPELLINGS else "false"
    attrs = "".join(f' {k}="{v}"' for k, v in sorted(attrib.items()))
    text = (element.text or "").strip()
    lines = ["  " * depth + f"<{element.tag}{attrs}>" + (repr(text) if text else "")]
    for child in element:
        lines.extend(canonical(child, depth + 1))
    return lines


def main():
    compiler, ui_dir = sys.argv[1], Path(sys.argv[2])
    sources = sorted(ui_dir.glob("*.blp"))
    if not sources:
        sys.exit(f"no Blueprint sources in {ui_dir}")
    stale = []
    with tempfile.TemporaryDirectory() as tmp:
        for blp in sources:
            fresh = Path(tmp) / f"{blp.stem}.ui"
            subprocess.run([compiler, "compile", "--output", str(fresh), str(blp)], check=True)
            committed = blp.with_suffix(".ui")
            if not committed.exists():
                stale.append(f"{committed.name}: missing (compiled from {blp.name})")
                continue
            want = canonical(ET.parse(fresh).getroot())
            have = canonical(ET.parse(committed).getroot())
            if want != have:
                diff = "\n".join(difflib.unified_diff(have, want, f"{committed.name} (committed)",
                                                      f"{committed.name} (from {blp.name})",
                                                      lineterm=""))
                stale.append(diff)
    if stale:
        print("\n".join(stale))
        sys.exit("Committed Groundhog .ui files are stale; build the groundhog-update-ui "
                 "target and commit the result")
    print(f"{len(sources)} committed Groundhog .ui files match their Blueprint sources")


if __name__ == "__main__":
    main()
