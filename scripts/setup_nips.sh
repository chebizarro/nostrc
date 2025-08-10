# scripts/setup_nips.sh
#!/usr/bin/env bash
set -euo pipefail

REPO_ROOT="$(git rev-parse --show-toplevel)"
cd "$REPO_ROOT"

mkdir -p docs

# Add submodule (shallow + blobless keeps it light)
git submodule add \
  -b master \
  --depth 1 \
  https://github.com/nostr-protocol/nips.git docs/nips

# Optional: tighten submodule config
git config -f .gitmodules submodule.docs/nips.shallow true
git config -f .gitmodules submodule.docs/nips.ignore dirty
git add .gitmodules docs/nips
git commit -m "docs: add nostr-protocol/nips as submodule under docs/nips"

echo "✅ NIPs submodule added at docs/nips (shallow)."
