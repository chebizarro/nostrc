---
title: Title-bar icon row mixes three different button treatments
issue_type: bug
priority: 3
area: chrome
labels: [cosmetic, ui, icons, consistency]
tester: claude-uat
session: 2026-04-19 (second pass)
build: build/apps/gnostr/gnostr (rebuilt during session)
---

## Summary
Reading the title bar left-to-right past the traffic lights, the
controls use three visually-different button treatments side by side:

| Element       | Treatment                                       |
|---------------|-------------------------------------------------|
| Gear ⚙        | Flat icon, no fill, no border                   |
| Hamburger ≡   | Flat icon, no fill, no border                   |
| Feeds + 6/7   | Icon + **text label** + secondary badge         |
| Search 🔍     | Flat icon, no fill, no border                   |
| Compose       | **Filled blue square** with white glyph inside  |
| Avatar        | Filled circle (image)                           |

The compose button is the only filled, colored, square-shaped chip in
the row. The Feeds button is the only one that has visible text. They
all live in the same horizontal strip, so the inconsistency reads as
"this app's chrome wasn't art-directed".

## Steps to reproduce
1. Launch gnostr.
2. Look at the title bar.

## Expected
Pick one rule per "tier" of importance and apply it consistently:
- Primary action (compose) → solid color fill, but match the
  corner radius of the avatar (circle or generous squircle, not a
  hard-edged square).
- Secondary actions (gear, hamburger, search, Feeds) → flat icon
  with optional hover/pressed state. Drop the "Feeds" text label
  or move it into a different element entirely.
- The 6/7 badge belongs on a status surface (a relay-status pill),
  not bolted onto the Feeds button.

## Actual
- Compose button is a filled blue **square** sitting next to a
  circular avatar.
- Feeds is the only labeled chip.
- Gear / hamburger / search look correctly understated; compose
  alone breaks the pattern.

## Notes
- Hamburger ≡ and Feeds icon are similar enough at this size that
  the user may need to look twice to tell them apart. See bead 0024
  for the related Feeds-vs-Hamburger overlap.
