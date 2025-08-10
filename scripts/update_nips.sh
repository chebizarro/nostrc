# scripts/update_nips.sh
#!/usr/bin/env bash
set -euo pipefail

REPO_ROOT="$(git rev-parse --show-toplevel)"
cd "$REPO_ROOT/docs/nips"

git fetch --depth=1 origin master
git checkout --detach origin/master
cd "$REPO_ROOT"
git add docs/nips
echo "✅ NIPs updated to latest master (detached). Commit the new submodule ref when ready."
