#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Source + exported-symbol boundary check, not an authorization test."""
import pathlib
import re
import subprocess
import sys

root = pathlib.Path(sys.argv[1])
library = pathlib.Path(sys.argv[2])
forbidden = re.compile(r"ExportIdentity|ExportRawIdentity|ImportIdentity|StoreKeyWithMnemonic|DecideRequest|"
                       r"gn.secure.entry|accounts_store|session.manager|approval.controller|"
                       r"sign_event|secret_service|nostr_nip|g_timeout_add|g_usleep")
for path in [*root.joinpath('src').glob('*.c'), *root.joinpath('include').rglob('*.h')]:
    assert not forbidden.search(path.read_text()), f'forbidden library dependency/API: {path}'
header = root.joinpath('include/grotto-panels-0/grotto-panels.h').read_text()
metadata = re.search(r'typedef struct \{(.*?)\} GrottoPanelsMetadata;', header, re.S).group(1)
metadata = re.sub(r'/\*.*?\*/', '', metadata, flags=re.S)
fields = re.findall(r'(?:const char \*|gboolean |gint64 |guint64 )(\w+)\s*;', metadata)
assert set(fields) == {'id', 'label', 'public_key', 'state', 'reason', 'access_policy',
                       'needs_review', 'blocked', 'deadline_usec', 'sequence'}, fields
assert not re.search(r'GVariant|gpointer|GBytes|GByteArray', metadata), 'opaque payload in display metadata'
# No arbitrary method name, payload result, key/credential or signer operation.
assert 'GrottoPanelCommand command' in header
assert re.search(r'gboolean \(\*invoke_finish\)', header)
assert not re.search(r'(?i)nsec|mnemonic|passphrase|private_key|sign_event|decide_request', header)
for name in ['CMakeLists.txt', 'meson.build']:
    build = root.joinpath(name).read_text()
    production = build.split('include(CTest)')[0] if name.startswith('CMake') else build.split("if get_option('tests')")[0]
    assert not re.search(r'nostr.gobject|nostr.gtk|nip55|accounts_store|gnostr.secret', production)
    assert 'GDK_VERSION_4_14' in production and 'ADW_VERSION_1_5' in production
symbols = subprocess.check_output(['nm', '-g', str(library)] if sys.platform == 'darwin'
                                  else ['nm', '-D', str(library)], text=True)
assert not forbidden.search(symbols), 'forbidden symbol in shared library'
assert 'grotto_panel_get_type' in symbols and 'grotto_panels_service_get_type' in symbols
if sys.platform.startswith('linux'):
    dynamic = subprocess.check_output(['readelf', '-d', str(library)], text=True)
    needed = re.findall(r'\(NEEDED\).*?\[(.*?)\]', dynamic)
    assert needed, 'missing dynamic dependency evidence'
    # Sanitizer linkers retain GTK's normal pkg-config closure even when the
    # ordinary linker drops unused entries. Neither route may gain signer deps.
    flags = subprocess.check_output(['pkg-config', '--libs-only-l', 'gtk4', 'libadwaita-1'], text=True).split()
    allowed = {flag[2:] for flag in flags if flag.startswith('-l')} | {'c', 'm', 'gcc_s', 'asan', 'ubsan'}
    assert all(n.startswith('ld-linux-') or (n.startswith('lib') and n[3:].split('.so')[0] in allowed)
               for n in needed), needed
print('PASS: public metadata only; no policy, secret RPC, host symbols or polling in shared library')
