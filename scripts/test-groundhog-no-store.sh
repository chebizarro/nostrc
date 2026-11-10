#!/usr/bin/env bash
# Compile the real optional-store application and run all tests registered for it.
# Linux callers supply a private bus/display: dbus-run-session -- xvfb-run -a ...
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
build=${1:?Usage: test-groundhog-no-store.sh BUILD_DIR [extra CMake arguments...]}
shift
export REAL_PKG_CONFIG=${REAL_PKG_CONFIG:-$(command -v pkg-config)}
cmake -S "$root" -B "$build" -DCMAKE_BUILD_TYPE=Debug \
  -DBUILD_TESTING=ON -DBUILD_GROUNDHOG=ON \
  -DBUILD_NOSTR_GOBJECT=ON -DBUILD_NOSTR_GTK=ON \
  -DBUILD_APPS=OFF -DBUILD_RELAYD=OFF -DSIGNET_ENABLE=OFF \
  -DNIP55L_SECRET_BACKEND=libsecret \
  -DGROUNDHOG_STORE_KEY_DEFAULT_BACKEND=secret-service \
  -DPKG_CONFIG_EXECUTABLE="$root/gnome/groundhog/tests/no-sqlcipher-pkg-config" "$@"
# Reject reuse of a store-enabled cache rather than accidentally testing it.
ctest --test-dir "$build/gnome/groundhog" --show-only=json-v1 | python3 -c '
import json, sys
names = {test["name"] for test in json.load(sys.stdin)["tests"]}
forbidden = names & {"groundhog-store", "groundhog-account-store", "groundhog-social-lists"}
if forbidden:
    sys.exit("Not a no-store configuration: " + ", ".join(sorted(forbidden)))
'
cmake --build "$build" --parallel "${CMAKE_BUILD_PARALLEL_LEVEL:-2}" \
  --target groundhog groundhog-test-build
# Missing optional Linux test tools must fail CI, not turn into green skips.
# Native macOS has explicitly platform-skipped keyring tests; report those.
ctest --test-dir "$build/gnome/groundhog" --output-on-failure --no-tests=error \
  --parallel 1 | tee "$build/groundhog-no-store-ctest.log"
if [[ ${GROUNDHOG_NO_STORE_REQUIRE_NO_SKIPS:-0} == 1 ]] && \
    grep -Eq '\*\*\*Skipped|Not Run' "$build/groundhog-no-store-ctest.log"; then
  echo 'Optional-store tests unexpectedly skipped or did not run' >&2
  exit 1
fi
