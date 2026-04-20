---
title: Spacing rhythm inconsistencies across chrome, sidebar, and content
issue_type: bug
priority: 3
area: chrome, layout
labels: [cosmetic, spacing, rhythm]
tester: claude-uat
session: 2026-04-19 (second pass)
build: build/apps/gnostr/gnostr (rebuilt during session)
---

## Summary
Several places where vertical/horizontal spacing breaks the visual
rhythm or feels arbitrary:

1. **Title-bar height vs filter-bar height.** The title bar is
   ~35px tall, but the inline "Search timeline…" filter bar that
   appears below it (when the hamburger is toggled) is ~40px tall.
   Two horizontal strips of slightly different heights stacked on
   top of each other look unintentional.

2. **Notification row whitespace.** A notification row is ~40px
   tall but the visible content occupies the leftmost ~600px
   (avatar + "npub… started following you") with the trailing
   ~500px completely blank before the right-aligned "1d"
   timestamp. The empty middle isn't being used and isn't an
   obvious affordance for hover/click.

3. **Sidebar item left padding.** The icon glyph and the label
   inside each sidebar row appear to have inconsistent gap
   between them — chess piece and "Chess" sit closer together
   than the search magnifier and "Search". Suggests icons are
   being rendered at slightly different intrinsic widths and not
   normalised before the gap.

4. **Compose button padding.** The compose button square has
   tighter internal padding than the avatar circle to its right
   (the white glyph nearly touches the edge). They visually want
   to feel paired but the breathing room is uneven.

5. **Filter pill row gap on Discover.** People / Live / Articles
   pills sit very close to each other (~6–8px gap) while Local /
   Network pills below them sit further apart (~12px).

## Steps to reproduce
1. Walk through Notifications, Discover, and the title bar at
   default window width.
2. Look for inconsistent gaps as listed above.

## Expected
Pick a spacing scale (e.g. 4 / 8 / 16 / 24px) and apply it
consistently. Equalise the title bar and filter bar heights, give
the notification row a meaningful midsection (e.g. preview text)
or shrink it, and normalise sidebar icon column width.

## Actual
Each surface has its own vertical / horizontal rhythm.

## Notes
- The biggest visible win would be normalising the sidebar icon
  column — even a 16px square frame for every sidebar icon would
  immediately tighten the whole rail.
