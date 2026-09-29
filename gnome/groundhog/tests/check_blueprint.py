#!/usr/bin/env python3
"""Check that the committed data/ui/*.ui files match their Blueprint sources.

usage: check_blueprint.py BLUEPRINT_COMPILER UI_DIR

Each UI_DIR/*.blp is compiled afresh and compared with the committed .ui as
canonical GtkBuilder XML: the generated header comment and whitespace-only text
are ignored, and boolean spellings of `translatable` are unified, because
blueprint-compiler 0.12 (Ubuntu 24.04) writes translatable="true" where newer
releases write "yes". Likewise an expression lookup's object is a <constant>
child in 0.12 output and plain text (<lookup ...>GtkListItem</lookup>) in
newer releases; GtkBuilder reads both as the same object reference. An inline
list-item factory (BuilderListItemFactory { template ListItem { ... } })
carries its template as an XML string in its "bytes" property; that string is
parsed and compared by the same rules (newer releases also put the generated
header comment in it). Any element, attribute, property value or string
change still fails. Regenerate with the groundhog-update-ui build target.
"""

import difflib
from pathlib import Path
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET

BOOLEAN_ATTRIBUTES = {"translatable"}
TRUE_SPELLINGS = {"true", "yes", "1", "t", "y"}


def lookup_object_as_constant(element):
    """<lookup>NAME</lookup> -> <lookup><constant>NAME</constant></lookup>."""
    for child in element:
        lookup_object_as_constant(child)
    if element.tag == "lookup" and len(element) == 0 and (element.text or "").strip():
        constant = ET.SubElement(element, "constant")
        constant.text = element.text.strip()
        element.text = None


def embedded_template(element):
    """The parsed GtkBuilder document in an inline factory's "bytes" property."""
    text = (element.text or "").strip()
    if element.tag != "property" or element.get("name") != "bytes" or \
            not text.startswith("<?xml") and not text.startswith("<interface"):
        return None
    root = ET.fromstring(text.encode("utf-8"))
    lookup_object_as_constant(root)
    return root


def canonical(element, depth=0):
    embedded = embedded_template(element)
    if embedded is not None:
        attrs = "".join(f' {k}="{v}"' for k, v in sorted(element.attrib.items()))
        return ["  " * depth + f"<{element.tag}{attrs}>(template)"] + canonical(embedded,
                                                                              depth + 1)
    attrib = dict(element.attrib)
    for name in BOOLEAN_ATTRIBUTES & attrib.keys():
        attrib[name] = "true" if attrib[name].lower() in TRUE_SPELLINGS else "false"
    attrs = "".join(f' {k}="{v}"' for k, v in sorted(attrib.items()))
    # Whitespace-only text is formatting; any other text (e.g. a label value)
    # is compared exactly, including leading/trailing spaces.
    text = element.text or ""
    if not text.strip():
        text = ""
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
            fresh_root, committed_root = ET.parse(fresh).getroot(), ET.parse(committed).getroot()
            lookup_object_as_constant(fresh_root)
            lookup_object_as_constant(committed_root)
            want = canonical(fresh_root)
            have = canonical(committed_root)
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
