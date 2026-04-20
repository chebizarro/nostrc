---
title: Pale-blue pill is overloaded — used for tabs, filter chips, hashtags, and badges interchangeably
issue_type: bug
priority: 2
area: chrome, theming
labels: [cosmetic, color, hierarchy]
tester: claude-uat
session: 2026-04-19 (second pass)
build: build/apps/gnostr/gnostr (rebuilt during session)
---

## Summary
The same washed-out pale-blue pill style is reused for at least four
semantically different controls:

- **Tabs** — Following (vs. selected Global which is solid deep blue)
- **Sub-filters** — Local / Network on Discover; Upcoming / Now / Past
  on Events
- **Category toggles** — People / Live / Articles on Discover
- **Trending hashtags** — `#ai` chip in the Trending Hashtags row
- **Status badge** — `6/7` next to Feeds in the title bar

Visually they're indistinguishable. A user sees a pale blue pill and
can't tell whether it's a tab, a filter, a hashtag, or a status
indicator — semantic meaning is being asked of a single visual style.

## Steps to reproduce
1. Open Discover.
2. Compare the People / Live / Articles row, the Local / Network row,
   the #ai chip in Trending Hashtags, and the Following tab on
   Timeline.

## Expected
Differentiate by treatment, not just by location:

- **Selected vs unselected tabs** — already differentiated (deep blue
  vs pale). Keep that pattern, but use it consistently across all
  tab-like rows.
- **Sub-filters / toggles** — make them obviously toggleable (e.g.
  with a chevron, or use a tinted border instead of fill).
- **Hashtags** — distinct chip style (e.g. monospaced, leading `#`
  inset, non-blue accent).
- **Status badges** — neutral grey or amber/red when state is bad,
  not pale blue (pale blue currently makes 6/7 read as a button).

## Actual
Same fill, same radius, same color → user infers no semantic
difference.

## Notes
- Audit alongside bead 0023 (button treatments) for a unified pass.
