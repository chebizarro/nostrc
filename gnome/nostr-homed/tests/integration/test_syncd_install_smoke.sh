#!/usr/bin/env bash
# test_syncd_install_smoke.sh — cmake --install into a scratch DESTDIR
# and assert the SYNCD binary + user unit land at the expected
# relative paths.
#
# Reads (from env, set by CMake):
#   CMAKE_BINARY_DIR         — build tree
#   CMAKE_INSTALL_PREFIX     — configured install prefix
#   NH_SYNCD_USER_UNIT_DIR   — resolved systemd user unit dir
#
# SPDX-License-Identifier: MIT
set -euo pipefail

: "${CMAKE_BINARY_DIR:?missing}"
: "${CMAKE_INSTALL_PREFIX:?missing}"
: "${NH_SYNCD_USER_UNIT_DIR:?missing}"

STAGE="$(mktemp -d -t nh_syncd_install_XXXXXX)"
trap 'rm -rf "${STAGE}"' EXIT

# Install only nostr-homed's subtree so we don't depend on unrelated
# targets (relayd, apps, …) being fully built. cmake --install of the
# subdirectory runs cmake_install.cmake in that dir only.
cd "${CMAKE_BINARY_DIR}"
DESTDIR="${STAGE}" cmake --install gnome/nostr-homed >/dev/null

# Reconstruct DESTDIR paths.
strip_leading_slash() { printf '%s' "${1#/}"; }

BIN_REL="$(strip_leading_slash "${CMAKE_INSTALL_PREFIX}/bin/nostr-home-syncd")"
UNIT_REL="$(strip_leading_slash "${NH_SYNCD_USER_UNIT_DIR}/nostr-home-sync.service")"

BIN="${STAGE}/${BIN_REL}"
UNIT="${STAGE}/${UNIT_REL}"

fail=0
if [[ ! -x "${BIN}" ]]; then
    echo "FAIL: expected daemon binary at ${BIN}" >&2
    fail=1
fi
if [[ ! -f "${UNIT}" ]]; then
    echo "FAIL: expected systemd user unit at ${UNIT}" >&2
    fail=1
fi

# Sanity-check the unit substitution.
if [[ -f "${UNIT}" ]]; then
    if grep -q '@NH_SYNCD_BIN_PATH@' "${UNIT}"; then
        echo "FAIL: unit still contains unsubstituted @NH_SYNCD_BIN_PATH@" >&2
        fail=1
    fi
    if ! grep -q 'nostr-home-syncd' "${UNIT}"; then
        echo "FAIL: unit does not reference nostr-home-syncd" >&2
        fail=1
    fi
    if ! grep -q '^WantedBy=default.target' "${UNIT}"; then
        echo "FAIL: unit missing WantedBy=default.target" >&2
        fail=1
    fi
fi

if [[ ${fail} -ne 0 ]]; then
    echo "--- staged tree ---"
    find "${STAGE}" -maxdepth 6 -print
    exit 1
fi
echo "ok"
