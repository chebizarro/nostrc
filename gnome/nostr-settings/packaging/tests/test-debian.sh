#!/usr/bin/env bash
# Build the real recipe and exercise its extracted entrypoint. Requires the
# installed panels SDK (build its native packages first in a disposable CI host).
# No installation, owner bus, config or keyring access.
set -euo pipefail
root=$(cd "$(dirname "$0")/../../../.." && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
mkdir "$work/source"
if git -C "$root" rev-parse --is-inside-work-tree >/dev/null 2>&1; then
  git -C "$root" ls-files -z --recurse-submodules |
    (cd "$root" && tar --null -T - -cf -) | tar -xf - -C "$work/source"
else
  rsync -a --exclude=.git --exclude='/build*' --exclude='/obj-*' "$root/" "$work/source/"
fi
rm -rf "$work/source/debian"
cp -a "$root/gnome/nostr-settings/packaging/debian" "$work/source/debian"
(cd "$work/source" && dpkg-buildpackage -b -us -uc -j"${JOBS:-2}")
deb=("$work"/nostr-settings_*.deb)
test "${#deb[@]}" -eq 1
dpkg-deb -f "${deb[0]}" Depends | grep -E 'libgrotto-panels-0-0 \(>= 0\.1\.0\)'
dpkg-deb -x "${deb[0]}" "$work/payload"
test -f "$work/payload/usr/share/dbus-1/services/org.nostr.Settings.service"
test -f "$work/payload/usr/share/glib-2.0/schemas/org.nostr.Settings.gschema.xml"
test -f "$work/payload/usr/lib/$(dpkg-architecture -qDEB_HOST_MULTIARCH)/nostr-settings/libnostr-json.so.2"
test -f "$work/payload/usr/lib/$(dpkg-architecture -qDEB_HOST_MULTIARCH)/nostr-settings/libnostr-publish.so.0"
# No other app/daemon, public library or activation authority may ride along.
test "$(find "$work/payload/usr/bin" -type f | wc -l)" -eq 1
test "$(find "$work/payload/usr/share/dbus-1/services" -type f | wc -l)" -eq 1
! find "$work/payload" -name '*nostr-share*' -o -name '*Signer.service' | grep .
version=$(sed -n 's/^set(NOSTR_SETTINGS_VERSION \([^)]*\)).*/\1/p' "$root/gnome/nostr-settings/CMakeLists.txt")
cmake -DNSS_EXECUTABLE="$work/payload/usr/bin/nostr-settings" -DNSS_VERSION="$version" \
  -P "$root/gnome/nostr-settings/tests/test-cli.cmake"
if ldd "$work/payload/usr/bin/nostr-settings" | grep 'not found'; then
  echo 'Unresolved packaged library' >&2; exit 1
fi
echo 'Settings Debian recipe, runtime dependencies, payload and CLI: PASS'
