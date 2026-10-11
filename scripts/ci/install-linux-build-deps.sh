#!/bin/sh
# Install everything the nostrc apps (gnostr, Grotto, grotto-daemon) need to
# build on an Ubuntu 24.04 runner, x86_64 or aarch64. Shared by the Linux
# jobs of .github/workflows/release.yml and gnostr-appimage.yml so their
# dependency lists cannot drift apart.
#
#   scripts/ci/install-linux-build-deps.sh [--fpm] [--appimage]
#
# --fpm also installs fpm (deb/rpm jobs); --appimage the tools linuxdeploy
# needs plus what the AppImage smoke test runs under.
set -eu

fpm=0 appimage=0
for arg in "$@"; do
  case "$arg" in
    --fpm) fpm=1 ;;
    --appimage) appimage=1 ;;
    *) echo "install-linux-build-deps: unknown option $arg" >&2; exit 2 ;;
  esac
done

sudo=
[ "$(id -u)" -eq 0 ] || sudo=sudo
export DEBIAN_FRONTEND=noninteractive

pkgs="build-essential cmake ninja-build pkg-config git ca-certificates python3-pip
  libgtk-4-dev libadwaita-1-dev libjansson-dev libwebsockets-dev
  libsecret-1-dev libssl-dev libpeas-2-dev libjson-glib-dev
  libsoup-3.0-dev libsqlite3-dev libqrencode-dev libsecp256k1-dev
  libsodium-dev libcurl4-openssl-dev
  librsvg2-dev libgdk-pixbuf-2.0-dev adwaita-icon-theme"
[ "$fpm" = 1 ] && pkgs="$pkgs ruby ruby-dev rubygems rpm"
[ "$appimage" = 1 ] && pkgs="$pkgs wget file patchelf desktop-file-utils xvfb xauth"

$sudo apt-get update -qq
# shellcheck disable=SC2086
$sudo apt-get install -y -qq --no-install-recommends $pkgs >/dev/null
# Noble's blueprint-compiler is older than the .blp files need.
$sudo python3 -m pip install --break-system-packages -q blueprint-compiler==0.22.2
[ "$fpm" = 1 ] && $sudo gem install --no-document fpm
exit 0
