#!/usr/bin/env python3
"""Focused regressions for the package boundary guard (no product build)."""
import importlib.util
import json
from pathlib import Path
import subprocess
import tempfile
import unittest

SCRIPT = Path(__file__).with_name("check-package-boundaries.py")
spec = importlib.util.spec_from_file_location("boundaries", SCRIPT)
b = importlib.util.module_from_spec(spec)
spec.loader.exec_module(b)


class Boundaries(unittest.TestCase):
    def test_private_includes_and_test_exception(self):
        sources = {
            "nips/nip44/src/a.c": '#include "../include/a.h"\n#include <libvault/src/private.h>\n',
            "nips/nip44/tests/a.c": '#include "../src/a.c"\n#include "../../nip46/src/a.c"\n',
            "tests/a.c": '#include "../libnostr/src/private.h"\n',
        }
        findings = b.source_findings(sources, b.source_symbols(sources))
        self.assertEqual(sum(findings.values()), 4)
        self.assertNotIn("include|nips/nip44/tests/a.c|../src/a.c", findings)

    def test_extern_symbol_ownership_and_comments(self):
        sources = {path: b.uncomment(text) for path, text in {
            "nips/nip44/src/a.c": 'int helper(const int *p) { return *p; }\nstatic int local(void) { return 1; }\n',
            "libvault/src/a.c": 'extern int\n helper(const int *p);\nextern int local(void);\n/* extern int helper(int); */\n',
            "libvault/tests/stub.c": 'int helper(const int *p) { return 0; }\n',
        }.items()}
        self.assertEqual(list(b.source_findings(sources, b.source_symbols(sources))),
                         ["extern|libvault/src/a.c|helper -> nips/nip44"])

    def test_layer_edges_variables_aliases_and_forward_targets(self):
        files = {
            "libnostr/CMakeLists.txt": '''
                add_library(nostr a.c)
                set(deps vault)
                target_link_libraries(nostr PRIVATE $<BUILD_INTERFACE:${deps}> nostrc::porthome)
            ''',
            "libporthome/CMakeLists.txt": '''
                add_library(porthome a.c)
                add_library(nostrc::porthome ALIAS porthome)
                target_link_libraries(porthome PRIVATE nostr)
            ''',
            "libvault/CMakeLists.txt": "add_library(vault a.c)",
        }
        findings = b.link_findings(files)
        self.assertEqual(sum(findings.values()), 2)
        self.assertTrue(any("-> vault (libvault:5)" in key for key in findings))
        self.assertTrue(any("-> nostrc::porthome (libporthome:4)" in key for key in findings))

    def test_cli_injection_and_allowlist_ratchet(self):
        with tempfile.TemporaryDirectory(prefix="r0-boundary-test-") as directory:
            root = Path(directory)
            def git(*args):
                return subprocess.run(["git", "-C", directory, *args], check=True,
                                      capture_output=True, text=True)
            def run(*args):
                return subprocess.run(["python3", str(SCRIPT), "--root", directory, *args],
                                      capture_output=True, text=True)
            git("init", "-q")
            git("config", "user.email", "boundary-test@example.invalid")
            git("config", "user.name", "Boundary test")
            (root / "scripts").mkdir()
            (root / "libnostr/src").mkdir(parents=True)
            source = root / "libnostr/src/a.c"
            source.write_text('#include "../private.h"\n')
            allowlist = root / b.ALLOWLIST
            original = "include|libnostr/src/a.c|../private.h\n"
            allowlist.write_text(original)
            git("add", ".")
            git("commit", "-qm", "Initial baseline")
            self.assertEqual(run().returncode, 0)
            # A new occurrence of the very same violation is not grandfathered.
            source.write_text(source.read_text() * 2)
            self.assertIn("new violation", run().stderr)
            allowlist.write_text(original * 2)
            self.assertIn("allowlist grew", run().stderr)
            # A same-size replacement is also forbidden.
            source.write_text('#include "../other.h"\n')
            allowlist.write_text("include|libnostr/src/a.c|../other.h\n")
            self.assertIn("allowlist grew", run().stderr)
            # Shrinking after removing a violation is allowed.
            source.write_text("int value;\n")
            allowlist.write_text("")
            self.assertEqual(run().returncode, 0)
            # A missing requested object inventory fails, never silently skips.
            self.assertNotEqual(run("--build-dir", directory).returncode, 0)
            # Untracked new source is scanned too.
            (root / "libnostr/src/injected.c").write_text('#include "../../libvault/src/private.h"\n')
            self.assertIn("injected.c", run().stderr)

    def test_nm_sibling_exports(self):
        with tempfile.TemporaryDirectory(prefix="r0-boundary-nm-") as directory:
            root = Path(directory)
            obj = root / "libvault/CMakeFiles/kdf.dir/src/core/kdf.c.o"
            obj.parent.mkdir(parents=True)
            source = root / "nips/nip44/src/kdf.c"
            source.parent.mkdir(parents=True)
            # Macro-generated names exercise the real nm index, not the source regex.
            source.write_text("#define NAME generated_kdf\nint NAME(void) { return 0; }\n")
            command = ["cc", "-c", str(source), "-o", str(obj)]
            subprocess.run(command, check=True)
            (root / "compile_commands.json").write_text(json.dumps([{
                "directory": str(root), "file": str(source), "arguments": command,
            }]))
            symbols = b.source_symbols({})
            b.object_symbols(root, symbols, root)
            findings = b.source_findings({"libvault/src/use.c": "extern int generated_kdf(void);\n"}, symbols)
            self.assertEqual(list(findings), ["extern|libvault/src/use.c|generated_kdf -> nips/nip44"])


if __name__ == "__main__":
    unittest.main()
