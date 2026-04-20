# UAT bead drop-folder

Issues filed by Claude during user-acceptance testing land here as one
markdown file per bug, ready to be fed into `bd`.

## File naming

`NNNN-<slug>.md` — four-digit counter starting at 0001, then a short
kebab-case slug. Example: `0003-composer-loses-draft-on-window-resize.md`.

## Format

```
---
title: Short one-line summary (imperative)
issue_type: bug            # bug | task | feature | question
priority: 2                # 0=critical, 1=high, 2=normal, 3=low
area: <area>               # e.g. relays, composer, profiles, timeline, settings
labels: [ui, crash]        # optional list
tester: claude-uat
session: 2026-04-18
build: build/apps/gnostr/gnostr (Mach-O arm64)
---

## Summary
One- or two-sentence restatement of the problem.

## Steps to reproduce
1. ...
2. ...

## Expected
What a reasonable end user would expect to happen.

## Actual
What actually happens. Include any visible error text verbatim.

## Screenshots
- screenshots/NNNN-01-<slug>.png  (captioned)

## Notes
Severity rationale, guesses at root cause, related beads, etc.
```

## Importing into `bd`

Each file maps cleanly onto `bd create` flags:

- `title:` from frontmatter → positional argument (or `--title`)
- `issue_type:` → `-t` / `--type`  (bug / feature / task / chore / …)
- `priority:` → `-p` / `--priority`  (0–4, 0 highest; matches what's
  already in the frontmatter)
- `labels:` + `area:` → `-l` / `--labels` (comma-separated)
- everything below the YAML frontmatter → description, piped to
  `--body-file -`

A minimal shell importer that uses only flags present in
`bd create --help`:

```bash
set -euo pipefail
for f in beads/incoming/[0-9]*.md; do
  title=$(sed -n 's/^title: //p' "$f" | head -1)
  type=$(sed -n 's/^issue_type: //p'   "$f" | head -1)
  prio=$(sed -n 's/^priority: //p'     "$f" | head -1)
  area=$(sed -n 's/^area: //p'         "$f" | head -1)
  # YAML list "[ui, layout]" → "ui,layout"
  labels=$(sed -n 's/^labels: //p' "$f" | head -1 \
           | tr -d '[] ' )
  # Combine area + labels (skip empties)
  all_labels=$(printf '%s\n' "area:$area" "$labels" \
               | grep -v '^area:$' | paste -sd, -)
  # Drop the YAML frontmatter for the body
  awk 'BEGIN{n=0} /^---$/{n++; next} n>=2{print}' "$f" \
    | bd create "$title" \
        --type     "$type" \
        --priority "$prio" \
        --labels   "$all_labels" \
        --body-file -
done
```

Add `--dry-run` while verifying. If you'd rather hand bd one big
multi-issue file, `bd create -f <file>` exists for that — I haven't
checked its expected shape, so I'd try `--dry-run` first with one of
these files to confirm before looping.

Claude won't run this — you do, on your Mac where `bd` lives.

## Screenshots

Stored in `screenshots/` alongside this README so relative paths in the
frontmatter resolve correctly. Claude saves each one via the
computer-use `screenshot` tool with `save_to_disk: true` and renames/moves
it into `screenshots/` before referencing it from a bead.
