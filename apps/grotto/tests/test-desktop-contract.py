#!/usr/bin/env python3
"""Canonical G08 install/activation contract; Linux requires a private bus and Xvfb.

Apple follows the existing G07 harness policy: CMake does not build the real
GTK application test core there. Static packaging and headless controller checks
still run on Apple; production-window activation acceptance runs on Linux/Xvfb.
This omission is not a claim of native macOS GUI acceptance.
"""
import configparser
import pathlib
import subprocess
import shutil
import sys
import xml.etree.ElementTree as ET

root = pathlib.Path(sys.argv[1])
build = pathlib.Path(sys.argv[2])
app = root / 'apps/grotto'

def desktop(path):
    result = configparser.ConfigParser(interpolation=None, strict=True)
    result.optionxform = str
    result.read(path)
    return result

entry = desktop(app / 'data/org.nostr.Grotto.desktop')
assert entry['Desktop Entry']['Name'] == 'Grotto (Preview)'
assert entry['Desktop Entry']['DBusActivatable'] == 'true'
assert entry['Desktop Entry']['X-GNOME-UsesNotifications'] == 'true'
assert set(entry['Desktop Entry']['Actions'].strip(';').split(';')) == {'new-identity', 'settings'}
assert entry['Desktop Action new-identity']['Exec'] == 'grotto --new-identity'
assert entry['Desktop Action settings']['Exec'] == 'grotto --settings'
assert set(entry['Desktop Entry']['MimeType'].strip(';').split(';')) == {
    'x-scheme-handler/bunker', 'x-scheme-handler/nostrconnect', 'x-scheme-handler/grotto'}
login = desktop(build / 'apps/grotto/org.nostr.Grotto-autostart.desktop')
assert login['Desktop Entry']['Exec'].endswith('/grotto" --gapplication-service')
assert login['Desktop Entry']['NoDisplay'] == 'true'
assert 'DBusActivatable' not in login['Desktop Entry']  # Exec is the windowless login contract.
for name, exe in [('org.nostr.Grotto', 'grotto'), ('org.nostr.Signer', 'grotto-daemon')]:
    service = desktop(build / f'apps/grotto/{name}.service')['D-BUS Service']
    assert service['Name'] == name
    assert service['Exec'].endswith(f'/{exe}' + (' --gapplication-service' if exe == 'grotto' else ''))
validator = shutil.which('appstreamcli')
if validator:
    subprocess.run([validator, 'validate', '--no-net', str(app / 'data/org.nostr.Grotto.metainfo.xml')], check=True)
elif sys.platform != 'darwin':
    raise RuntimeError('Linux desktop-contract requires appstreamcli (install appstream)')
metadata = ET.parse(app / 'data/org.nostr.Grotto.metainfo.xml')
assert metadata.findtext('name') == 'Grotto (Preview)'
copy = ' '.join(metadata.getroot().itertext()).lower()
for promise in ('same-user malware', 'signing is not publication', 'transiently handle key material'):
    assert promise in copy, promise
for unproven in ('hardware wallet', 'pkcs#11', 'multisig', 'social recovery', 'delegation'):
    assert unproven not in copy, unproven
for screenshot in metadata.findall('screenshots/screenshot/image'):
    prefix = 'https://raw.githubusercontent.com/chebizarro/nostrc/master/apps/grotto/data/screenshots/'
    assert screenshot.text.startswith(prefix), screenshot.text
    local = app / 'data/screenshots' / screenshot.text.removeprefix(prefix)
    assert local.is_file() and local.read_bytes().startswith(b'\x89PNG\r\n\x1a\n'), local
assert {node.text for node in metadata.findall('provides/mediatype')} == {
    'x-scheme-handler/bunker', 'x-scheme-handler/nostrconnect', 'x-scheme-handler/grotto'}
assert 'org.nostr.Grotto' in {node.text for node in metadata.findall('provides/dbus')}
schema = ET.parse(app / 'data/org.nostr.Grotto.gschema.xml')
keys = {key.attrib['name']: key.findtext('default') for key in schema.findall('.//key')}
assert {key: keys[key] for key in ('run-in-background', 'launch-on-login', 'pause-on-screen-lock',
    'settings-migration-version')} == {'run-in-background': 'true', 'launch-on-login': 'false',
    'pause-on-screen-lock': 'false', 'settings-migration-version': '0'}
cmake = (app / 'CMakeLists.txt').read_text()
meson = (app / 'meson.build').read_text()
for text in (cmake, meson):
    for asset in ('data/org.nostr.Grotto.desktop', 'data/org.nostr.Grotto.service.in',
                  'data/org.nostr.Grotto.metainfo.xml', 'data/org.nostr.Grotto-autostart.desktop.in',
                  'daemon/packaging/systemd/user/grotto-daemon.service',
                  'daemon/packaging/systemd/user/grotto-daemon.service.d/bunker.conf.example',
                  'daemon/packaging/systemd/user/grotto-daemon.service.d/headless-bunker.conf.example'):
        assert asset in text, asset
assert "install_data('packaging/appimage/grotto.desktop'" not in meson
# Version consolidation is atomic with native packaging: no independent About
# or daemon literal may drift away from the build's component version.
version = metadata.find('releases/release').attrib['version']
assert f'set(GROTTO_VERSION "{version}"' in cmake
assert f"version: '{version}'" in meson
assert f'-DGROTTO_VERSION="@0@"' in meson
assert 'set_version(ADW_ABOUT_DIALOG(about), GROTTO_VERSION)' in (app / 'src/main_app.c').read_text()
assert '#define DAEMON_VERSION GROTTO_VERSION' in (app / 'daemon/main_daemon.c').read_text()
assert (app / 'packaging/debian/changelog').read_text().startswith(f'grotto ({version}-')
assert f'Version:        {version}\n' in (app / 'packaging/rpm/grotto.spec').read_text()
assert f'pkgver={version}\n' in (app / 'packaging/archlinux/PKGBUILD').read_text()
# The removed unsupported TCP package must not survive as a dangling overlay export.
assert 'grotto-daemon-tcp' not in (root / 'flake.nix').read_text()
control = (app / 'packaging/debian/control').read_text()
assert control.count('\nPackage: ') == 1 and '\nPackage: grotto\n' in control
for payload in ('usr/bin/grotto-daemon', 'org.nostr.Signer.service',
                'org.nostr.Grotto.service', 'org.nostr.Grotto-autostart.desktop',
                'usr/share/doc/grotto/examples/*'):
    assert payload in (app / 'packaging/debian/grotto.install').read_text(), payload
assert (app / 'data/icons/hicolor/scalable/apps/org.nostr.Grotto-symbolic.svg').is_file()
for source in (app / 'src').rglob('*.c'):
    if source.name != 'background.c':
        assert '"autostart"' not in source.read_text(), source
        assert '"start-minimized"' not in source.read_text(), source
for size in (16, 24, 32, 48, 64, 128, 256, 512):
    assert (app / f'data/icons/hicolor/{size}x{size}/apps/org.nostr.Grotto.png').is_file()
if len(sys.argv) > 3:
    result = subprocess.run([sys.argv[3]], check=False)
    result.check_returncode()
elif sys.platform == 'darwin':
    print('Dynamic GTK activation skipped under the documented Apple G07 worker policy')
else:
    raise RuntimeError('Linux desktop-contract requires the GTK activation worker and Xvfb')
print('G08 canonical desktop, actions, MIME, service and install contract passed')
