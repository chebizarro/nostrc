#!/bin/bash
# Import UAT beads from beads/incoming/ into bd.
#
# Usage:
#   ./import-beads.sh                  # dry-run, all files
#   ./import-beads.sh --for-real       # actually create
#   ./import-beads.sh 0001             # dry-run, just bead 0001
#   ./import-beads.sh --for-real 0003  # create just bead 0003
#
# No `set -e` on purpose — we want to see every bead attempt even if
# one fails.

DRY_RUN="--dry-run"
FILTER=""

for arg in "$@"; do
  case "$arg" in
    --for-real) DRY_RUN="" ;;
    *)          FILTER="$arg" ;;
  esac
done

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TMP_BODY="$(mktemp -t bd-body.XXXXXX)"
trap 'rm -f "$TMP_BODY"' EXIT

pattern="$SCRIPT_DIR/[0-9]*.md"
if [ -n "$FILTER" ]; then
  pattern="$SCRIPT_DIR/${FILTER}*.md"
fi

for f in $pattern; do
  [ -f "$f" ] || continue
  echo "----- $(basename "$f") -----"

  title=$(sed -n 's/^title: //p'       "$f" | head -1)
  type=$(sed  -n 's/^issue_type: //p'  "$f" | head -1)
  prio=$(sed  -n 's/^priority: //p'    "$f" | head -1)
  area=$(sed  -n 's/^area: //p'        "$f" | head -1)
  raw_labels=$(sed -n 's/^labels: //p' "$f" | head -1 | tr -d '[] ')

  # Build comma-separated labels; skip empties.
  label_list=""
  [ -n "$area"       ] && label_list="area:$area"
  if [ -n "$raw_labels" ]; then
    [ -n "$label_list" ] && label_list="$label_list,"
    label_list="$label_list$raw_labels"
  fi

  # Extract body (everything after the closing --- of the frontmatter).
  awk 'BEGIN{n=0} /^---$/ && n<2 {n++; next} n>=2 {print}' "$f" > "$TMP_BODY"

  # Show what we're about to do.
  printf '  title:    %s\n'   "$title"
  printf '  type:     %s\n'   "$type"
  printf '  priority: %s\n'   "$prio"
  printf '  labels:   %s\n'   "$label_list"
  printf '  body:     %d bytes\n' "$(wc -c < "$TMP_BODY")"

  bd create "$title" \
    --type     "$type" \
    --priority "$prio" \
    --labels   "$label_list" \
    --body-file "$TMP_BODY" \
    $DRY_RUN
  echo
done
