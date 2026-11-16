#!/bin/sh
# The CMake configure arguments every fipsd package build shares: the daemon,
# its control/native-API libraries and nothing else from the monorepo. Printed
# one per line so debian/rules, the RPM spec, the PKGBUILD and the macOS
# builder read the same list (`$(sh fipsd/packaging/common/cmake-args.sh)`).
#
# This is .github/workflows/fips-ci.yml's configure line minus the sanitizer
# and warning flags, plus BUILD_TESTING=OFF and an explicit
# BUILD_SHARED_LIBS=OFF: Fedora's %cmake macro turns it on, and a daemon
# linked against shared libnostr/libfips-core/libgo would need packages that
# do not exist. (libnostr-json is shared regardless; the daemon carries a
# private copy, see fipsd/CMakeLists.txt.) libnostr keeps its GLib layer
# (the default): the daemon's Nostr rendezvous event code uses GLib, so the
# packages depend on libglib2.0 -- but on no GObject wrapper, GTK or libsoup.
# fips-gateway is off until the packages carry it (its binary, unit and
# libnftables dependency): otherwise a build host that happens to have
# libnftables would ship it through debian/fipsd.install's usr/bin/fips*.
set -eu
cat <<'ARGS'
-DBUILD_FIPS=ON
-DBUILD_APPS=OFF
-DBUILD_RELAYD=OFF
-DBUILD_LIBHANAMI=OFF
-DBUILD_LIBMARMOT=OFF
-DBUILD_MARMOT_GOBJECT=OFF
-DBUILD_NOSTR_GOBJECT=OFF
-DBUILD_NOSTR_GTK=OFF
-DSIGNET_ENABLE=OFF
-DWITH_NOSTRDB=OFF
-DLIBNOSTR_WITH_NOSTRDB=OFF
-DBUILD_TESTING=OFF
-DBUILD_TESTING_FRAMEWORK=OFF
-DBUILD_SHARED_LIBS=OFF
-DFIPSD_BUILD_GATEWAY=OFF
ARGS
