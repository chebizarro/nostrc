#!/usr/bin/env bash
# Run as root only in a disposable Debian/Ubuntu CI runner/container with Grotto's
# runtime dependencies installed. Usage: bash test-debian-triggers.sh grotto.deb
set -euo pipefail

if [[ ${GROTTO_DEB_TEST_DISPOSABLE:-0} != 1 || $EUID != 0 || $# != 1 ]]; then
  echo 'Requires root, GROTTO_DEB_TEST_DISPOSABLE=1, and one built grotto.deb; never run on a desktop.' >&2
  exit 2
fi
for tool in dpkg dpkg-deb dpkg-query lintian gsettings gtk-update-icon-cache; do
  command -v "$tool" >/dev/null || { echo "Missing required tool: $tool" >&2; exit 2; }
done
deb=$(realpath "$1")
[[ $(dpkg-deb -f "$deb" Package) == grotto ]]
fixture=nostrc-cache-trigger-test
if dpkg-query -W -f='${db:Status-Status}' "$fixture" 2>/dev/null | grep -qx installed; then
  echo "Refusing to replace existing $fixture" >&2
  exit 2
fi
work=$(mktemp -d)
trap 'dpkg --purge "$fixture" >/dev/null 2>&1 || true; rm -rf "$work"' EXIT
assert_clean() {
  dpkg --audit > "$work/audit"
  if [[ -s $work/audit ]]; then cat "$work/audit" >&2; return 1; fi
  [[ $(dpkg-query -W -f='${Status}' grotto) == 'install ok installed' ]]
}
# Restrict lintian to maintainer-script checks; unrelated payload/release checks
# are not part of this regression test. Warnings as well as errors fail.
lintian --allow-root --no-cfg --check-part scripts --fail-on error,warning "$deb"
dpkg-deb -e "$deb" "$work/control"
if [[ -f $work/control/triggers ]] && grep -Eq '^interest(-noawait|-await)?[[:space:]]+/usr/share/(glib-2.0/schemas|icons/hicolor|applications)(/|[[:space:]]|$)' "$work/control/triggers"; then
  echo 'Grotto must not subscribe to system-owned desktop cache triggers' >&2
  exit 1
fi
dpkg --audit > "$work/audit"
[[ ! -s $work/audit ]] || { cat "$work/audit" >&2; exit 1; }
dpkg --install "$deb"
assert_clean
# Exercise the shipped generated postinst, if dh needed one, not a source stub.
if [[ -f /var/lib/dpkg/info/grotto.postinst ]]; then
  for action in triggered abort-upgrade abort-remove abort-deconfigure; do
    /var/lib/dpkg/info/grotto.postinst "$action" 'nostrc-cache-trigger-test'
  done
fi

mkdir -p "$work/fixture/DEBIAN" "$work/fixture/usr/share/glib-2.0/schemas" \
  "$work/fixture/usr/share/icons/hicolor/scalable/apps" "$work/fixture/usr/share/applications"
cat > "$work/fixture/DEBIAN/control" <<'CONTROL'
Package: nostrc-cache-trigger-test
Version: 1.0
Architecture: all
Maintainer: Nostr CI <ci@example.org>
Description: disposable desktop cache trigger activator
CONTROL
cat > "$work/fixture/usr/share/glib-2.0/schemas/org.nostrc.cache-trigger-test.gschema.xml" <<'SCHEMA'
<schemalist>
  <schema id="org.nostrc.cache-trigger-test" path="/org/nostrc/cache-trigger-test/">
    <key name="enabled" type="b"><default>true</default></key>
  </schema>
</schemalist>
SCHEMA
cat > "$work/fixture/usr/share/icons/hicolor/scalable/apps/nostrc-cache-trigger-test.svg" <<'ICON'
<svg xmlns="http://www.w3.org/2000/svg" width="16" height="16"><rect width="16" height="16"/></svg>
ICON
cat > "$work/fixture/usr/share/applications/nostrc-cache-trigger-test.desktop" <<'DESKTOP'
[Desktop Entry]
Type=Application
Name=Nostr cache trigger test
Exec=true
MimeType=application/x-nostrc-cache-trigger-test;
DESKTOP
dpkg-deb --build --root-owner-group "$work/fixture" "$work/fixture.deb"
# Leave triggers pending so the test proves each real file trigger was activated.
dpkg --no-triggers --unpack "$work/fixture.deb"
dpkg-query -W -f='${binary:Package} ${Triggers-Pending}\n' > "$work/pending"
grep '/usr/share/' "$work/pending"
for path in /usr/share/glib-2.0/schemas /usr/share/icons/hicolor /usr/share/applications; do
  grep -F "$path" "$work/pending" >/dev/null || { echo "Trigger not activated: $path" >&2; exit 1; }
done
dpkg --no-triggers --configure "$fixture"
dpkg --triggers-only --pending
assert_clean
gsettings list-keys org.nostrc.cache-trigger-test | grep -qx enabled
gtk-update-icon-cache --validate --quiet /usr/share/icons/hicolor
grep -F 'application/x-nostrc-cache-trigger-test=nostrc-cache-trigger-test.desktop;' /usr/share/applications/mimeinfo.cache
# Removal must update the caches too, without leaving Grotto half-configured.
dpkg --purge "$fixture"
assert_clean
if gsettings list-schemas | grep -x org.nostrc.cache-trigger-test >/dev/null; then
  echo 'Removed schema remains in the compiled cache' >&2
  exit 1
fi
echo 'PASS: Grotto install, supported postinst callbacks, desktop file triggers and removal; dpkg audit clean'
