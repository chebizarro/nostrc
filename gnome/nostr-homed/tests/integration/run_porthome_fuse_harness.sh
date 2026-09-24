#!/usr/bin/env bash
# run_porthome_fuse_harness.sh — Phase 4 P4-I (bead nostrc-1u55).
#
# Skips gracefully when the FUSE binary is not built. Otherwise runs
# the binary in NH_FUSE_TEST_HARNESS=1 mode which exercises the
# snapshot loader + key loader + source seam WITHOUT calling
# fuse_mount. This is the CI-friendly proof that the wiring compiles
# and runs; the real-mount integration cases (a..k) live under
# run_porthome_fuse_*.sh and SKIP when /dev/fuse or fusermount3 is
# absent.
#
# SPDX-License-Identifier: MIT
set -euo pipefail

BIN="${1:-}"
if [[ -z "${BIN}" || ! -x "${BIN}" ]]; then
    echo "SKIP: nostr-home-fuse binary not built"
    exit 0
fi
if ! command -v python3 >/dev/null 2>&1; then
    echo "SKIP: python3 not available (needed for fixture)"
    exit 0
fi

STATE_DIR="${NH_FUSE_STATE_DIR:-/tmp/nhfuse_harness_state_$$}"
MP="${NH_FUSE_MOUNTPOINT:-/tmp/nhfuse_harness_mp_$$}"
STATUS_DIR="${NH_FUSE_STATUS_DIR:-/tmp/nhfuse_harness_status_$$}"
rm -rf "${STATE_DIR}" "${MP}" "${STATUS_DIR}"
mkdir -p "${STATE_DIR}" "${MP}" "${STATUS_DIR}"

# Write a minimal snapshot.json. Two entries: a dir + a file whose
# chunk address is bogus (harness never dereferences chunks). Use
# fixed root_id_hex.
cat > "${STATE_DIR}/snapshot.json" <<'JSON'
{
  "schema": 1,
  "generation": 5,
  "root": "/home/testuser",
  "d_tag": "nostr-homed.home.v1:personal",
  "account_pubkey_hex": "",
  "root_id_hex": "cdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcd",
  "files": {
    "Documents":       {"kind":"dir","mode":493,"uid":0,"gid":0,"mtime_ns":1000000000,"size":0,"content_hash_hex":""},
    "Documents/hi.txt":{"kind":"file","mode":420,"uid":0,"gid":0,"mtime_ns":1000000000,"size":5,"content_hash_hex":"01","chunk_addrs_hex":["aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"]}
  }
}
JSON
echo 5 > "${STATE_DIR}/generation"

# Seed via env (avoids needing the broker drop file).
export NH_FUSE_SEED_HEX="${NH_FUSE_SEED_HEX:-1111111111111111111111111111111111111111111111111111111111111111}"
export NH_FUSE_STATE_DIR="${STATE_DIR}"
export NH_FUSE_MOUNTPOINT="${MP}"
export NH_FUSE_STATUS_DIR="${STATUS_DIR}"
export NH_FUSE_TEST_HARNESS=1

OUT="$("${BIN}")"
echo "harness output: ${OUT}"
grep -q '^READY generation=5 entries=2' <<< "${OUT}" || {
    echo "FAIL: harness did not report READY with expected shape"
    exit 1
}

# Status file must have been written.
[[ -f "${STATUS_DIR}/fuse-status.json" ]] || {
    echo "FAIL: fuse-status.json not written"
    exit 1
}
grep -q '"generation":5' "${STATUS_DIR}/fuse-status.json" || {
    echo "FAIL: status file missing generation"
    exit 1
}

# Cleanup unless the caller pinned dirs.
if [[ -z "${NH_FUSE_STATE_DIR:-}" || "${STATE_DIR}" == /tmp/nhfuse_harness_state_* ]]; then
    rm -rf "${STATE_DIR}" "${MP}" "${STATUS_DIR}"
fi

echo "run_porthome_fuse_harness OK"
