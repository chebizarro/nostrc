#!/usr/bin/env python3
"""Recompile authoritative Blueprint using the pinned compiler; never edit UI XML."""
import ast
import pathlib
import re
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
production_ui = {r.text for r in resources.findall('.//file') if r.text.endswith('.ui')}
for suffix in ('.ui', '.blp'):
    on_disk = {str(p.relative_to(app / 'data').with_suffix('.ui')) for p in (app / 'data/ui').rglob('*' + suffix)}
    for name in sorted(on_disk - production_ui):
        failed.append(f'{name}: orphan {suffix} outside the production resource inventory')
    for name in sorted(production_ui - on_disk):
        failed.append(f'{name}: missing {suffix}')
cmake = (app / 'CMakeLists.txt').read_text()
meson = (app / 'meson.build').read_text()
for build, source in [('CMake', cmake), ('Meson', meson)]:
    listed = {str(pathlib.PurePosixPath(p).with_suffix('.ui')).removeprefix('data/') for p in re.findall(r'data/ui/[\w/.-]+\.blp', source)}
    if listed != production_ui:
        failed.append(f'{build}: Blueprint inventory differs from gresource: {sorted(listed ^ production_ui)}')
    for path in (app / 'src/ui').rglob('*.c'):
        if str(path.relative_to(app)) not in source:
            failed.append(f'{build}: unlisted UI source {path.relative_to(app)}')
def message_ids(path):
    ids, fragments = set(), None
    for line in path.read_text().splitlines():
        if line.startswith('msgid '):
            if fragments is not None:
                ids.add(''.join(fragments))
            fragments = [ast.literal_eval(line[6:])]
        elif fragments is not None and line.startswith('"'):
            fragments.append(ast.literal_eval(line))
        elif fragments is not None:
            ids.add(''.join(fragments)); fragments = None
    if fragments is not None:
        ids.add(''.join(fragments))
    return ids
pot_ids = message_ids(app / 'po/grotto.pot')
for catalogue in (app / 'po').glob('*.po'):
    for orphan in sorted(message_ids(catalogue) - pot_ids):
        failed.append(f'{catalogue.name}: retired translation entry {orphan}')
    # msgmerge --no-obsolete must reconcile shipped catalogues with the live POT.
    if '#~ msgid' in catalogue.read_text():
        failed.append(f'{catalogue.name}: obsolete translation entries remain')
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
