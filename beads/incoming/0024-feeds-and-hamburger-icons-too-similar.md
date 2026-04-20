---
title: "Feeds" icon and Hamburger menu icon are visually too similar
issue_type: bug
priority: 3
area: chrome
labels: [cosmetic, ui, icons]
tester: claude-uat
session: 2026-04-19 (second pass)
build: build/apps/gnostr/gnostr (rebuilt during session)
---

## Summary
The hamburger menu icon (three horizontal lines) and the Feeds button
icon (three horizontal lines with small bullets, looks like a list)
sit ~80px apart in the title bar. At normal viewing distance they
read as the same icon, so the user has to look closely to distinguish
which is the menu and which is the feeds switcher.

## Steps to reproduce
1. Launch gnostr.
2. Look at the title bar — hamburger is to the left of the gear,
   Feeds is on its own slightly to the right.

## Expected
Pick visually distinct icons for these two functions. For example:
- Hamburger → standard `view-more-symbolic` (three-dot or ⋯)
- Feeds → a stacked-cards or rss-like icon, not "rows"

Or merge them: if the hamburger always opens the same menu that
contains "Feeds", drop the Feeds button entirely.

## Actual
Both icons are stacks of horizontal lines. The bullets on the Feeds
icon are too small to register as a differentiator at default
window scale.

## Notes
- Combine with bead 0023 if doing one icon-language pass.
