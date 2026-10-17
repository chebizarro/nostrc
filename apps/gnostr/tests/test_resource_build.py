#!/usr/bin/env python3
"""Real CMake/Blueprint/GResource regressions; never build into the checkout."""
import hashlib
import re
import shutil
import subprocess
import tempfile
import unittest
import xml.etree.ElementTree as ET
from pathlib import Path

APP = Path(__file__).resolve().parents[1]
RESOURCE = "/org/gnostr/ui/ui/gnostr-main-window.ui"


def run(*args, ok=True):
    result = subprocess.run(args, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    if ok and result.returncode:
        raise AssertionError(result.stdout.decode(errors="replace"))
    return result


def hashes(directory):
    return {str(p.relative_to(directory)): hashlib.sha256(p.read_bytes()).hexdigest()
            for p in directory.rglob("*") if p.is_file()}


def assert_spinner(test, content):
    root = ET.fromstring(content)
    test.assertIsNone(root.find(".//object[@class='AdwSpinner']"))
    spinners = root.findall(".//object[@class='GtkSpinner']")
    test.assertEqual(len(spinners), 1)
    properties = {p.attrib["name"]: p.text for p in spinners[0].findall("property")}
    test.assertEqual(properties["spinning"], "true")
    test.assertEqual(properties["height-request"], "64")
    test.assertEqual(properties["width-request"], "64")
    test.assertEqual(properties["halign"], "3")  # GTK_ALIGN_CENTER


class ResourceBuildTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="gnostr-resources-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.source = self.root / "source"
        self.build = self.root / "build"
        shutil.copytree(APP / "data", self.source / "data")
        shutil.copytree(APP / "cmake", self.source / "cmake")
        self.blp = self.source / "data/ui/gnostr-main-window.blp"
        self.ui = self.blp.with_suffix(".ui")
        # Exercise precisely the application's authored Blueprint input list.
        inputs = re.findall(r"\$\{CMAKE_CURRENT_SOURCE_DIR\}/data/(\S+\.blp)",
                            (APP / "CMakeLists.txt").read_text())
        self.assertTrue(inputs)
        arguments = "\n".join(f'  "${{CMAKE_CURRENT_SOURCE_DIR}}/data/{p}"' for p in inputs)
        (self.source / "CMakeLists.txt").write_text('''cmake_minimum_required(VERSION 3.22)
project(gnostr_resource_regression C)
find_package(PkgConfig REQUIRED)
pkg_check_modules(GIO REQUIRED IMPORTED_TARGET gio-2.0)
include(cmake/GnostrResources.cmake)
gnostr_compile_resources(resource_c "${CMAKE_CURRENT_SOURCE_DIR}/data"
''' + arguments + ''')
add_executable(resource-probe probe.c "${resource_c}")
target_link_libraries(resource-probe PRIVATE PkgConfig::GIO)
''')
        (self.source / "probe.c").write_text('''#include <gio/gio.h>
#include <stdio.h>
int main(int argc, char **argv) {
  if (argc != 2) return 2;
  GError *error = NULL;
  GBytes *bytes = g_resources_lookup_data(argv[1], 0, &error);
  if (!bytes) { g_printerr("%s\\n", error->message); g_error_free(error); return 1; }
  gsize size;
  const void *data = g_bytes_get_data(bytes, &size);
  int result = fwrite(data, 1, size, stdout) == size ? 0 : 1;
  g_bytes_unref(bytes);
  return result;
}
''')

    def configure(self, compiler=None):
        args = ["cmake", "-S", str(self.source), "-B", str(self.build), "-G", "Ninja"]
        if compiler is not None:
            args.append(f"-DBLUEPRINT_COMPILER={compiler}")
        run(*args)

    def compile(self, ok=True):
        return run("cmake", "--build", str(self.build), "--parallel", "2", ok=ok)

    def extract(self, path=RESOURCE):
        return run(str(self.build / "resource-probe"), path).stdout

    def test_generated_precedence_clean_source_and_incremental_inputs(self):
        # A stale, even malformed, tracked fallback must not win over Blueprint.
        self.ui.write_text("stale source UI - must not enter the generated resource")
        self.blp.write_text(self.blp.read_text().replace("Loading…", "Fresh Blueprint marker"))
        before = hashes(self.source)
        self.configure()
        self.compile()
        content = self.extract()
        self.assertIn(b"Fresh Blueprint marker", content)
        assert_spinner(self, content)
        self.assertEqual(content, (self.build / "data/ui/gnostr-main-window.ui").read_bytes())
        self.assertEqual(before, hashes(self.source))

        self.blp.write_text(self.blp.read_text().replace("Fresh Blueprint marker", "Incremental marker"))
        before = hashes(self.source)
        self.compile()
        self.assertIn(b"Incremental marker", self.extract())
        self.assertEqual(before, hashes(self.source))
        # A missing nested generated output is recreated, not taken from source.
        generated = self.build / "data/ui/dialogs/gnostr-profile-edit.ui"
        generated.unlink()
        self.compile()
        self.assertTrue(generated.is_file())
        self.assertEqual(before, hashes(self.source))

        # Non-Blueprint resources retain their exact path and incremental dependency.
        embed = self.source / "data/ui/widgets/gnostr-note-embed.ui"
        embed.write_text(embed.read_text() + "\n<!-- source-only marker -->\n")
        before = hashes(self.source)
        self.compile()
        self.assertEqual(self.extract("/org/gnostr/ui/ui/widgets/gnostr-note-embed.ui"), embed.read_bytes())
        self.assertEqual(before, hashes(self.source))
        self.assertEqual(self.extract("/org/gnostr/assets/assets/background.png"),
                         (self.source / "data/assets/background.png").read_bytes())
        self.assertEqual(self.extract("/org/gnostr/icons/scalable/actions/gnostr-bug-symbolic.svg"),
                         (self.source / "data/icons/hicolor/scalable/actions/gnostr-bug-symbolic.svg").read_bytes())

    def test_no_compiler_fallback_ignores_stale_generated_tree(self):
        self.blp.write_text(self.blp.read_text().replace("Loading…", "Old generated marker"))
        self.configure()
        self.compile()
        self.assertIn(b"Old generated marker", self.extract())
        self.configure("")
        before = hashes(self.source)
        self.compile()
        self.assertEqual(self.extract(), self.ui.read_bytes())
        assert_spinner(self, self.extract())
        self.assertEqual(before, hashes(self.source))

    def test_bad_blueprint_refuses_instead_of_using_valid_fallback(self):
        self.blp.write_text(self.blp.read_text().replace("Gtk.Spinner", "Adw.NoSuchSpinner"))
        self.configure()
        before = hashes(self.source)
        result = self.compile(ok=False)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn(b"NoSuchSpinner", result.stdout)
        self.assertFalse((self.build / "resource-probe").exists())
        self.assertEqual(before, hashes(self.source))

    def test_spinner_contract_rejects_inactive_and_new_adwaita_widget(self):
        content = self.ui.read_bytes()
        assert_spinner(self, content)
        for invalid in (content.replace(b"GtkSpinner", b"AdwSpinner"),
                        content.replace(b'name="spinning">true', b'name="spinning">false')):
            with self.assertRaises(AssertionError):
                assert_spinner(self, invalid)


if __name__ == "__main__":
    unittest.main()
