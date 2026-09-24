#!/usr/bin/env bash
# test_syncd_off_closure.sh — nm-based OFF-closure verification.
#
# Called with $1 = path to the nostr-authd executable. Asserts that
# no nh_syncd_cache_* or nostr-home-syncd symbols are linked into the
# broker. When SYNCD_EXPERIMENTAL is OFF the cache library is not
# compiled at all, so the check is trivially satisfied. When SYNCD is
# ON we still refuse to link the daemon-scope library into
# nostr-authd because SYNCD pulls in libnostr / libhanami.
#
# SPDX-License-Identifier: MIT
set -euo pipefail

BIN="${1:-}"
if [[ -z "${BIN}" || ! -x "${BIN}" ]]; then
    echo "usage: $0 /path/to/nostr-authd" >&2
    exit 2
fi

if ! command -v nm >/dev/null 2>&1; then
    echo "SKIP: nm not available" >&2
    exit 77
fi

# Dynamic + local symbols. `-D` alone would miss statically linked
# archive members.
if nm -a "${BIN}" 2>/dev/null \
   | awk '{print $NF}' \
   | grep -E '(nh_syncd_cache_|nostr_home_syncd|nh_syncd_sweep_|nh_syncd_pin_ring_|nh_fuse_)' \
   >/tmp/off-closure-$$; then
    echo "FAIL: nostr-authd contains SYNCD or FUSE symbols:" >&2
    cat /tmp/off-closure-$$ >&2
    rm -f /tmp/off-closure-$$
    exit 1
fi

# libfuse3 must never appear in the broker's ldd closure — Phase 4
# P4-I ships nostr-home-fuse as a SEPARATE binary and the
# NOSTR_HOMED_ENABLE_PORTHOME_FUSE_EXPERIMENTAL gate must keep
# libfuse3 out of pam_nostr / nostr-authd even when it is ON.
if command -v ldd >/dev/null 2>&1; then
    if ldd "${BIN}" 2>/dev/null | grep -qi 'libfuse'; then
        echo "FAIL: nostr-authd links libfuse:" >&2
        ldd "${BIN}" | grep -i libfuse >&2
        exit 1
    fi
fi

rm -f /tmp/off-closure-$$
echo "ok"
