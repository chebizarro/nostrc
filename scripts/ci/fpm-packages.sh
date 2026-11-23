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

pkg() { # NAME ROOT [FPM_ARGS...]
  local name=$1 root=$2; shift 2
  fpm -s dir -t "$type" -n "$name" -v "$version" -C "$root" -p "$out/${name}_VERSION_ARCH.$type" "$@" .
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
    usr/share/dbus-1/services/org.nostr.Grotto.service \
    usr/share/applications/org.nostr.Grotto.desktop
  (cd "$stage" && ls usr/share/icons/hicolor/*/apps/org.nostr.Grotto.png) |
    while read -r f; do put "$r" "$f"; done
  # The GUI's embedded panels SDK installs its runtime at the library root.
  # Keep only its versioned DSOs, not headers, static archives or SDK metadata.
  mkdir -p "$r/usr/lib/grotto"
  cp -a "$stage"/usr/lib/libgrotto-panels-0.so.* "$r/usr/lib/grotto/"
  glib-compile-schemas "$r/usr/share/glib-2.0/schemas"
  # The daemon package owns the shared private runtime used by both binaries.
  pkg grotto "$r" --depends "grotto-daemon = $version"
fi

if [[ $component == global || $component == grotto || $component == grotto-daemon-tcp ]]; then
  name=grotto-daemon
  [[ $component == grotto-daemon-tcp ]] && name=grotto-daemon-tcp
  r=package-roots/$name
  put "$r" usr/bin/grotto-daemon usr/share/systemd/user/grotto-daemon.service
  mkdir -p "$r/usr/lib/grotto"
  cp -a "$stage/usr/lib/grotto/." "$r/usr/lib/grotto/"
  pkg "$name" "$r"
fi
