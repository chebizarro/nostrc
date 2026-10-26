#!/usr/bin/env python3
"""Check that every symbol a GIR references is a dynamic export (Linux/ELF).

Run after building a GIR target, e.g.:
  scripts/check-gir-exports.py build/nostr-gobject/gir/GNostr-1.0.gir \
      build/nostr-gobject/libnostr-gobject-1.0.so

Typelib consumers resolve functions, methods, constructors and *_get_type by
name at runtime, so a hidden-by-default library must export all of them.
"""
import argparse
import json
import pathlib
import subprocess
import sys
import xml.etree.ElementTree as ET

CORE = "{http://www.gtk.org/introspection/core/1.0}"
C = "{http://www.gtk.org/introspection/c/1.0}"
GLIB = "{http://www.gtk.org/introspection/glib/1.0}"
CALLABLES = {CORE + tag for tag in ("function", "method", "constructor")}


def exports(library):
    output = subprocess.run(["nm", "-D", "--defined-only", str(library)],
                            check=True, text=True, capture_output=True).stdout
    return {line.split()[-1].split("@")[0] for line in output.splitlines() if line.split()}


def required(gir):
    symbols = set()
    for node in ET.parse(gir).iter():
        if node.tag in CALLABLES and node.get(C + "identifier"):
            symbols.add(node.get(C + "identifier"))
        get_type = node.get(GLIB + "get-type")
        if get_type and get_type != "intern":
            symbols.add(get_type)
    return symbols


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("gir", type=pathlib.Path)
    parser.add_argument("library", type=pathlib.Path, nargs="+",
                        help="DSO(s) that own the namespace's symbols")
    args = parser.parse_args()
    for path in [args.gir, *args.library]:
        if not path.is_file():
            parser.error(f"missing input: {path}")
    needed = required(args.gir)
    provided = set().union(*(exports(lib) for lib in args.library))
    missing = sorted(needed - provided)
    print(json.dumps({"gir": str(args.gir), "required": len(needed),
                      "missing": missing}, indent=2))
    return 1 if missing or not needed else 0


if __name__ == "__main__":
    sys.exit(main())
