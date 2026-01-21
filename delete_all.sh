#!/usr/bin/env bash
set -euo pipefail
REPO="$(gh repo view --json nameWithOwner -q .nameWithOwner)"

# Fetch all run IDs
gh api "/repos/$REPO/actions/runs" --paginate -q '.workflow_runs[].id' \
| xargs -n1 -I % gh api "/repos/$REPO/actions/runs/%" -X DELETE --silent

echo "All workflow runs deleted in $REPO"
