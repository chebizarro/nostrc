#!/usr/bin/env python3
"""The installed CLI version query must not start GTK, D-Bus, or migration."""
import os
import pathlib
import subprocess
import sys
import tempfile

with tempfile.TemporaryDirectory(prefix='grotto-version-') as tmp:
    config = pathlib.Path(tmp) / 'config'
    legacy = config / 'gnostr-signer'  # grotto-names: allow legacy-fixture (pre-rename config dir; must stay untouched)
    legacy.mkdir(parents=True)
    sentinel = legacy / 'sentinel'
    sentinel.write_text('preserve legacy state\n')
    env = dict(os.environ, XDG_CONFIG_HOME=str(config), DISPLAY='',
               WAYLAND_DISPLAY='', DBUS_SESSION_BUS_ADDRESS='unix:path=/nonexistent')
    result = subprocess.run([sys.argv[1], '--version'], env=env, text=True,
                            capture_output=True, timeout=3, check=True)
    assert result.stdout == f'Grotto {sys.argv[2]}\n', result.stdout
    assert result.stderr == '', result.stderr
    assert sentinel.read_text() == 'preserve legacy state\n'
    assert not (config / 'grotto').exists(), 'version query migrated user state'
print('Version query passed without display, bus, or config migration')
