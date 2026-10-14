#!/usr/bin/env python3
"""Recompile authoritative Blueprint using the pinned compiler; never edit UI XML."""
import pathlib
import shutil
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET

app = pathlib.Path(sys.argv[1])
compiler = shutil.which('blueprint-compiler')
if not compiler:
    sys.exit('blueprint-compiler 0.22.2 is required for signer/blueprint-drift')
version = subprocess.check_output([compiler, '--version'], text=True).strip()
if version != '0.22.2':
    sys.exit(f'Expected blueprint-compiler 0.22.2, found {version}')
resources = ET.parse(app / 'data/resources.gresource.xml')
failed = []
for name in (app / 'po/POTFILES.in').read_text().splitlines():
    if name and not name.startswith('#') and not (app / name).is_file():
        failed.append(f'POTFILES.in: missing production input {name}')
with tempfile.TemporaryDirectory(prefix='grotto-blueprint-') as tmp:
    for resource in resources.findall('.//file'):
        name = resource.text
        if not name.endswith('.ui'):
            continue
        ui = app / 'data' / name
        blueprint = ui.with_suffix('.blp')
        if not blueprint.exists():
            failed.append(f'{name}: no authoritative Blueprint')
            continue
        output = pathlib.Path(tmp) / ui.name
        subprocess.run([compiler, 'compile', '--output', str(output), str(blueprint)], check=True)
        if output.read_bytes().rstrip(b'\n') != ui.read_bytes().rstrip(b'\n'):
            failed.append(f'{name}: regenerate with blueprint-compiler 0.22.2')
    # The template must match its C API on the 1.5 floor, not a newer Adw dialog.
    shortcuts = ET.parse(app / 'data/ui/shortcuts-window.ui').find('object')
    if shortcuts.get('class') != 'GtkShortcutsWindow':
        failed.append('shortcuts-window: must be GtkShortcutsWindow')
if failed:
    sys.exit('\n'.join(failed))
subprocess.run([sys.executable, str(app / '../../scripts/generate-grotto-kind-catalog.py'), '--check'], check=True)
print('All production templates match pinned Blueprint 0.22.2; kind catalogue matches registry')
