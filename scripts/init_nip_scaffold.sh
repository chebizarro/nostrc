# scripts/init_nip_scaffold.sh
#!/usr/bin/env bash
set -euo pipefail
NIP_NUM="${1:?Usage: $0 <nip-number like 04>}"
REPO_ROOT="$(git rev-parse --show-toplevel)"

CODE_DIR="$REPO_ROOT/nips/nip${NIP_NUM}"
DOC_FILE="$REPO_ROOT/docs/nips/${NIP_NUM}.md"

mkdir -p "$CODE_DIR"

cat > "$CODE_DIR/SPEC_SOURCE" <<EOF
# Do not delete. Used by IDE-LLM and tooling.
SPEC_MD=../../docs/nips/${NIP_NUM}.md
EOF

echo "✅ Scaffolding for NIP-${NIP_NUM} initialized at nips/nip${NIP_NUM}/"
