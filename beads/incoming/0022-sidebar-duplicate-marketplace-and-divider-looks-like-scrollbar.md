---
title: Sidebar has duplicate "Marketplace" entries and a section divider that mimics a scrollbar thumb
issue_type: bug
priority: 2
area: navigation, chrome
labels: [cosmetic, ui, sidebar, visual-confusion]
tester: claude-uat
session: 2026-04-18 (second pass)
build: build/apps/gnostr/gnostr (rebuilt during session)
---

## Summary
Two cosmetic/IA problems compound each other in the left sidebar:

1. **Duplicate "Marketplace" item.** Marketplace appears twice in
   the list, once in the upper section and once in the lower. Same
   label, same icon, same apparent destination.

2. **Section divider is visually identical to a scrollbar thumb.**
   Between the upper group (Timeline, Notifications, Messages,
   Discover, Search, Marketplace, Git Repos, Events) and the lower
   group (Marketplace, Group Chats, Invitations, Secure DMs, Chess)
   there is a gray, pill-shaped rectangle of exactly the
   proportions a GTK scrollbar thumb would occupy. It reads as
   "there's more content off-screen — drag me to reveal it" rather
   than "new section starts here".

Together they make the sidebar feel like a broken scroll region
with a duplicated entry, when actually it's two sections with
overlapping items.

## Steps to reproduce
1. Launch gnostr.
2. Look at the left sidebar from top to bottom.

## Expected
- The two Marketplace entries consolidate to one (and/or the lower
  one renames to whatever actually differs — e.g. "My Listings"
  vs. "Marketplace").
- Section boundary uses a real divider treatment: a thin hairline
  rule, a section label ("Plugins", "More", etc.), extra vertical
  whitespace, or all three — none of which can be mistaken for a
  scrollbar thumb.
- Consider: Messages (upper) and Secure DMs (lower) overlap
  conceptually; worth deciding whether they're the same
  destination or genuinely different and labeling accordingly.

## Actual
- Marketplace × 2.
- Gray pill between sections that the eye reads as a scrollbar.

## Notes
- When the selected item is in the lower section, items like
  Timeline / Notifications / Messages scroll above the visible
  sidebar area. There IS real overflow in the sidebar on top of
  this visual noise, which makes the fake-thumb all the more
  misleading.
