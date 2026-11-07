#!/usr/bin/env bash
# Schema-only check: public test inputs, no credentials or cloud mutations.
set -euo pipefail
here=$(cd -- "$(dirname -- "$0")" && pwd)
tmpdir=$(mktemp -d)
trap 'rm -rf -- "$tmpdir"' EXIT

# Throwaway public key; its private key was discarded, not a deployment key.
cat > "$tmpdir/operator.pub" <<'KEY'
ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIPxWb/lqabObET7dtG2Tqn5JYuQZ2TySUZa+g7QKrI0l signet-cloud-init-schema
KEY

cloud-init schema --config-file "$here/cloud-init.yaml"
OPERATOR_SSH_PUBLIC_KEY_FILE="$tmpdir/operator.pub" \
  DO_VOLUME_NAME=signet-schema \
  DO_VOLUME_ID=00000000-0000-4000-8000-000000000001 \
  python3 "$here/render-cloud-init.py" > "$tmpdir/cloud-init.yaml"
cloud-init schema --config-file "$tmpdir/cloud-init.yaml"
