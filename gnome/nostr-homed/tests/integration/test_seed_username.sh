#!/usr/bin/env bash
# The installed seeder must reject a non-namespaced username before it
# creates authority.db; the older generic "seed: enroll" line was mistaken
# for a success message in the live smoke lab.
set -euo pipefail
seed="${NH_SEED_AUTHORITY:?NH_SEED_AUTHORITY must name the built seeder}"
dir="$(mktemp -d)"
trap 'rm -rf "$dir"' EXIT
rc=0
"$seed" "$dir" testuser smoke-test-passphrase --no-fetch-profile >"$dir/output" 2>&1 || rc=$?
if [ "$rc" -ne 2 ]; then
  echo "FAIL: invalid username exit=$rc (expected 2)" >&2
  cat "$dir/output" >&2
  exit 1
fi
if ! grep -q "seed: ERROR: invalid username 'testuser': expected n_" "$dir/output"; then
  echo 'FAIL: no actionable username diagnostic' >&2
  cat "$dir/output" >&2
  exit 1
fi
if [ -e "$dir/authority.db" ]; then
  echo 'FAIL: invalid username created authority.db' >&2
  exit 1
fi
echo 'PASS: seeder rejects unprefixed name before state creation'
