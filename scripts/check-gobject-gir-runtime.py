#!/usr/bin/env python3
"""Exercise PyGObject in an executable-contract-linked Python host (ELF).

This is NOT proof of standalone /usr/bin/python3 loading the executable-owned
runtime. Prerequisites: built Groundhog GNostr GIR, C compiler, python3-embed
pkg-config/development files and PyGObject for the embedded Python version.
"""
import argparse
import os
import pathlib
import shlex
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("build", type=pathlib.Path)
    parser.add_argument("--cc", default=os.environ.get("CC", "cc"))
    parser.add_argument("--configuration", default="", help="CMake multi-config selection")
    args = parser.parse_args()
    build = args.build.resolve()
    source = pathlib.Path(__file__).resolve().parents[1]
    runtime = build / "nostr-gobject" / args.configuration
    contract = runtime / "nostr-gobject-build-tree.pc"
    typelib = build / "nostr-gobject/gir/GNostr-1.0.typelib"
    if not contract.is_file() or not typelib.is_file():
        parser.error("build nostr-gobject-gir and nostr-gobject-build-tree-pc first")
    cflags = shlex.split(subprocess.check_output(
        ["pkg-config", "--cflags", "python3-embed"], text=True))
    libs = shlex.split(subprocess.check_output(
        ["pkg-config", "--libs", str(contract), "python3-embed"], text=True))
    with tempfile.TemporaryDirectory(prefix="gnostr-pygi-") as temp:
        host = pathlib.Path(temp) / "python-host"
        subprocess.run([*shlex.split(args.cc), *cflags,
                        str(source / "nostr-gobject/tests/gir_python_host.c"),
                        "-Wl,--export-dynamic", "-Wl,--no-undefined", *libs,
                        "-o", str(host)], check=True)
        def definitions(binary):
            output = subprocess.check_output(["nm", "--defined-only", str(binary)], text=True)
            return [line.split()[-1] for line in output.splitlines() if line.split()]

        assert definitions(host).count("nostr_named_mutex_pool") == 1
        for dso in (runtime / "libnostr-gobject-1.0.so",
                    build / "libjson" / args.configuration / "libnostr-json.so"):
            assert "nostr_named_mutex_pool" not in definitions(dso), dso
        env = os.environ.copy()
        env["GI_TYPELIB_PATH"] = str(typelib.parent)
        env["G_DEBUG"] = "fatal-warnings"
        subprocess.run([str(host), "-c", """
import gi
from pathlib import Path

gi.require_version('GNostr', '1.0')
from gi.repository import GNostr
one = GNostr.Event.new()
two = GNostr.Event.new()
one.set_content('contract-linked PyGObject')
assert one.get_content() == 'contract-linked PyGObject'
assert one.__gtype__ == two.__gtype__
# Core belongs to the embedding executable, not a second GObject DSO copy.
maps = Path('/proc/self/maps').read_text().splitlines()
gobject_libraries = {line.split()[-1] for line in maps if 'libnostr-gobject-1.0.so' in line}
assert len(gobject_libraries) == 1, gobject_libraries
print('GNostr PyGObject Event roundtrip passed; one GObject DSO')
"""], env=env, check=True)


if __name__ == "__main__":
    main()
