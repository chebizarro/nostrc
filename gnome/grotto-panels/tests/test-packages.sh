#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Explicit disposable-Linux package smoke, never a developer-host install.
set -euo pipefail
[[ $(uname -s) == Linux && $(id -u) == 0 && ${GROTTO_PACKAGE_TEST_DISPOSABLE:-} == 1 ]] || {
  echo 'Run only as root inside a disposable Linux container with GROTTO_PACKAGE_TEST_DISPOSABLE=1' >&2; exit 1;
}
for tool in dpkg-buildpackage dh dpkg-architecture cmake ninja cc pkg-config rpmbuild rpm readelf; do
  command -v "$tool" >/dev/null || { echo "Missing required package-test tool: $tool" >&2; exit 1; }
done
root=$(cd "$(dirname "$0")/../../.." && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
source=$work/grotto-panels-0.2.0
mkdir -p "$source/gnome" "$source/cmake"
cp -R "$root/gnome/grotto-panels" "$source/gnome/"
cp "$root/cmake/VersionHelpers.cmake" "$source/cmake/"
cp -R "$source/gnome/grotto-panels/packaging/debian" "$source/debian"
chmod +x "$source/debian/rules"
(cd "$source" && dpkg-buildpackage -b -us -uc)
runtime=$(find "$work" -maxdepth 1 -name 'libgrotto-panels-0-0_*.deb' -print -quit)
devel=$(find "$work" -maxdepth 1 -name 'libgrotto-panels-0-dev_*.deb' -print -quit)
[[ -n $runtime && -n $devel ]]
dpkg-deb --field "$runtime" Depends
dpkg-deb --field "$devel" Depends
dpkg --install "$runtime" "$devel"
/sbin/ldconfig
mkdir "$work/consumer"
cat > "$work/consumer/main.c" <<'C'
#include <grotto-panels.h>
int main(void) {
  GrottoPanelsMetadata m = {.id = "public-id", .label = "Installed consumer"};
  GError *error = NULL;
  GrottoPanelsItem *item = grotto_panels_item_new(&m, &error);
  if (!item || error) return 1;
  g_object_unref(item);
  return 0;
}
C
cat > "$work/consumer/CMakeLists.txt" <<'CMAKE'
cmake_minimum_required(VERSION 3.22)
project(installed_consumer C)
find_package(GrottoPanels 0.1 REQUIRED CONFIG)
add_executable(installed-consumer main.c)
target_link_libraries(installed-consumer PRIVATE GrottoPanels::Panels)
CMAKE
check_consumer() {
  [[ $(pkg-config --modversion grotto-panels-0) == 0.2.0 ]]
  [[ -e $(pkg-config --variable=libdir grotto-panels-0)/libgrotto-panels-0.so.0 ]]
  # Intentional shell splitting of pkg-config's compiler/linker flags.
  cc "$work/consumer/main.c" $(pkg-config --cflags --libs grotto-panels-0) -o "$work/consumer/pc-consumer"
  readelf -d "$work/consumer/pc-consumer" | grep -F 'libgrotto-panels-0.so.0'
  env -u LD_LIBRARY_PATH "$work/consumer/pc-consumer"
  cmake -S "$work/consumer" -B "$work/consumer/build-$1" -G Ninja
  cmake --build "$work/consumer/build-$1"
  env -u LD_LIBRARY_PATH "$work/consumer/build-$1/installed-consumer"
}
check_consumer deb
# Same RPM spec/payload, native compiler/ELF on Ubuntu. --nodeps is necessary
# because installed development dependencies are registered in dpkg, NOT RPM.
# This is not Fedora dependency-solver or native distro acceptance.
dpkg --purge libgrotto-panels-0-dev libgrotto-panels-0-0
rpmroot=$work/rpm
mkdir -p "$rpmroot/SOURCES" "$rpmroot/SPECS"
tar -czf "$rpmroot/SOURCES/grotto-panels-0.2.0.tar.gz" -C "$work" \
  grotto-panels-0.2.0/gnome grotto-panels-0.2.0/cmake
cp "$root/gnome/grotto-panels/packaging/rpm/grotto-panels.spec" "$rpmroot/SPECS/"
libdir=/usr/lib/$(dpkg-architecture -qDEB_HOST_MULTIARCH)
rpmbuild -bb --nodeps --define "_topdir $rpmroot" --define "_libdir $libdir" "$rpmroot/SPECS/grotto-panels.spec"
runtime=$(find "$rpmroot/RPMS" -name 'grotto-panels-0.2.0-*.rpm' -print -quit)
devel=$(find "$rpmroot/RPMS" -name 'grotto-panels-devel-0.2.0-*.rpm' -print -quit)
[[ -n $runtime && -n $devel ]]
rpm -qp --provides "$runtime" | grep -F 'libgrotto-panels-0.so.0'
rpm -qp --provides "$devel" | grep -F 'pkgconfig(grotto-panels-0) = 0.2.0'
rpm -qp --requires "$devel"
rpm --install --nodeps "$runtime" "$devel"
/sbin/ldconfig
check_consumer rpm
printf '%s\n' 'PASS: native Debian install + RPM payload diagnostic; pkg-config and CMake consumers link and run without LD_LIBRARY_PATH'
