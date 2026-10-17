#!/usr/bin/env python3
"""Exercise each component's resource inputs through the shared real-tool seam."""
import argparse
import hashlib
import re
import shutil
import subprocess
import tempfile
import unittest
import xml.etree.ElementTree as ET
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(add_help=False)
parser.add_argument("--component", choices=("gnostr", "nostr-gtk"), required=True)
parser.add_argument("--blueprint-compiler", default=shutil.which("blueprint-compiler"))
options, unittest_args = parser.parse_known_args()
IS_APP = options.component == "gnostr"
COMPONENT = ROOT / ("apps/gnostr" if IS_APP else "nostr-gtk")
MANIFEST = "resources.gresource.xml" if IS_APP else "nostr-gtk.gresource.xml"
PRIMARY = "ui/gnostr-main-window.ui" if IS_APP else "ui/gnostr-profile-pane.ui"
RESOURCE = ("/org/gnostr/ui/" if IS_APP else "/org/nostr/gtk/") + PRIMARY
MARKER = "Loading…" if IS_APP else "User profile"


def run(*args, ok=True):
    result = subprocess.run(args, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    if ok and result.returncode:
        raise AssertionError(result.stdout.decode(errors="replace"))
    return result


def hashes(directory):
    return {str(p.relative_to(directory)): hashlib.sha256(p.read_bytes()).hexdigest()
            for p in directory.rglob("*") if p.is_file()}


def assert_primary_contract(test, content):
    root = ET.fromstring(content)
    if not IS_APP:
        template = root.find("template")
        test.assertIsNotNone(template)
        test.assertEqual(template.attrib["class"], "NostrGtkProfilePane")
        test.assertEqual(template.attrib["parent"], "GtkWidget")
        return
    test.assertIsNone(root.find(".//object[@class='AdwSpinner']"))
    spinners = root.findall(".//object[@class='GtkSpinner']")
    test.assertEqual(len(spinners), 1)
    properties = {p.attrib["name"]: p.text for p in spinners[0].findall("property")}
    test.assertEqual(properties["spinning"], "true")
    test.assertEqual(properties["height-request"], "64")
    test.assertEqual(properties["width-request"], "64")
    test.assertEqual(properties["halign"], "3")  # GTK_ALIGN_CENTER


def normalized_xml(content):
    root = ET.fromstring(content)
    for node in root.iter():
        if node.text and node.text.isspace():
            node.text = None
        node.tail = None
    return ET.tostring(root)


class ResourceBuildTests(unittest.TestCase):
    def setUp(self):
        self.assertTrue(options.blueprint_compiler, "real blueprint-compiler required")
        self.temp = tempfile.TemporaryDirectory(prefix="nostrc-resources-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.source = self.root / "source"
        self.build = self.root / "build"
        shutil.copytree(COMPONENT / "data", self.source / "data")
        self.blp = (self.source / "data" / PRIMARY).with_suffix(".blp")
        self.ui = self.blp.with_suffix(".ui")
        shutil.copyfile(ROOT / "cmake/NostrcGtkResources.cmake", self.source / "NostrcGtkResources.cmake")
        # Exercise precisely each component's authored Blueprint list, not a copy.
        inputs = re.findall(r"\$\{CMAKE_CURRENT_SOURCE_DIR\}/data/(\S+\.blp)",
                            (COMPONENT / "CMakeLists.txt").read_text())
        self.assertTrue(inputs)
        arguments = "\n".join(f'  "${{CMAKE_CURRENT_SOURCE_DIR}}/data/{p}"' for p in inputs)
        (self.source / "CMakeLists.txt").write_text('''cmake_minimum_required(VERSION 3.22)
project(nostrc_resource_regression C)
find_package(PkgConfig REQUIRED)
pkg_check_modules(GIO REQUIRED IMPORTED_TARGET gio-2.0)
include(NostrcGtkResources.cmake)
nostrc_compile_blueprint_resources(resource_c
  "${CMAKE_CURRENT_SOURCE_DIR}/data/''' + MANIFEST + '''"
  "${CMAKE_CURRENT_BINARY_DIR}/resources.c" blueprint_outputs
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
        if compiler is None:
            compiler = options.blueprint_compiler
        run("cmake", "-S", str(self.source), "-B", str(self.build), "-G", "Ninja",
            f"-DBLUEPRINT_COMPILER={compiler}")

    def compile(self, ok=True):
        return run("cmake", "--build", str(self.build), "--parallel", "2", ok=ok)

    def extract(self, path=RESOURCE):
        return run(str(self.build / "resource-probe"), path).stdout

    def check_all_resources(self):
        for resource in ET.parse(self.source / "data" / MANIFEST).getroot():
            for entry in resource:
                relative = entry.text
                name = resource.attrib["prefix"] + "/" + entry.attrib.get("alias", relative)
                expected = self.build / "data" / relative
                if not expected.is_file():
                    expected = self.source / "data" / relative
                actual = self.extract(name)
                if entry.attrib.get("preprocess") == "xml-stripblanks":
                    # GLib's documented preprocessing changes whitespace, not UI.
                    self.assertEqual(normalized_xml(actual), normalized_xml(expected.read_bytes()))
                else:
                    self.assertEqual(actual, expected.read_bytes(), name)

    def test_generated_precedence_clean_source_and_incremental_inputs(self):
        self.ui.write_text("stale source UI - must not enter the generated resource")
        self.blp.write_text(self.blp.read_text().replace(MARKER, "Fresh Blueprint marker"))
        before = hashes(self.source)
        self.configure()
        self.compile()
        content = self.extract()
        self.assertIn(b"Fresh Blueprint marker", content)
        assert_primary_contract(self, content)
        self.assertEqual(content, (self.build / "data" / PRIMARY).read_bytes())
        self.check_all_resources()
        self.assertEqual(before, hashes(self.source))

        self.blp.write_text(self.blp.read_text().replace("Fresh Blueprint marker", "Incremental marker"))
        before = hashes(self.source)
        self.compile()
        self.assertIn(b"Incremental marker", self.extract())
        self.assertEqual(before, hashes(self.source))
        nested = ("ui/dialogs/gnostr-profile-edit.ui" if IS_APP
                  else "ui/widgets/gnostr-timeline-view.ui")
        generated = self.build / "data" / nested
        generated.unlink()
        self.compile()
        self.assertTrue(generated.is_file())
        self.assertEqual(before, hashes(self.source))

        source_only = ("ui/widgets/gnostr-note-embed.ui" if IS_APP else "gnostr-thread-view.ui")
        ui = self.source / "data" / source_only
        ui.write_text(ui.read_text() + "\n<!-- source-only marker -->\n")
        before = hashes(self.source)
        self.compile()
        prefix = "/org/gnostr/ui/" if IS_APP else "/org/nostr/gtk/"
        self.assertEqual(self.extract(prefix + source_only), ui.read_bytes())
        self.check_all_resources()
        self.assertEqual(before, hashes(self.source))

    def test_no_compiler_fallback_ignores_stale_generated_tree(self):
        self.blp.write_text(self.blp.read_text().replace(MARKER, "Old generated marker"))
        self.configure()
        self.compile()
        self.assertIn(b"Old generated marker", self.extract())
        self.configure("")
        before = hashes(self.source)
        self.compile()
        self.assertEqual(self.extract(), self.ui.read_bytes())
        assert_primary_contract(self, self.extract())
        self.assertEqual(before, hashes(self.source))

    def test_bad_blueprint_refuses_instead_of_using_valid_fallback(self):
        original = self.blp.read_text()
        invalid = (original.replace("Gtk.Spinner", "Adw.NoSuchSpinner") if IS_APP
                   else original.replace("NostrGtkProfilePane: Widget", "NostrGtkProfilePane: Gtk.NoSuchWidget"))
        self.assertNotEqual(invalid, original)
        self.blp.write_text(invalid)
        self.configure()
        before = hashes(self.source)
        result = self.compile(ok=False)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn(b"NoSuch", result.stdout)
        self.assertFalse((self.build / "resource-probe").exists())
        self.assertEqual(before, hashes(self.source))

    def test_primary_contract_rejects_wrong_widget_or_inactive_spinner(self):
        content = self.ui.read_bytes()
        assert_primary_contract(self, content)
        invalid_templates = ([content.replace(b"GtkSpinner", b"AdwSpinner"),
                              content.replace(b'name="spinning">true', b'name="spinning">false')]
                             if IS_APP else [content.replace(b"NostrGtkProfilePane", b"WrongWidget")])
        for invalid in invalid_templates:
            with self.assertRaises(AssertionError):
                assert_primary_contract(self, invalid)


if __name__ == "__main__":
    unittest.main(argv=[__file__, *unittest_args])
