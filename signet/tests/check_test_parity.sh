#!/bin/sh
# SPDX-License-Identifier: MIT
#
# fp-52iq guard: fail the suite if the CMake and meson test lists diverge.
#
# CMake is the shipped build configuration. Six tests plus a shell test once
# existed only in meson.build, so CMake CI covered strictly less than the
# developer build while still reporting green -- the same class of gap as
# fp-3126, where tests that never executed looked like passing tests.
#
# Any deliberate divergence must be listed in CMAKE_ONLY/MESON_ONLY below with
# a reason, which forces the omission to be an explicit decision rather than an
# accident of two hand-maintained lists.
#
# Usage: check_test_parity.sh <signet/tests source dir>

set -eu

dir=${1:-$(dirname "$0")}
cmake_file="$dir/CMakeLists.txt"
meson_file="$dir/meson.build"

for f in "$cmake_file" "$meson_file"; do
  if [ ! -f "$f" ]; then
    echo "check_test_parity: missing $f" >&2
    exit 1
  fi
done

# Deliberate divergences. Keep the reason with the entry.
#   live_dbus_interop_manual: needs a live session bus and real hardware; it is
#   registered DISABLED for manual invocation only, so it has no meson twin.
CMAKE_ONLY="live_dbus_interop_manual"
MESON_ONLY=""

tmp=$(mktemp -d "${TMPDIR:-/tmp}/signet-test-parity.XXXXXX")
trap 'rm -rf "$tmp"' EXIT INT TERM

# add_test(NAME <name> ...)
sed -n 's/^[[:space:]]*add_test(NAME[[:space:]]\{1,\}\([A-Za-z0-9_]\{1,\}\).*/\1/p' \
  "$cmake_file" | sort -u >"$tmp/cmake"

# test('<name>', ...)
sed -n "s/^[[:space:]]*test('\([A-Za-z0-9_]\{1,\}\)'.*/\1/p" \
  "$meson_file" | sort -u >"$tmp/meson"

if [ ! -s "$tmp/cmake" ] || [ ! -s "$tmp/meson" ]; then
  echo "check_test_parity: parsed an empty test list -- the extraction patterns" >&2
  echo "no longer match the build files, so this guard is not actually checking" >&2
  echo "anything. Fix the patterns in $0." >&2
  exit 1
fi

printf '%s\n' $CMAKE_ONLY | sed '/^$/d' | sort -u >"$tmp/cmake_only_ok"
printf '%s\n' $MESON_ONLY | sed '/^$/d' | sort -u >"$tmp/meson_only_ok"

comm -23 "$tmp/cmake" "$tmp/meson" | comm -23 - "$tmp/cmake_only_ok" >"$tmp/cmake_extra"
comm -13 "$tmp/cmake" "$tmp/meson" | comm -23 - "$tmp/meson_only_ok" >"$tmp/meson_extra"

status=0
if [ -s "$tmp/cmake_extra" ]; then
  echo "ERROR: registered in CMakeLists.txt but not meson.build:" >&2
  sed 's/^/  - /' "$tmp/cmake_extra" >&2
  status=1
fi
if [ -s "$tmp/meson_extra" ]; then
  echo "ERROR: registered in meson.build but not CMakeLists.txt:" >&2
  sed 's/^/  - /' "$tmp/meson_extra" >&2
  echo "CMake is the shipped build; a meson-only test provides no release assurance." >&2
  status=1
fi

if [ "$status" -ne 0 ]; then
  echo "" >&2
  echo "Add the test to both build files, or add it to CMAKE_ONLY/MESON_ONLY in" >&2
  echo "$0 with a reason." >&2
  exit 1
fi

echo "signet test list parity: PASS ($(wc -l <"$tmp/cmake" | tr -d ' ') CMake, $(wc -l <"$tmp/meson" | tr -d ' ') meson)"
