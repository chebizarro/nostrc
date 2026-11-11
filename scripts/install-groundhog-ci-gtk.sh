#!/usr/bin/env bash
# CI-only Noble ownership fixes (nostrc-swjdk, nostrc-4bi6r), not leak suppressions.
# Use Ubuntu's source including its distro/security patches, not an upstream
# replacement that silently drops them. Install privately; never replace apt's
# libraries. Both hosted sanitizers and linux-ci.Dockerfile use this recipe.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
PREFIX="${GROUNDHOG_CI_GTK_PREFIX:-/opt/groundhog-ci-gtk}"
JOBS="${JOBS:-4}"
WORK="$(mktemp -d /tmp/groundhog-ci-gtk.XXXXXX)"
trap 'rm -rf "$WORK"' EXIT
for tool in curl sha256sum tar dpkg-source dpkg-buildflags patch meson ninja sassc; do
  command -v "$tool" >/dev/null || { echo "Missing $tool: install the CI build dependencies" >&2; exit 1; }
done

fetch() {
  local pool="$1" name="$2" digest="$3"
  if [[ -n "${GROUNDHOG_CI_SOURCE_CACHE:-}" && -f "$GROUNDHOG_CI_SOURCE_CACHE/$name" ]]; then
    cp "$GROUNDHOG_CI_SOURCE_CACHE/$name" "$WORK/$name"
  else
    curl --fail --location --retry 3 --connect-timeout 30 --max-time 300 \
      "https://archive.ubuntu.com/ubuntu/pool/main/$pool/$name" --output "$WORK/$name"
  fi
  printf '%s  %s\n' "$digest" "$WORK/$name" | sha256sum --check
}
# SHA256 values from the matching Ubuntu .dsc files, including the distro patch
# archives. Pin source and fixes together; review them on dependency updates.
fetch g/gtk4 gtk4_4.14.5+ds.orig.tar.xz bd52b294cd18784339b105e2f25d2569e3db94411c64b9b141b23fe39118a0e5
fetch g/gtk4 gtk4_4.14.5+ds-0ubuntu0.10.debian.tar.xz 17692a86b96fa9bc930f744694c02a43f7a048abdfb1f9864fe2b9925f649d00
fetch liba/libadwaita-1 libadwaita-1_1.5.0.orig.tar.xz 92d887c51c164ddd4b445db64edf13fa3a198cd15202006988e9aac9613e1f41
fetch liba/libadwaita-1 libadwaita-1_1.5.0-1ubuntu2.debian.tar.xz a110ad291d0496a632a245589f2da86dc1202edda8d88fb81fddc48a2c493325

prepare() {
  local dir="$1" orig="$2" distro="$3" fix="$4"
  mkdir "$dir"
  tar -xJf "$WORK/$orig" --strip-components=1 -C "$dir"
  tar -xJf "$WORK/$distro" -C "$dir"
  dpkg-source --before-build "$dir"
  patch --batch --fuzz=0 -d "$dir" -p1 < "$HERE/ci/$fix"
}
prepare "$WORK/gtk" gtk4_4.14.5+ds.orig.tar.xz gtk4_4.14.5+ds-0ubuntu0.10.debian.tar.xz gtk-4.14.5-relation-ownership.patch
prepare "$WORK/adwaita" libadwaita-1_1.5.0.orig.tar.xz libadwaita-1_1.5.0-1ubuntu2.debian.tar.xz libadwaita-1.5.0-ownership.patch

# Match the distro's plain Meson configuration and compiler flags. Meson's
# debugoptimized preset also enables GTK's extra debug-only geometry checks,
# which is NOT the distro runtime being reproduced. -g still supplies full
# symbols; Groundhog's ASan/UBSan/LSan settings and fatal-critical guards stay on.
export DEB_BUILD_MAINT_OPTIONS=hardening=+all
export CFLAGS="$(dpkg-buildflags --get CFLAGS)"
export CPPFLAGS="$(dpkg-buildflags --get CPPFLAGS)"
export LDFLAGS="$(dpkg-buildflags --get LDFLAGS)"
# Unstripped debug symbols are integral to these CI libraries. No modules,
# allocators or app frames are suppressed. Keep the ordinary X11/Wayland and
# GL/Cairo paths; only upstream's own demos/tests/docs and optional services
# are omitted from this dependency build.
meson setup "$WORK/gtk-build" "$WORK/gtk" --prefix "$PREFIX" --libdir lib \
  --buildtype plain --wrap-mode nodownload -Dstrip=false -Db_lto=false \
  -Dintrospection=disabled -Dbuild-tests=false -Dbuild-testsuite=false \
  -Dbuild-examples=false -Dbuild-demos=false -Ddocumentation=false -Dman-pages=false \
  -Dmedia-gstreamer=disabled -Dvulkan=disabled \
  -Dprint-cups=disabled -Dcloudproviders=disabled -Dtracker=disabled -Dcolord=disabled \
  -Dsysprof=disabled
meson compile -C "$WORK/gtk-build" -j "$JOBS"
meson install -C "$WORK/gtk-build"
export PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
export LD_LIBRARY_PATH="$PREFIX/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
meson setup "$WORK/adwaita-build" "$WORK/adwaita" --prefix "$PREFIX" --libdir lib \
  --buildtype plain --wrap-mode nodownload -Dstrip=false -Db_lto=false \
  -Dintrospection=disabled -Dvapi=false -Dtests=false -Dexamples=false -Dgtk_doc=false
meson compile -C "$WORK/adwaita-build" -j "$JOBS"
meson install -C "$WORK/adwaita-build"
printf 'CI ownership fixes installed in %s: GTK %s, libadwaita %s\n' \
  "$PREFIX" "$(pkg-config --modversion gtk4)" "$(pkg-config --modversion libadwaita-1)"
