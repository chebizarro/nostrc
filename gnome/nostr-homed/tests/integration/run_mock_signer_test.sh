#!/usr/bin/env bash
set -euo pipefail

# Try common paths relative to CTest WORKING_DIRECTORY (usually CMAKE_BINARY_DIR)
CANDIDATES=(
  "./gnome/nostr-homed/test_secrets_decrypt"
  "./test_secrets_decrypt"
  "../gnome/nostr-homed/test_secrets_decrypt"
)

# Require explicit opt-in to run this test to avoid DBus/session env flakiness
if [ "${RUN_MOCK_SIGNER:-}" != "1" ]; then
  echo "RUN_MOCK_SIGNER not set; skipping secrets_decrypt_integration" >&2
  exit 77
fi

for exe in "${CANDIDATES[@]}"; do
  if [ -x "$exe" ]; then
    dir=$(dirname "$exe")
    base=$(basename "$exe")
    cd "$dir"
    exec "./$base"
  fi
done

echo "test_secrets_decrypt not found in expected locations; skipping" >&2
exit 77
