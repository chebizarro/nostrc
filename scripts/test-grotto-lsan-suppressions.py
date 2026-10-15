#!/usr/bin/env python3
"""Check representative fixture/object leaks remain visible under GUI exceptions."""
import os
from pathlib import Path
import re
import shlex
import subprocess
import sys

if len(sys.argv) != 2:
    raise SystemExit("usage: test-grotto-lsan-suppressions.py OUTPUT_DIR")
root = Path(__file__).resolve().parents[1]
supp = root / "apps/grotto/tests/lsan.supp"
entries = [line for line in supp.read_text().splitlines() if line and not line.startswith("#")]
assert entries and all(re.fullmatch(r"leak:[A-Za-z_][A-Za-z_0-9]*", line) for line in entries), \
    "Only exact third-party function names, never module/path/wildcard patterns"
assert not any(line.split(":")[1] in {"g_malloc", "g_malloc0", "g_object_new", "g_type_create_instance"}
               or "variant" in line.lower() for line in entries), "No application allocators"

# The async GIO allocation stack cannot contain its consuming GTK callback.
# Its exception is valid only while these GUI/library sources own no such query.
for directory in ("apps/grotto", "nips/nip55l", "nostr-gobject", "nostr-gtk"):
    for path in (root / directory).rglob("*"):
        if path.suffix in {".c", ".h"} and path.is_file():
            assert "g_file_query_info_async" not in path.read_text(), \
                f"Reassess query_info_async_thread LSan exception before adding app queries: {path}"

out = Path(sys.argv[1]).resolve()
out.mkdir(parents=True, exist_ok=True)
source = out / "deliberate-leaks.c"
source.write_text(r'''
#include <gio/gio.h>
#include <string.h>
__attribute__((noinline)) static void grotto_deliberate_variant_leak(void) {
  g_variant_ref_sink(g_variant_new_string("Grotto fixture ownership must be checked"));
}
__attribute__((noinline)) static void grotto_deliberate_bus_leak(void) {
  /* Constructor only: no bus, fork watcher or session singleton is started. */
  g_test_dbus_new(G_TEST_DBUS_NONE);
}
__attribute__((noinline)) static void grotto_deliberate_file_info_leak(void) {
  g_file_info_new();
}
int main(int argc, char **argv) {
  if (argc != 2) return 2;
  if (!strcmp(argv[1], "variant")) grotto_deliberate_variant_leak();
  else if (!strcmp(argv[1], "bus")) grotto_deliberate_bus_leak();
  else if (!strcmp(argv[1], "file-info")) grotto_deliberate_file_info_leak();
  else return 2;
  g_print("Deliberate application leak fixture finished\n");
  return 0;
}
''')
flags = shlex.split(subprocess.check_output(["pkg-config", "--cflags", "--libs", "gio-2.0"], text=True))
binary = out / "deliberate-leaks"
subprocess.run([os.environ.get("CC", "cc"), "-g", "-O0", "-fno-omit-frame-pointer",
                "-fsanitize=address", str(source), "-o", str(binary), *flags], check=True)
env = dict(os.environ, ASAN_OPTIONS="detect_leaks=1:halt_on_error=1",
           LSAN_OPTIONS=f"suppressions={supp}:print_suppressions=1")
for case, symbol in [("variant", "grotto_deliberate_variant_leak"),
                     ("bus", "grotto_deliberate_bus_leak"),
                     ("file-info", "grotto_deliberate_file_info_leak")]:
    result = subprocess.run([str(binary), case], env=env, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, text=True)
    (out / f"{case}.log").write_text(result.stdout)
    assert result.returncode != 0 and "LeakSanitizer: detected memory leaks" in result.stdout \
        and symbol in result.stdout, f"{case}: leak was hidden or sanitizer did not execute; see {out}"
    print(f"{case}: deliberate application/fixture leak still fails with GUI exceptions loaded")
