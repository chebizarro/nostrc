#!/bin/sh
# Build the groundhog and grotto Arch packages from a release source archive.
# Shared by the x86_64 (archlinux) and aarch64 (Arch Linux ARM) jobs of
# .github/workflows/groundhog-release.yml, and runnable by hand in either
# container:
#
#   scripts/ci/build-arch-package.sh src/groundhog-<version>.tar.gz dist
#
# Runs as root; makepkg runs as an unprivileged "build" user.
set -eux

archive=$(readlink -f "$1")
out=$(readlink -f "${2:-dist}")
version=$(basename "$archive" .tar.gz)
version=${version#groundhog-}

# pacman 7 downloads as a Landlock-sandboxed user; container kernels without
# Landlock (Docker Desktop, some CI hosts) make every download fail. This is
# a throwaway build root, so turn the download sandbox off.
grep -q '^DisableSandbox' /etc/pacman.conf ||
  sed -i '/^\[options\]/a DisableSandbox' /etc/pacman.conf
pacman -Syu --noconfirm --needed base-devel sudo xorg-server-xvfb dbus >/dev/null
id build >/dev/null 2>&1 || useradd -m build
echo 'build ALL=(ALL) NOPASSWD: ALL' > /etc/sudoers.d/build

work=/home/build/pkg
rm -rf "$work" && mkdir -p "$work"
tar -xzf "$archive" -C "$work" "groundhog-${version}/packaging/archlinux/groundhog/PKGBUILD"
mv "$work/groundhog-${version}/packaging/archlinux/groundhog/PKGBUILD" "$work/"
rm -rf "$work/groundhog-${version}"
cp "$archive" "$work/"
# The PKGBUILD names the archive by its pinned release; a branch build
# (workflow_dispatch) carries a 0.0.0-<sha> version instead. Point the
# PKGBUILD at the archive that is actually here so makepkg never downloads.
sed -i "s/^_version=.*/_version=${version}/" "$work/PKGBUILD"
chown -R build "$work"
# Arch Linux ARM's makepkg.conf defaults to .pkg.tar.xz; ship .zst on both.
su build -c "cd $work && PKGDEST=$work PKGEXT=.pkg.tar.zst makepkg -s --noconfirm"
mkdir -p "$out"
cp "$work"/*.pkg.tar.zst "$out/"
