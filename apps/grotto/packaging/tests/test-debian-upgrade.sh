#!/usr/bin/env bash
# Metadata-only dpkg regression; root is permitted only in a disposable runner.
set -euo pipefail
if [[ ${GROTTO_DEB_TEST_DISPOSABLE:-0} != 1 || $EUID != 0 ]]; then
  echo 'Requires root and GROTTO_DEB_TEST_DISPOSABLE=1 in a disposable container' >&2
  exit 2
fi
control=$(cd "$(dirname "$0")/../debian" && pwd)/control
work=$(mktemp -d)
for package in grotto grotto-daemon; do
  if dpkg-query -W -f='${db:Status-Status}' "$package" 2>/dev/null | grep -qx installed; then
    echo "Refusing to replace existing $package" >&2
    exit 2
  fi
done
trap 'dpkg --purge grotto grotto-daemon >/dev/null 2>&1 || true; rm -rf "$work"' EXIT
mkdir -p "$work/old/DEBIAN" "$work/new/DEBIAN"
cat > "$work/old/DEBIAN/control" <<'CONTROL'
Package: grotto-daemon
Version: 0.1.1-alpha1
Architecture: all
Maintainer: Test <test@example.org>
Description: old split daemon fixture
CONTROL
# Copy the actual package transition fields, without its runtime dependencies.
awk '/^(Breaks|Replaces):/ {print; continuation=1; next}
     /^[[:space:]]/ {if (continuation) print; next}
     {continuation=0}' "$control" > "$work/new/DEBIAN/control"
cat >> "$work/new/DEBIAN/control" <<'CONTROL'
Package: grotto
Version: 0.1.2~alpha1-1
Architecture: all
Maintainer: Test <test@example.org>
Description: combined package fixture
CONTROL
for tree in old new; do
  mkdir -p "$work/$tree/usr/bin" "$work/$tree/usr/share/systemd/user"
  printf '%s\n' "$tree" > "$work/$tree/usr/bin/grotto-daemon"
  printf '%s\n' "$tree" > "$work/$tree/usr/share/systemd/user/grotto-daemon.service"
  dpkg-deb --build "$work/$tree" "$work/$tree.deb"
done
dpkg --install "$work/old.deb"
dpkg --auto-deconfigure --install "$work/new.deb"
[[ $(cat /usr/bin/grotto-daemon) == new ]]
[[ $(cat /usr/share/systemd/user/grotto-daemon.service) == new ]]
[[ $(dpkg-query -W -f='${Status}' grotto) == 'install ok installed' ]]
[[ -z $(dpkg --audit) ]]
