#!/usr/bin/env bash
# Package an installed release tree (cmake --install --prefix STAGE/usr) as
# deb or rpm packages with fpm. Shared by the deb and rpm jobs of
# .github/workflows/release.yml so both formats ship the same files.
#
#   scripts/ci/fpm-packages.sh deb|rpm COMPONENT VERSION STAGE OUT
#
# COMPONENT: global (everything), gnostr, grotto, or grotto-daemon-tcp (the
# TCP-IPC daemon alone). fpm names files NAME_VERSION_ARCH.{deb,rpm}.
set -euo pipefail

type=${1:?type} component=${2:?component} version=${3:?version}
stage=$(cd "${4:?stage}" && pwd)
out=${5:?out}
mkdir -p "$out" package-roots

pkg() { # NAME ROOT
  fpm -s dir -t "$type" -n "$1" -v "$version" -C "$2" -p "$out/$1_VERSION_ARCH.$type" .
}
put() { # ROOT FILE...: copy STAGE/usr files into ROOT, same paths
  local root=$1; shift
  for f in "$@"; do
    install -D -m "$( [ -x "$stage/$f" ] && echo 755 || echo 644 )" "$stage/$f" "$root/$f"
  done
}

if [[ $component == global || $component == gnostr ]]; then
  r=package-roots/gnostr
  put "$r" usr/bin/gnostr \
    usr/share/applications/org.gnostr.gnostr.desktop \
    usr/share/icons/hicolor/scalable/apps/org.gnostr.gnostr.svg
  pkg gnostr "$r"
fi

if [[ $component == global || $component == grotto ]]; then
  r=package-roots/grotto
  put "$r" usr/bin/grotto \
    usr/share/glib-2.0/schemas/org.nostr.Grotto.gschema.xml \
    usr/share/dbus-1/services/org.nostr.Signer.service \
    usr/share/applications/org.nostr.Grotto.desktop
  (cd "$stage" && ls usr/share/icons/hicolor/*/apps/org.nostr.Grotto.png) |
    while read -r f; do put "$r" "$f"; done
  glib-compile-schemas "$r/usr/share/glib-2.0/schemas"
  pkg grotto "$r"
fi

if [[ $component == global || $component == grotto || $component == grotto-daemon-tcp ]]; then
  name=grotto-daemon
  [[ $component == grotto-daemon-tcp ]] && name=grotto-daemon-tcp
  r=package-roots/$name
  put "$r" usr/bin/grotto-daemon usr/share/systemd/user/grotto-daemon.service
  pkg "$name" "$r"
fi
